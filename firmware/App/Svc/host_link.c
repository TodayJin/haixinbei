/**
 * @file  host_link.c
 * @brief 上位机通信链路：帧编解码 + 参数表 + 命令分发 + 遥测打包。
 *
 * 为什么不用文本协议：100Hz × 64 字节遥测在 921600 下只占几个百分点，
 * 而文本要 3~5 倍带宽、丢一个字节就整行错乱。详见 docs/上位机通信协议.md。
 *
 * 数据来源（都是**真实读数**，不编）：
 *   yaw/pitch/roll   <- jy61p.c（USART3 收的 JY61P）
 *   pwm_l/pwm_r      <- TIM8 CH1..CH4 的 CCR（AT8236 两根输入的实际占空比）
 *   servo1/servo2    <- TIM1 CH1/CH2 的 CCR（实际脉宽 us）
 *   vbat/ibat        <- INA226（只在 OLED 不占 I2C1 时读，避免抢总线）
 *   line_mask        <- 8 路灰度原始电平（1 = 压线）
 *   state_id         <- 任务解释器在跑时的当前状态号（没在跑 = 0xFF，见 TELEM2_TASK_OFF 同口径）
 */

#include "host_link.h"

#include <string.h>

#include "main.h"
#include "board.h"
#include "bsp_board.h"
#include <stdio.h>          /* snprintf（每秒诊断文本用）*/
#include "host_ext.h"      /* v2：驾驶/动作/舵机/固化（业务都在这里，协议层不碰控制模块） */
#include "jy61p.h"
#include "kk_oled.h"
#include "motion.h"        /* v2：驾驶状态回报用 Motion_GetMode */
#include "nvram.h"         /* v2：T_PARAM_SAVE 真保存（nvram_touch） */
#include "servo.h"         /* v2：动作表/舵机角度 */
#include "speed_control.h" /* 遥测 v_l/v_r：真实轮速（编码器 mm/s） */
#include "app_ui.h"          /* ★审计 #10：g_cfg 镜像（T_PARAM_SET 要同步）*/
#include "task_run.h"      /* v2：任务解释器（T_TASK_RUN 启动 / T_TASK_STATUS 查询） */
#include "task_store.h"    /* v2：任务程序 Flash 存储（页 58 目录 + 页 59..61 程序区） */
#include "trace_cal.h"     /* v2：T_CAL_GET 标定回读 */
#include "tracing.h"
#include "SEGGER_RTT.h"
#include "gyro_control.h"   /* GyroControl_GetYawCont / GetTargetAngle：角度环探针用 */

/* 实际 PWM / 舵机脉宽要从定时器比较寄存器读 */
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim8;

/* =====================================================================
 *  CRC16/CCITT-FALSE  (poly 0x1021, init 0xFFFF, 不反转)
 *  标准校验值："123456789" -> 0x29B1（上位机单测用同一个值钉住）
 * ===================================================================== */
static uint16_t crc16_ccitt(const uint8_t *d, uint16_t n)
{
  uint16_t crc = 0xFFFFu;
  uint8_t  b;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (b = 0; b < 8u; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* =====================================================================
 *  参数表
 * ===================================================================== */
typedef struct {
  uint16_t id;
  float    def;
  float    min;
  float    max;
} param_spec_t;

static const param_spec_t SPECS[] = {
  /* 循迹/角度环增益 = 旧板硬编码值 ×7.2（旧板 PWM 满量程 1000 → 本板 7200；这两个环的输出
   * 是直接加在左右轮上的 PWM 偏置，所以照抄旧值等效只有 1/7.2 力度）。积分限幅不折算
   * （量纲是误差本身）。见 app_ui.c 的 g_cfg 默认值注释（2026-09-30 m02112 同步）。 */
  { HP_LINE_KP,          2700.0f,     0.0f,     5000.0f },
  { HP_LINE_KI,            10.8f,     0.0f,      200.0f },
  /* HP_LINE_KD：2026-10-05 起归**串级外环的微分（阻尼）项**用（用户 m33254；老 mode-2 循迹
   * PID 在现在的固件里没有任何调用者）。新量纲 = °/s per (格/s)：重心每 1 格/s 的变化要求
   * 多少额外转率。13.0 ⇒ 外环阻尼比 ζ≈0.7 @per_unit=60、≈1.0 @per_unit=30。
   * 老值 360 是旧量纲（PWM 偏置 / 误差），上新律会过阻尼，别再用。
   * 2026-10-05 先定 **12.0**（第 20 轮 q 系列 + 第 18 轮 z 系列：D=12 与 D=18 同口径无差别）。
   * ★2026-10-06 **真车在 0.5m 正常线上重定为 36.0**（第 29~31 轮共 19 趟，1.4s/趟、丢线全 0）：
   *   kd=12 → pos 摆动 RMS 0.274~0.308、限幅 6~7%；kd=36 → **0.112~0.244、限幅 2.2~2.9%、
   *   换向 0/0/1.3**；kd=48 → 限幅跳到 15~18%（D 项超出执行器能力）。为什么理论 zeta≈1 还要加：
   *   `TRACE_FF_LPF` 那道 2Hz 低通在 ~1Hz 处吃掉约 27° 相位 ⇒ 阻尼要给到理论值的 3 倍才见效。 */
  { HP_LINE_KD,            36.0f,     0.0f,     1000.0f },
  { HP_LINE_ILIMIT,        50.0f,     0.0f,    10000.0f },
  /* 循迹基准速度：默认与 app_ui.c 的 g_cfg.trace_speed 默认值一致（6480 = 旧 900 刻度），
   * 开机时 app_ui 会把菜单里的值 push 进来覆盖它。
   * 慢速档 2500 = 满量程 35%，是静摩擦起步死区（~2500~2900）的下沿；旧板写的 200
   * 是旧刻度（旧板满量程 1000），×7.2 只有 1440 —— 仍在死区以下，所以这里**故意不折算**，
   * 保持 2500（m02112 同步里的例外，理由=硬件死区）。 */
  /* ★2026-10-06 循迹专用的「期望角速度上限」。为什么必须与 HP_GYRO_RATE_MAX 分开：
   * 角度环里 rate_ref = ang_kp×err + ff_rate 被夹在 ±s_rate_max，而 corr = rate_kp×(rate_ref−v)
   * ⇒ **corr 的上限 = rate_kp × rate_max**。pivot 那套是 40×120 = 4800，而 base=7000 时
   * 要让内侧轮反转需要 corr > base = 7000（⇒ rate_ref > 175）。直角/大偏差时这个需求是真的
   * （用户 m：「如果这个转弯要特别大，比如碰见 90° 的直角之类的，它的左轮或者右轮是可以变成
   * 负数的」；实测车上 corr 恒 7200 顶死、航向误差积到 16.8° 就是不动的证据）。
   * 分开之后：循迹（ff_rate != 0）用本项，**pivot（ff_rate == 0）仍用 HP_GYRO_RATE_MAX**
   * ⇒ 原地转圈定稿那条路逐位不变。
   * 300：两轮反向对转的物理上限约 240 °/s，留一点余量；它同时是 PID_tracing.c 外环自己的
   * 速率夹子（那边原来借的就是 HP_GYRO_RATE_MAX）。 */
  { HP_TRACE_RATE_MAX,    300.0f,    10.0f,      300.0f },
  { HP_TRACE_CORNER,        0.0f,     0.0f,        1.0f },   /* ★直角主动转弯开关（0=关＝默认纯循迹，1=开）*/
  { HP_BASE_SPEED,       6480.0f,     0.0f,     7200.0f },
  { HP_BASE_SPEED_SLOW,  2500.0f,     0.0f,     7200.0f },
  /* 任务解释器启动时的基准速度（`task_run.c:305` 用它 `Motion_SetSpeed` 一次，之后串级循迹
   * 就按这个速度跑）。2026-10-06 用户 m 要求「把基准速度拉高，基本上到最后在 7000 左右」⇒ 6480 → 7000。
   * 注意它**不**受 NVRAM 影响（不在 app_ui 的 g_cfg 里）⇒ 改这里 + 烧录就对，不用做迁移。 */
  { HP_TASK_SPEED,       7000.0f,     0.0f,     7200.0f },
  /* 速度环 PI 增益 */
  { HP_SPEED_KP,           20.0f,     0.0f,     2000.0f },
  { HP_SPEED_KI,            2.0f,     0.0f,      200.0f },
  { HP_SPEED_KD,            0.0f,     0.0f,      500.0f },
  { HP_SPEED_ILIMIT,     3000.0f,     0.0f,    20000.0f },
  /* 速度环前馈（台架实测拟合，硬件标定量；默认值来自 host_link.h 的宏） */
  { HP_SPEED_FF_SLOPE, SPEED_FF_SLOPE, 0.0f,       2.0f },
  { HP_SPEED_FF_FRIC,  SPEED_FF_FRIC,  0.0f,    5000.0f },
  /* 角度环增益：**不按 ×7.2**。2026-10-01 原地台架实测（只改 RAM，见
   * out/_angletest.ps1）：由旧刻度换算来的 720/72 在本板必自激（Δ=3°..20° 全振，
   * 尾晃 20~38°、峰值 400~600°/s）；150/15 稳定（3° 阶跃误差 0.6°、20° 过冲 4~10°、
   * 90° 分六次增量转累计误差 1.1°）。改这里要同步 app_ui.c 的 g_cfg 默认值
   * 与开机迁移判断（CFG_GYRO_*_OLD_X100）。 */
  { HP_GYRO_KP,           150.0f,     0.0f,     2000.0f },   /* 实测选定（旧 720 自激） */
  { HP_GYRO_KI,             0.0f,     0.0f,      200.0f },   /* 默认 0：积分被静摩擦死区"喂"成常驻推力，车会停在停车带外一直抽 */
  { HP_GYRO_KD,            15.0f,     0.0f,      500.0f },   /* ≈Kp/10，压过冲 */
  { HP_GYRO_ILIMIT,      1000.0f,     0.0f,    20000.0f },
  { HP_TURN_ANGLE_OFFSET,  90.0f,     1.0f,      180.0f },
  { HP_TURN_TOLERANCE,      2.0f,     0.1f,       20.0f },
  { HP_TURN_TIMEOUT,     1500.0f,   100.0f,    10000.0f },
  { HP_YAW_RATE_DAMP,       1.0f,     0.0f,       20.0f },
  { HP_MARKER_HOLD,         3.0f,     1.0f,       50.0f },
  { HP_MARKER_MIN_GAP,     50.0f,     0.0f,     5000.0f },
  /* ★2026-10-15：默认值改成**现役电机 MG513X** 的真值。
   * 旧值 1980 = 11(线) × 4(倍频) × 45(减速比)，是**上一代 JGB37-3865-520** 的。
   * MG513X 数据手册（用户提供）：13 线霍尔编码器、减速比 **1:28**、
   *   额定 12V/0.36A、堵转 3.2A、空载 370±12rpm、额定扭矩 1.02kgf·cm、4.1W。
   *   ⇒ 计数/输出轴圈 = 13 × 4 × 28 = **1456**。
   * 这个值只影响 mm/s 遥测换算（speed_control.c 的 s_mmps）与台架标尺；
   * 速度环的控制刻度是 HP_SPEED_FULL_COUNTS（下面那条），两个要一起改。 */
  { HP_COUNTS_PER_REV,   1456.0f,     1.0f,   100000.0f },
  { HP_WHEEL_DIA,          65.0f,    10.0f,      300.0f },
  { HP_WHEEL_BASE,        150.0f,    10.0f,      500.0f },
  /* 满 PWM(7200) 时每 5ms 的期望编码器计数：速度环的"PWM 刻度 ↔ 计数"换算刻度。
   * ★2026-10-15：改成 MG513X 的真值。
   *   空载 370 rpm ⇒ 370/60 = 6.167 圈/s ⇒ × 1456 计数/圈 = 8980 计数/s
   *   ⇒ 每 5ms = 8980/200 = **44.9 ⇒ 45**。
   *   交叉验算：45 计数/5ms ⇒ 45 × 200/1456 × π × 65 = 1262 mm/s；
   *            而数据手册 370 rpm × 204.2mm/60 = 1259 mm/s ✓ 吻合。
   * 旧值 25 是 JGB37-3865-520（11 线 / 45:1 / 空载 150rpm ⇒ 4950 计数/s = 24.75）的。
   * ⚠ 换电机后 counts_per_rev 与这一项**必须成对改**，否则遥测与环的量纲各差一个比例。 */
  { HP_SPEED_FULL_COUNTS,    45.0f,     1.0f,     5000.0f },
  { HP_PWM_MAX,          7200.0f,   100.0f,     7200.0f },
  { HP_VBAT_NOMINAL,    12000.0f,  3000.0f,    30000.0f },
  { HP_STALL_CURRENT,    2500.0f,   100.0f,    20000.0f },
  { HP_VBAT_LOW,        10500.0f,  3000.0f,    30000.0f },
  { HP_GYRO_BRAKE_GAIN,     2.0f,     1.0f,       10.0f },   /* ★刹车增强系数，见 host_link.h */
  /* ---- v2（用户 m13466 的新上位机）：控制环标定量全部进参数表 ----
   * 这几项原来只活在控制模块的 `volatile float` 全局里（台架靠 SWD 直接改），
   * 现在由各控制环每拍从参数表回读（见 gyro_control.c / pid.c / PID_tracing.c），
   * 所以"上位机改参数 + 固化到 Flash"就能覆盖所有环，不必再烧固件。
   * 默认值 = 2026-10 台架扫出来的那套（第十/十一轮，见 docs/自主优化记录.md）。
   * ★2026-10-05 真车循迹定稿（第 18~21 轮，见 out/_sweep18..21.log 与 docs 记录）：
   *   **`HP_GYRO_RATE_KP` 250 → 80** —— 根因定案：3~4Hz 横向极限环跟着**速率环 P** 走、
   *   不跟 `gyro_ang_kp` 走。台架扫（30° 阶跃 + 5° 小信号）与真车都测到 rkp 250 时修正量
   *   长时间钉在 ±corr_lim（限幅占用 66~85%）、车"原地拧"；降到 80 后限幅占用 0.0~1.7%、
   *   `pos` 晃动 RMS 0.762→0.13~0.30、yaw 晃动 4.61°→0.61~1.2°，且同 base 下**车速翻倍**
   *   （183→344 刻度，因为不再有一半时间在跟限幅较劲）。
   *   rkp125 是**双稳态**的（第 18 轮 v3/v6 同起点配对，同一参数两次差 7 倍），80 没出现过
   *   ⇒ 取 80，留裕量。（`gyro_rate_kd` 扫过 4/8/12/20：**无差别**，D 项相位超前的收益被
   *   对陀螺做差分带来的噪声抵掉 ⇒ 保持 4。`HP_GYRO_RATE_MAX` 110→120 以与验证条件逐位一致。）
   * ★★2026-10-05 深夜**原地转圈**整定（用户 m35487「原地转圈调一下这个角度环」，桥
   *   `POST /api/drive {"mode":"base_turn","base":0,"turn":±30}`，工具 `out/_gsweep.py`
   *   / `out/_greport.py`，30° 阶跃，5 次重复的组合头对头）：
   *   **`HP_GYRO_RATE_KD` 4 → 0** —— D 项是抖动的元凶：实测 `corr` 在逼近段反复打到**反向满轨**
   *   （t=0.70 那一刻实测 yaw 速率只有 10~20°/s 而 rref=31，P 项根本算不出 −3000，只有
   *   `kd × dv/dt` 要 ~750°/s² 才解释得通）。kd = 4/0/10/20 中位数：上升 0.797、**0.687**、1.367、
   *   到不了 90%，换向 5.1、**2.4**、7.1、7.5 次·s⁻¹ ⇒ **kd=0 全面最好**（也解释了为什么降
   *   rate_kp 反而更抖：rate_kp 越小，D 的占比越大）。
   *   **`HP_GYRO_ANG_KP` 3.5 → 7、`HP_GYRO_RATE_KP` 80 → 160** —— D 项堵死这条路之后，两个增益
   *   往高扫才显出收益（`corr = rate_kp × (rref − v)`：rate_kp=80 时 corr 要打到 3000 才撑得住
   *   82°/s，160 能撑到 101°/s）。组合头对头（5 次重复，cap 3000）：出厂原值 3.5/80/4 → 上升
   *   0.735s、静差 −1.50°、换向 4.8 次/s；**7/160/0 → 上升 0.281s、静差 −0.40°、换向 1.2 次/s、
   *   调节时间 1.204s、末段纹波 0.00°**（比出厂原值快 2.6 倍）。
   *   同批确认**不动**的旋钮：`gyro_rate_max`（60~300 全在重复噪声里）、`gyro_corr_limit`
   *   （抬到 4500/6000/7200 只快一点，代价是 ±3.7~4.7° 的**持续极限环**）、`gyro_corr_slew`
   *   （调小到 1e4/3e3 会把超调推到 +61%/+107%）、`gyro_settle_deg/settle_ms`。
   *   ⚠ 遗留：循迹 P/D（`trace_angle_per_unit=30`、`line_kd=12`）是在 ang_kp=3.5、rate_kp=80、
   *   kd=4 的内环下调出来的，内环换了 ⇒ **赛道行为需复验**。
   * ★★2026-10-06 凌晨**原地转圈残余晃动**整定（用户 m35778「现在我感觉还是有点晃，就是在这个
   *   目标角度附近震荡」；同样的 30° 阶跃台架，工具 `out/_gsweep.py` 的 ab2/ab3/ab4/ab5 组做
   *   **同会话交错 A/B**（每 1 个候选值前面紧跟一趟基线，4~5 轮共 40+ 趟），口径用 `out/_ghold.py`
   *   / `_ghold2.py`：从"第一次到 90%"起的 1.2s 窗内 yaw 峰峰值 p-p、纹波 RMS、换向次数、到静止时间）：
   *   **`HP_GYRO_RATE_KP` 160 → 40** —— 到位后那串"来回踢"是**内环 P 的 stick-slip**：
   *   `corr = rate_kp × (rref − v)` 在 160 时，小误差处 160×7×err 恰好落在**静摩擦起转阈值
   *   （≈3450）**附近 ⇒ 顶过去一脚 → 被刹停回摆 → 再满轨踢回，`corr` 反复打到 ±3000
   *   （"顶夹"帧占比基线 27~88%）。降到 40 后顶夹只剩 8~11%：摆动 p-p 中位 **6.2°（3.3~9.0）
   *   → 2.6~3.1°**、换向 **1~5 → 0~1**、到位后 0.17~0.46s 内彻底冻住（8s 长保持从 1.7s 起
   *   每桶 p-p 恒 0.00、vl=vr=0）；代价是到位时间 t90 0.48s → 0.65s（**+0.17s**）。
   *   累计样本：rate_kp=40 系 17 趟**全部** p-p 2.6~3.3°，基线 21+ 趟 3.3~9.0°。
   *   **`HP_GYRO_SETTLE_DEG` 2.0 → 1.0**（当天先收到 1.0，白天再收到 0.8，见下段）—— rate_kp=40 单独用会**系统性少转 ~1.4°**：起转助推
   *   （gyro_control.c:673 `if (!s_settled && fabsf(err) > s_settle_deg && ...)`）在
   *   |err| < settle_deg 时**不武装**，最后 1~2° 名义上没人推。收到 1.0 后末误差回到 −1.4~+1.1°
   *   （与基线同量级），而 p-p 仍 2.7~3.0°、t90 0.55~0.69s（比 rate_kp=40+settle 2.0 还快）；
   *   0.5 反而更差（p-p 回到 4.2°）。
   *   同批**试过并否掉**的旋钮：`gyro_rate_kd` 1/2（p-p 与基线同量级，顶夹只从 31% 降 28%）、
   *   `gyro_settle_ms` 300/600（顶夹与 p-p 都更差）、`gyro_corr_slew` 调小（3000 时 p-p 31~40°
   *   灾难）、`s_settle_rate` 用 SWD 写 RAM 降到 12/4（**更差**：换向 5~10、到窗口结束都不静）、
   *   `gyro_ang_kp` 3.5（p-p 3.0° 但 t90 0.86s ⇒ 被本组支配）。
   * ★★2026-10-06 白天**续调**（用户 m37624「你继续调吧,反正最后把调好的所有参数都要同步到
   *   上位机,固件,还有 Git」；工具 `out/_gsweep.py` 的 ak4/ak5/fin1/fin2 组 + `out/_agg.py`
   *   / `out/_ghold.py`，真车桥 `:8770`，同一台架 30° 阶跃）：
   *   **`HP_GYRO_ANG_KP` 7 → 10** —— 唯一能压缩到位时间的旋钮。三轮独立交错 A/B（ak5 7 候选×5 轮、
   *   fin1 3×8 轮 4s 窗、fin2 3×6 轮，共 19 趟、两个转向各半）一致：10-90% **0.47s → 0.29s**、
   *   到 90% **0.67s → 0.50s**（基线没有一趟进得了 0.45s，新值只有一趟 0.67s）；窗 p-p ~3°、
   *   yaw 换向 0~1 次、静止 0.3~0.4s **都不变**；峰值反而更收敛（fin1 八趟最大 +2.3%，基线同期
   *   有一趟冲过头到 139.8°）。ang_kp 14 更快但稳定 +4~5% 冲过、ang_kp 5 慢一倍 ⇒ 都不取。
   *   **`HP_GYRO_SETTLE_DEG` 1.0 → 0.8** —— 0.8 是本组唯一「五趟窗 p-p 全 ≤3.0°、中位 2.8°、
   *   yaw 换向 0 次」的候选；它还把 ang_kp=10 的偶发冲过从 +7.7%（配 1.0）收到 +5.0%；
   *   0.5 会把 corr 抖动放大（换向 3.4 次/s）⇒ 不再往下压。末误差 −0.9~+0.7°。
   *   同批否掉：`gyro_rate_max` 200（顶夹 12.4~14.1% 变高、p-p 最大 4.8°）、ang_kp 5/14。
   *   ⚠ **方法论坑（备查）**：交错组的转向按**全局 run_no** 交替（`out/_gsweep.py` 里
   *   `turn = a.turn if run_no % 2 == 0 else -a.turn`）⇒ **每轮候选数必须是奇数**，否则同一个
   *   标签永远只朝一个方向转、方向与配置混淆（ak4 就是 6 个候选：base/a14/rmax200 恒 +30、
   *   a10/a5/sd08 恒 −30，只能读同方向的两对）。ak5/fin1/fin2 全用奇数候选。
   *   ⚠ `gyro_settle_deg` 只影响原地转圈（带内松手 / 锁存 / 助推全部被 `ff_rate == 0` 门控），
   *   但 **`HP_GYRO_RATE_KP` 与 `HP_GYRO_ANG_KP` 都是转圈与循迹共用的**：PID_tracing.c 每拍修改
   *   目标航向（`GyroControl_AimRelative`）并把重心误差以前馈送进 `rate_ref` ⇒ 循迹航向环的阻尼
   *   由 `rate_kp`、刚度由 `ang_kp` 决定。本轮 rate_kp 160→40 与 ang_kp 7→10 合起来把循迹航向环的
   *   开环增益从 7×160=1120 改到 10×400=400（软 2.8 倍）⇒ **上赛道前必须复验循迹**。
   *   （要零风险得给转圈分支单独引 `s_rate_kp_pivot` / `s_ang_kp_pivot`，那要改代码 + 重烧；
   *   台架一键还原：`out/_apply_final.py --revert` = 7 / 160 / 2.0。） */
  { HP_GYRO_ANG_KP,       10.0f,     1.0f,       20.0f },   /* 角度误差 → 期望角速度 (°/s)/°（2026-10-06 定 10，见上） */
  { HP_GYRO_RATE_KP,      40.0f,    10.0f,     2000.0f },   /* 速率环 P（2026-10-06 原地转圈定稿 40，见上） */
  { HP_GYRO_RATE_KD,       0.0f,     0.0f,      100.0f },   /* 速率环 D（默认关：D 项是抖动源） */
  { HP_GYRO_RATE_MAX,    120.0f,    10.0f,      300.0f },   /* 期望角速度上限 °/s */
  /* ★2026-10-05 第 21 轮真车 A/B 定稿：**2000 → 3000**。
   * 本拍生效的上限是 `cap = limit × max(1, |base| / corr_ref)`（gyro_control.c:485-495），
   * 所以 base=7000 时：旧默认 2000/3000 ⇒ cap 只有 4666，新默认 3000/3000 ⇒ cap 7000。
   * 实测（8 趟 A/B，rkp80/P30/D12 全锁死，起点航向差记在 out\_sweep21.log）：
   *   cap 4666（k1 起点 Δyaw=0°：`pos` 偏离 RMS 1.295、大误差 32.5%、rate 限幅 26.7%；
   *            k8 Δyaw=18°：0.360、0.0%、1.2%）—— **同一参数一次好一次坏 = 贴着稳定边界**；
   *   cap 7000（k2 Δyaw=2°：0.394、0.0%、1.2%；k7 Δyaw=25°：0.294、0.0%、2.3%）
   *            —— **两趟全干净，且对 25° 的起点姿态误差不敏感**；
   *   cap 7200（k3 Δyaw=20°：1.904、46.4%、50.2%；k6 Δyaw=35°：0.609、2.0%、1.6%）—— 不一致；
   *   cap 2333（k4/k5：真丢线 70.6% / 56.3%）—— **灾难性，可复现**。
   * ⇒ 出厂值取**唯一两趟全干净的 3000/3000**（cap 7000）。注意这同时把 base≤3000 时的
   * 温柔刹车从 2000 抬到 3000，方向与本节注释的原意（"给 4000 兜底，别把加速档 3600 夹掉"）一致。 */
  { HP_GYRO_CORR_LIMIT, 3000.0f,   200.0f,     7200.0f },   /* 修正量限幅（PWM 刻度） */
  { HP_GYRO_CORR_SLEW, 300000.0f, 1000.0f,  2000000.0f },   /* 修正量斜率限幅 PWM/s */
  { HP_GYRO_SETTLE_DEG,    0.8f,     0.2f,       10.0f },   /* 停车带宽 °（2026-10-06 定 0.8：让起转助推能补最后 1°，且窗 p-p ≤3.0°） */
  { HP_GYRO_SETTLE_MS,   150.0f,     0.0f,     2000.0f },   /* 到位锁存时长 ms */
  /* 差速天花板随速放大的参考刻度（0=关闭）。
   * ★2026-10-06 试过 3000 → 1500（cap 7000 → 14000，想让内侧轮在带速时反转），
   * **真车否掉了**：cap 一放大，看见线时的环路就变成继电器 —— 用户 m 报「拐回来之后
   * 一直震荡在直线上」，日志里 corr 两个方向打到 ±14000、车速 20~250mm/s 抽、
   * pos 以 ~750ms 周期扫（out/_drv_7000_fix3.txt）。这就是第 21 轮同一条教训。
   * 直角要的"轮子变负"改成**进找线时把基准打成 0**（`PID_tracing.c` 的找线分支）——
   * 那时 `left = −corr / right = +corr` 天然对转，走的是调好的 pivot 工况（cap = corr_limit），
   * 不需要动这里。所以本项**保持 3000**。 */
  { HP_GYRO_CORR_REF,   3000.0f,     0.0f,     7200.0f },
  /* ★2026-10-07 **起步助推**的两个旋钮（用户报「操控页原地转一卡一卡」的定位结论：
   * 起步半秒角速度是「冲 36°/s → 掉回 0 停 ~0.3s → 再冲到 75」，因为静摩擦滞回
   * （起转 ≈3450 刻度 / 维持 ≈1000）配上**脉冲式**助推 —— 爬到 3600 把车点着、车一动
   * 助推立刻撤、比例项又推不动，于是掉回静止，只能等冷却结束再点一次。
   * 加这两项**只为能在线扫**：像 `gyro_corr_limit` 一样 J-Link 只读 / 桥
   * `POST /api/params/set` + `push` 改 RAM，试一个值不必重烧。
   * ⚠ **不持久化**：不在 `app_ui.c` 的 `g_cfg`/NVRAM 里（与 `HP_TASK_SPEED`、
   *   `HP_TRACE_RATE_MAX` 同类）⇒ 只吃本表 + 上位机 push，复位/烧录回默认；
   *   **不要**去动 `app_ui.c` 的 `push_pull_pid_params()`（那三处只同步持久化的那些项）。
   * ⚠ **默认值 = 进参数表之前的硬编码值**（`gyro_control.c` 里 `s_boost_gap = 250.0f`、
   *   `s_boost_rate = 60000.0f`）⇒ 不 push 时行为与以前**逐位一致**。
   * ⚠ 250ms 冷却**不能盲目调小**：它是为了防「撤了又点 ⇒ 来回射」（60° 台阶尾部
   *   ±2~4°、±50°/s 的来回穿梭）。要试小值就 push 着试，随时 push 回 250。
   * 单位与语义见 `gyro_control.c` 的 `s_boost_gap` / `s_boost_rate` 注释。 */
  { HP_GYRO_BOOST_GAP_MS,  250.0f,     0.0f,     1000.0f },  /* 起转助推冷却 ms（0 = 不冷却，退回 2026-10 之前的行为）*/
  { HP_GYRO_BOOST_RATE,  60000.0f, 10000.0f,   200000.0f },  /* 起转助推爬升斜率 PWM/s（60000 ⇒ 60ms 到 3600；10000 ⇒ 360ms）*/
  { HP_TRACE_ANGLE_PER_UNIT, 30.0f,  0.5f,       60.0f },   /* 串级：重心误差 → 转率 (°/s)/格 */
                                                            /* 2026-10-05 定稿 30（第 20 轮 P 剂量-反应）：
                                                             * P=40/50 会把 rate_ref 顶到 `HP_GYRO_RATE_MAX`
                                                             * （限幅占用 58~63%）⇒ 外环进继电器状态、~0.5Hz
                                                             * 大摆（`pos` 偏离 1.7~1.9 格）；P=20 有一趟真丢线
                                                             * 18%。P=30 最好且最稳（偏离 0.316、占 0.8%）。 */
  { HP_TRACE_ANGLE_MAX,   35.0f,     1.0f,       90.0f },   /* 串级：目标航向相对当前航向的前置量上限 ° */
  { HP_PIVOT_KP,         900.0f,     0.0f,    20000.0f },   /* 原地转圈（base=0）循迹环 P */
  { HP_PIVOT_KI,           0.0f,     0.0f,      200.0f },   /* 原地转圈 I（默认 0：积分会被静摩擦喂成常驻推力） */
  { HP_PIVOT_KD,         405.0f,     0.0f,    20000.0f },   /* 原地转圈 D */
};
#define NSPEC (sizeof(SPECS) / sizeof(SPECS[0]))

static float s_param[NSPEC];
static host_link_stats_t s_stats;

/* 全表参数 dump 的分帧发送状态（见 handle_frame 的 T_PARAM_GET）：
 * 一次 dump 有 NSPEC 帧，不一次性发完，由 host_link_poll 每拍发 DUMP_PER_POLL 帧。
 * 921600 + 发 DMA 槽位队列（board.c）之后一帧只是"拷 131 字节 + 返回"，
 * 每拍 8 帧 ≈ 1KB ≈ 11ms 的串口时间，正好填满队列又不挡控制环。 */
static uint16_t s_dump_i;
static uint8_t  s_dump_seq;
static uint8_t  s_dump_busy;
#define DUMP_PER_POLL 8u

/* v2 遥测帧选择（T_CMD / CMD_SET_TELEM_MODE）：
 *   0 = 只发老 32 字节 T_TELEM（带宽小）
 *   1 = 只发 64 字节 T_TELEM2（默认：新上位机要灰度/重心/角度环/模式这些量）
 *   2 = 两个都发（调试用；早先 115200 下 50Hz 就占 37% 带宽，921600 之后随便发） */
static uint8_t s_telem_mode = 1u;

/* ★探针（2026-10-15）：Load() 实际算出的两路值，定义在 motor.c。
 * 用来对照"环要多少(req/out) / Load 写多少(loadA/B) / TIM8 寄存器(regL/R)"三段，
 * 定位"右路对负目标无输出"这类问题。 */
extern volatile int g_load_a, g_load_b;

static int spec_index(uint16_t id)
{
  uint16_t i;
  for (i = 0; i < NSPEC; i++) {
    if (SPECS[i].id == id) return (int)i;
  }
  return -1;
}

static uint32_t param_table_crc(void)
{
  /* 与上位机 protocol.py 的 param_table_crc 同构：把所有 (id, def) 打包后算 CRC */
  uint8_t buf[NSPEC * 6];
  uint16_t n = 0, i;
  for (i = 0; i < NSPEC; i++) {
    uint32_t bits;
    buf[n++] = (uint8_t)(SPECS[i].id & 0xFFu);
    buf[n++] = (uint8_t)(SPECS[i].id >> 8);
    memcpy(&bits, &SPECS[i].def, 4);
    buf[n++] = (uint8_t)(bits & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 8) & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 16) & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 24) & 0xFFu);
  }
  return (uint32_t)crc16_ccitt(buf, n);
}

/* 参数值必须是有限数：NaN 会让范围检查失效（NaN 与任何数比较都是 false），
 * 一旦混进 Kp/Ki 就一直传到 (int) 强转 —— 那是未定义行为，输出会变成野值。
 * |v| > 1e9 把 ±Inf 也挡掉（Inf != Inf 不成立，只能靠范围判）。 */
static int param_finite(float v)
{
  return (v == v) && (v >= -1.0e9f) && (v <= 1.0e9f);
}

float host_param_get(uint16_t id, float fallback)
{
  int i = spec_index(id);
  if (i < 0) return fallback;
  return param_finite(s_param[i]) ? s_param[i] : fallback;
}

uint8_t host_param_u8(uint16_t id, uint8_t fallback)
{
  float v = host_param_get(id, (float)fallback);
  if (v < 0.0f) v = 0.0f;
  if (v > 255.0f) v = 255.0f;
  return (uint8_t)(v + 0.5f);
}

uint32_t host_param_u32(uint16_t id, uint32_t fallback)
{
  float v = host_param_get(id, (float)fallback);
  return (v <= 0.0f) ? 0u : (uint32_t)(v + 0.5f);
}

void host_param_set_local(uint16_t id, float v)
{
  int i = spec_index(id);
  if (i >= 0 && param_finite(v)) s_param[i] = v;   /* 非有限值直接丢掉，不污染参数表 */
}

/* =====================================================================
 *  帧的收发
 * ===================================================================== */
#define SOF0 0xA5u
#define SOF1 0x5Au
#define MAX_PAYLOAD 128u

enum { T_TELEM = 0x01, T_TELEM2 = 0x03, T_PARAM_GET = 0x10, T_PARAM_SET = 0x11,
       T_PARAM_REPORT = 0x12, T_PARAM_SAVE = 0x13, T_CAL_GET = 0x14, T_CAL_REPORT = 0x15,
       T_ACTIONS_GET = 0x16, T_ACTIONS = 0x17, T_ACTION_SET = 0x18, T_ACTION_SAVE = 0x19,
       T_SERVO_SET = 0x1A, T_SERVO_PLAY = 0x1B, T_DRIVE = 0x1C, T_DRIVE_REPORT = 0x1D,
       T_SERVO_DEV = 0x1E,
       /* ★2026-10-11（用户："传感器页把两种标定的值都用图显示"）：第二套标定。
        * 一帧装不下两套（82×2=164 > MAX_PAYLOAD 128）⇒ 分两帧发：
        *   · T_CAL_REPORT   = **当前在用的那套**（格式一个字没动，老上位机照旧能用）
        *   · T_CAL_REPORT_B = [当前套号 u8][**另一套** trace_cal_data_t]
        * 老上位机不认识这个帧类型，会按"不认识"忽略 —— 兼容是安全的。 */
       T_CAL_REPORT_B = 0x1F,
       T_CMD = 0x20, T_ACK = 0x21, T_HELLO = 0x40, T_HELLO_REQ = 0x41, T_TEXT = 0x50,
       /* v2 上位机任务程序（docs/任务编程.md §5） */
       T_TASK_LIST = 0x60, T_TASK_LIST_R = 0x61, T_TASK_PUT = 0x62, T_TASK_COMMIT = 0x63,
       T_TASK_GET = 0x64, T_TASK_DATA = 0x65, T_TASK_DEL = 0x66, T_TASK_RUN = 0x67,
       T_TASK_STOP = 0x68, T_TASK_STATUS = 0x69 };

enum { ST_OK = 0x00, ST_UNKNOWN_TYPE = 0x02, ST_BAD_PARAM = 0x03,
       ST_OUT_OF_RANGE = 0x04, ST_NOT_READY = 0x05, ST_BAD_CMD = 0x06,
       ST_BUSY = 0x07,
       /* 0x08：命令/功能"认识但不支持"（不是坏帧，也不是暂时没就绪）。
        * 目前唯一来源：CMD_START_A/B/C —— 内置比赛流程已删除，改用任务页的 0x67 RUN。
        * 上位机 protocol.py 的 ST_NAMES 要同步这个码，否则只显示成未知 0x08。 */
       ST_UNSUPPORTED = 0x08 };

enum { CMD_STOP = 0x01, CMD_START_A = 0x02, CMD_START_B = 0x03, CMD_START_C = 0x04,
       CMD_CALIB_GYRO = 0x05, CMD_SAVE_PARAMS = 0x06, CMD_CLEAR_FAULT = 0x07,
       CMD_SET_TELEM_HZ = 0x08, CMD_RESET = 0x09, CMD_SET_LOGGING = 0x0A,
       CMD_SET_TELEM_MODE = 0x0F };

/* v2：能力位（T_HELLO 末尾 u32）——上位机据此决定页面显示什么，不靠猜版本号 */
#define HOST_CAPS_ALL  ((uint32_t)(HOST_CAP_TELEM2 | HOST_CAP_ACTIONS | HOST_CAP_SERVO \
                                 | HOST_CAP_DRIVE | HOST_CAP_CAL | HOST_CAP_FLASH \
                                 | HOST_CAP_WATCHDOG | HOST_CAP_TASK))

#define TELEM_LEN  32u
#define TELEM2_LEN 64u
/* T_TELEM2 相对 T_TELEM 新增的偏移（前 32 字节逐字节相同，上位机一套解析两用） */
#define TELEM2_GRAY_OFF   32u   /* u8[8]  8 路灰度柱高 0..255 */
#define TELEM2_POS_OFF    40u   /* i16   循迹重心 ×100 */
#define TELEM2_PID_OFF    42u   /* i16   循迹环本拍转向偏置 */
#define TELEM2_AERR_OFF   44u   /* i16   角度环误差 ×10 */
#define TELEM2_ARATE_OFF  46u   /* i16   角度环期望角速度 ×10 */
#define TELEM2_ACORR_OFF  48u   /* i16   角度环修正量 */
#define TELEM2_MODE_OFF   50u   /* u8    0 空闲 1 陀螺 2 循迹 3 串级 */
#define TELEM2_TASK_OFF   51u   /* u8    任务号（TaskRun_Id）；没载入 = 0xFF */
#define TELEM2_S1_OFF     52u   /* u8    舵机1 角度 */
#define TELEM2_S2_OFF     53u   /* u8    舵机2 角度 */
#define TELEM2_ACT_OFF    54u   /* u8    当前动作号（最近一次下发的） */
#define TELEM2_NVRAM_OFF  55u   /* u8    NVRAM_STATE_* */
#define TELEM2_DRIVE_OFF  56u   /* u8    驾驶模式 0..3 */
#define TELEM2_DEN_OFF    57u   /* u8    1 = 正在被上位机驾驶 */
#define TELEM2_CROSS_OFF  58u   /* u8    1 = 检测到路口 */
#define TELEM2_TRIPS_OFF  59u   /* u8    链路看门狗自动停车次数 */
#define TELEM2_DEV1_OFF   60u   /* i16   舵机1 软件中位偏差 µs */
#define TELEM2_DEV2_OFF   62u   /* i16   舵机2 软件中位偏差 µs */

static uint8_t tx_buf[7u + MAX_PAYLOAD];

static void frame_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
  uint16_t crc;
  if (len > MAX_PAYLOAD) return;
  tx_buf[0] = SOF0;
  tx_buf[1] = SOF1;
  tx_buf[2] = type;
  tx_buf[3] = seq;
  tx_buf[4] = len;
  if (len && payload) memcpy(&tx_buf[5], payload, len);
  crc = crc16_ccitt(&tx_buf[2], (uint16_t)(3u + len));
  tx_buf[5u + len] = (uint8_t)(crc & 0xFFu);
  tx_buf[6u + len] = (uint8_t)(crc >> 8);
  /* 非阻塞：拷进发 DMA 的槽位队列就走（board.c），队列满只丢帧并计数。
     921600 下一帧 39 字节 ≈ 0.4ms，主循环再也不用等串口。 */
  (void)board_dl_send(tx_buf, (uint16_t)(7u + len));
  s_stats.tx_frames++;
}

void host_link_send_text(const char *s)
{
  size_t n = strlen(s);
  if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
  frame_send(T_TEXT, 0, (const uint8_t *)s, (uint8_t)n);
}

static void send_ack(uint8_t seq, uint8_t status)
{
  uint8_t p[2];
  p[0] = seq;
  p[1] = status;
  frame_send(T_ACK, seq, p, 2);
}

static void send_param_report(uint8_t seq, uint16_t id, float v)
{
  uint8_t p[6];
  uint32_t bits;
  p[0] = (uint8_t)(id & 0xFFu);
  p[1] = (uint8_t)(id >> 8);
  memcpy(&bits, &v, 4);
  p[2] = (uint8_t)(bits & 0xFFu);
  p[3] = (uint8_t)((bits >> 8) & 0xFFu);
  p[4] = (uint8_t)((bits >> 16) & 0xFFu);
  p[5] = (uint8_t)((bits >> 24) & 0xFFu);
  frame_send(T_PARAM_REPORT, seq, p, 6);
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }

static void send_hello(uint8_t seq)
{
  uint8_t p[24];
  const char *name = "quandi-car";
  uint32_t crc = param_table_crc();
  uint16_t hz = s_stats.telem_hz;
  memset(p, 0, sizeof(p));
  p[0] = (uint8_t)(HOST_FW_VERSION & 0xFFu);   /* fw version 0x0003（v3 起 IF_STAGE 的 p0 = 满足时间 ms）*/
  p[1] = (uint8_t)(HOST_FW_VERSION >> 8);
  p[2] = (uint8_t)HOST_PROTO_VERSION;          /* proto version 2 */
  p[3] = 0x00;                                 /* reserved */
  p[4] = (uint8_t)(crc & 0xFFu);
  p[5] = (uint8_t)(crc >> 8);
  p[6] = (uint8_t)(hz & 0xFFu);
  p[7] = (uint8_t)(hz >> 8);
  memcpy(&p[8], name, strlen(name) > 12u ? 12u : strlen(name));
  put_u32(&p[20], HOST_CAPS_ALL);              /* v2：能力位 */
  frame_send(T_HELLO, seq, p, 24);
}

/* =====================================================================
 *  遥测：把车上的真实读数打包
 * ===================================================================== */
static int16_t clamp_i16(float v)
{
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)v;
}

/* TIM8 CH1/CH2 = 电机A 两根输入，CH3/CH4 = 电机B。正速度走 IN1，负速度走 IN2。 */
static int16_t motor_pwm(uint32_t ch_fwd, uint32_t ch_rev)
{
  uint32_t a = __HAL_TIM_GET_COMPARE(&htim8, ch_fwd);
  uint32_t b = __HAL_TIM_GET_COMPARE(&htim8, ch_rev);
  if (a >= b) return (int16_t)(a > 32767u ? 32767u : a);
  return (int16_t)(-(int32_t)(b > 32767u ? 32767u : b));
}

static uint8_t line_mask(void)
{
  uint8_t m = 0, i;
  for (i = 0; i < 8u; i++) {
    if (bsp_trace_bit(i) == GPIO_PIN_SET) m |= (uint8_t)(1u << i);   /* 1 = 压线 */
  }
  return m;
}

/* 遥测 flags 位（与 tools/console/docs/API.md 的 flags 一一对应） */
#define FL_GYRO_OK    0x01u   /* 陀螺帧在动（500ms 内有新帧） */
#define FL_LOST       0x02u   /* 循迹丢线 */
#define FL_HOST_DRIVE 0x04u   /* 正在被上位机驾驶 */
#define FL_SLOPE      0x08u   /* 坡道（pitch 粗判） */
#define FL_MOTOR      0x10u   /* 速度环使能中（电机可能带电） */
#define FL_LOGGING    0x20u   /* 车上记录（未实现，恒 0） */
#define FL_FLASH      0x40u   /* 正在写 Flash */
#define FL_FAULT      0x80u   /* 运动层故障 */

/* 前 32 字节 = 老 T_TELEM 的逐字节相同内容（上位机一套解析两用） */
static void put_telemetry_base(uint8_t *p, uint32_t loop_us)
{
  static int32_t vbus_mv, ibat_ma;
  static uint32_t last_ina;
  static uint32_t last_gyro_frames;
  static uint32_t gyro_ok_ms;
  uint8_t  lm = line_mask();
  uint8_t  flags = 0;
  uint16_t v1, v2;
  uint32_t now = HAL_GetTick();
  uint32_t frames = jy61p_frame_count();

  /* INA226 也在 I2C1 上，而 OLED 走异步中断/DMA 刷屏 —— 只在屏不忙时读，
     免得抢总线把显示刷挂。100ms 读一次就够。 */
  if (!OLED_IsBusy() && (now - last_ina >= 100u)) {
    int32_t t;
    last_ina = now;
    if (ina226_read_bus_voltage_mv(&t) == HAL_OK) {
      vbus_mv = t;
      /* 顺手报给 host_ext 的母线低压保护：电池被拔（~0.9V）时它会停车，
         这样"拔电池"就等于"停机"，再插上电池也不会自己跑（用户 m29239）。 */
      HostExt_NoteVbusMv((uint16_t)((t > 0) ? t : 0));
    }
    if (ina226_read_current_ma(&t) == HAL_OK)     ibat_ma = t;
  }

  /* 陀螺在线检测：不用"有没有配好"，直接看帧计数有没有在涨（台架规矩：
   * 判陀螺活性只看 s_frames 增长 —— 见 docs/自主优化记录.md 第 16.8 节）。 */
  if (frames != last_gyro_frames) {
    last_gyro_frames = frames;
    gyro_ok_ms = now;
  }
  if ((uint32_t)(now - gyro_ok_ms) < 500u) flags |= FL_GYRO_OK;

  /* 丢线标志（用户 m29852）：以前看 lm —— 那是 PA0~PA7 裸脚，模拟量模式下模块不驱动
     它们、上拉恒为高 ⇒ lm 恒 0xFF，传感器死了也永远不报丢线，车"以为线在正中"直着走。
     现在看循迹算法自己判的丢线 + 灰度链路此刻给不出真值（trace_cal_held，含界面按住）。
     p[26] 的 line 则是"数字口原始电平"，只有「灰度接口 = 数字量」时才有意义。 */
  if (HostExt_TraceLost() != 0 || trace_cal_held() != 0u) flags |= FL_LOST;
  if (Pitch > 8.0f || Pitch < -8.0f)  flags |= FL_SLOPE;
  if (HostExt_DriveEnabled())         flags |= FL_HOST_DRIVE;
  if (HostExt_MotorEnabled())         flags |= FL_MOTOR;
  if (HostExt_MotionFault() != 0u)    flags |= FL_FAULT;
  if (nvram_state() == NVRAM_STATE_BUSY) flags |= FL_FLASH;

  memset(p, 0, TELEM_LEN);
  put_u32(&p[0], now);
  put_i16(&p[4], clamp_i16(Yaw * 10.0f));
  put_i16(&p[6], clamp_i16(Pitch * 10.0f));
  put_i16(&p[8], clamp_i16(Roll * 10.0f));
  /* v_l/v_r：编码器算出来的真实轮速（协议文档 offset 10/12，单位 mm/s）。
     用户 m28949 就是被这里骗了：本字段长期硬写 0（旧注释说"编码器还没做"，
     但 encoder.c/TIM3+TIM4 早就在跑，speed_control.c 每 5ms 都在读），
     操控页的速度波形永远是一条 0 线 —— 看着就像"PID 把速度抵消成 0"。
     正方向约定：和 Load()/速度环目标一致，**正 = 该轮朝前进方向转**；
     注意 pwm_r（offset 16）读的是 CH3/CH4 的原始极性，右路镜像安装，
     所以两轮都前进时 pwm_r 显示为负 —— 三个字段各自的约定见协议文档。 */
  put_i16(&p[10], clamp_i16((float)SpeedControl_SpeedMmps(0)));
  put_i16(&p[12], clamp_i16((float)SpeedControl_SpeedMmps(1)));
  put_i16(&p[14], motor_pwm(TIM_CHANNEL_1, TIM_CHANNEL_2));
  put_i16(&p[16], motor_pwm(TIM_CHANNEL_3, TIM_CHANNEL_4));
  put_u16(&p[18], (uint16_t)(vbus_mv < 0 ? 0 : (vbus_mv > 65535 ? 65535 : vbus_mv)));
  put_i16(&p[20], (int16_t)(ibat_ma > 32767 ? 32767 : (ibat_ma < -32768 ? -32768 : ibat_ma)));
  v1 = (uint16_t)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_1);
  v2 = (uint16_t)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_2);
  put_u16(&p[22], v1);
  put_u16(&p[24], v2);
  p[26] = lm;
  /* 判据用 IsRunning 而不是 Loaded：程序跑完 END / 被 STOP / 出故障之后，
   * "RAM 里还留着一份校验过的程序"对操作员没有信息量，报旧任务号/状态号反而误导。
   * 遥测这两个字节只回答一件事："现在在跑哪个任务的哪个状态"。 */
  p[27] = TaskRun_IsRunning() ? TaskRun_State() : TASK_RUN_OFF;  /* 解释器当前状态号 */
  put_u16(&p[28], (uint16_t)(loop_us > 65535u ? 65535u : loop_us));
  p[30] = flags;
  p[31] = 0;
}

static void send_telemetry(uint32_t loop_us)
{
  uint8_t p[TELEM_LEN];
  put_telemetry_base(p, loop_us);
  frame_send(T_TELEM, 0, p, TELEM_LEN);
}

/* v2 扩展遥测：老 32 字节 + 灰度柱高/重心/角度环/模式/动作/驾驶状态 */
static void send_telemetry2(uint32_t loop_us)
{
  uint8_t p[TELEM2_LEN];
  uint8_t gray[8];

  put_telemetry_base(p, loop_us);
  memset(&p[TELEM2_GRAY_OFF], 0, TELEM2_LEN - TELEM2_GRAY_OFF);
  HostExt_GrayLevel(gray);
  memcpy(&p[TELEM2_GRAY_OFF], gray, 8u);
  put_i16(&p[TELEM2_POS_OFF],   clamp_i16(HostExt_TracePos() * 100.0f));
  put_i16(&p[TELEM2_PID_OFF],   clamp_i16(HostExt_TracePid()));
  put_i16(&p[TELEM2_AERR_OFF],  clamp_i16(HostExt_AngleErr() * 10.0f));
  put_i16(&p[TELEM2_ARATE_OFF], clamp_i16(HostExt_AngleRateRef() * 10.0f));
  put_i16(&p[TELEM2_ACORR_OFF], clamp_i16(HostExt_AngleCorr()));
  p[TELEM2_MODE_OFF]  = HostExt_MotionMode();
  p[TELEM2_TASK_OFF]  = TaskRun_IsRunning() ? (uint8_t)TaskRun_Id() : 0xFFu;
  p[TELEM2_S1_OFF]    = (uint8_t)(HostExt_ServoAngle(1) + 0.5f);
  p[TELEM2_S2_OFF]    = (uint8_t)(HostExt_ServoAngle(2) + 0.5f);
  p[TELEM2_ACT_OFF]   = HostExt_CurrentAction();
  p[TELEM2_NVRAM_OFF] = HostExt_NvramState();
  p[TELEM2_DRIVE_OFF] = HostExt_DriveMode();
  p[TELEM2_DEN_OFF]   = HostExt_DriveEnabled();
  p[TELEM2_CROSS_OFF] = (uint8_t)HostExt_TraceCross();
  p[TELEM2_TRIPS_OFF] = HostExt_DriveWatchdogTrips();
  put_i16(&p[TELEM2_DEV1_OFF], (int16_t)Servo_GetDeviation(1));
  put_i16(&p[TELEM2_DEV2_OFF], (int16_t)Servo_GetDeviation(2));
  frame_send(T_TELEM2, 0, p, TELEM2_LEN);
}

/* v2：14 个动作的终点角度（14 × {舵1,舵2}） */
static void send_actions(uint8_t type, uint8_t seq)
{
  uint8_t p[SERVO_ACTION_N * 2u];
  int n, a1, a2;
  for (n = 1; n <= Servo_ActionCount(); n++) {
    a1 = 0; a2 = 0;
    (void)HostExt_ActionGet(n, &a1, &a2);
    if (a1 < 0) a1 = 0;
    if (a1 > 255) a1 = 255;
    if (a2 < 0) a2 = 0;
    if (a2 > 255) a2 = 255;
    p[(n - 1) * 2]     = (uint8_t)a1;
    p[(n - 1) * 2 + 1] = (uint8_t)a2;
  }
  frame_send(type, seq, p, (uint8_t)sizeof(p));
}

/* v2：8 路黑白标定原文（trace_cal_data_t，82 字节） */
static void send_cal_report(uint8_t seq)
{
  trace_cal_data_t cur;
  trace_cal_data_t s0;
  trace_cal_data_t s1;
  trace_cal_data_t other;
  uint8_t active = 0u;
  uint8_t p[1u + sizeof(trace_cal_data_t)];

  trace_cal_get_data(&cur);
  frame_send(T_CAL_REPORT, seq, (const uint8_t *)&cur, (uint8_t)sizeof(cur));

  /* ★2026-10-11：再补一帧把**另一套**也发出去（用户要传感器页同时看两套）。
   * `trace_cal_export()` 按 `s_set` 自动摆位（set0 = 普通 / set1 = 亚克力）并回填当前套号，
   * 所以这两个值就是"哪套是当前"的权威来源。分两帧是因为一帧塞不下 164 字节。 */
  trace_cal_export(&s0, &s1, &active);
  other = (active == 0u) ? s1 : s0;
  p[0] = active;
  memcpy(&p[1], &other, sizeof(other));
  frame_send(T_CAL_REPORT_B, seq, p, (uint8_t)sizeof(p));
}

/* v2：驾驶状态回报（改完/查询都回一帧，上位机不用等下一次遥测） */
static void send_drive_report(uint8_t seq)
{
  uint8_t p[12];
  int a = 0, b = 0;
  (void)HostExt_DriveGet(&a, &b);
  p[0] = HostExt_DriveMode();
  p[1] = HostExt_DriveEnabled();
  put_i16(&p[2], clamp_i16((float)a));
  put_i16(&p[4], clamp_i16((float)b));
  put_i16(&p[6], motor_pwm(TIM_CHANNEL_1, TIM_CHANNEL_2));
  put_i16(&p[8], motor_pwm(TIM_CHANNEL_3, TIM_CHANNEL_4));
  p[10] = (uint8_t)(HostExt_ServoAngle(1) + 0.5f);
  p[11] = (uint8_t)(HostExt_ServoAngle(2) + 0.5f);
  frame_send(T_DRIVE_REPORT, seq, p, 12);
}

/* =====================================================================
 *  收帧状态机（CRC 错只前移 1 字节重同步，上电乱码能自愈）
 * ===================================================================== */
static uint8_t  rx_buf[5u + MAX_PAYLOAD + 2u];
static uint16_t rx_len;
static uint8_t  rx_expect;

static void handle_frame(uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len);

static void rx_reset(void)
{
  rx_len = 0;
  rx_expect = 2;      /* 先要 SOF0 SOF1 */
}

static void rx_byte(uint8_t b)
{
  if (rx_len < 2u) {
    if (rx_len == 0u) {
      if (b == SOF0) rx_buf[rx_len++] = b;
      else s_stats.resyncs++;
    } else {
      if (b == SOF1) rx_buf[rx_len++] = b;
      else { s_stats.resyncs++; rx_len = (b == SOF0) ? 1u : 0u; if (rx_len) rx_buf[0] = SOF0; }
    }
    if (rx_len == 2u) rx_expect = 5u;   /* 再收 type/seq/len */
    return;
  }
  if (rx_len < 5u) {
    rx_buf[rx_len++] = b;
    if (rx_len == 5u) {
      uint8_t plen = rx_buf[4];
      if (plen > MAX_PAYLOAD) {         /* 长度不合理 -> 丢掉这个假头 */
        s_stats.resyncs++;
        rx_len = (b == SOF0) ? 1u : 0u;
        if (rx_len) rx_buf[0] = SOF0;
        rx_expect = 2u;
        return;
      }
      rx_expect = (uint8_t)(5u + plen + 2u);
    }
    return;
  }
  rx_buf[rx_len++] = b;
  if (rx_len >= rx_expect) {
    uint8_t plen = rx_buf[4];
    uint16_t crc_rx = (uint16_t)rx_buf[5u + plen] | ((uint16_t)rx_buf[6u + plen] << 8);
    uint16_t crc_calc = crc16_ccitt(&rx_buf[2], (uint16_t)(3u + plen));
    if (crc_rx == crc_calc) {
      s_stats.rx_frames++;
      handle_frame(rx_buf[2], rx_buf[3], &rx_buf[5], plen);
    } else {
      s_stats.crc_errors++;
    }
    rx_reset();
  }
}

/* =====================================================================
 *  v2：上位机任务程序（T_TASK_*，见 docs/任务编程.md §5）
 *
 *  车只是执行机构：上位机把任务编成 task_prog 的二进制、分片 PUT 上来，
 *  COMMIT 校验通过才擦进 Flash（App/Svc/task_store.c：页 58 目录 + 页 59..61 程序区）。
 *
 *  为什么 LIST_R / DATA 仍然由 poll 分拍推进：早先一帧 board_dl_send 是同步阻塞的
 *  （64 字节数据 + 11 字节帧头 ≈ 6.5ms @115200），一次 GET 最长 TASK_STORE_CAP（4096）
 *  字节 ≈ 64 帧 ≈ 420ms 会按住主循环。现在发是 DMA 槽位队列（非阻塞），但队列只有
 *  8 帧深，一次全倒进去必然丢帧 —— 仍旧挂"待发"标记，每拍推 TASK_TX_PER_POLL 帧。
 *
 *  收尾约定（§5 的语法允许 n=0，桥按这个判"收完了"，文档同一节有写）：
 *    LIST_R：每个在场的槽一帧，最后**必发一帧空载荷**；
 *    DATA  ：从 off=0 起每帧最多 TASK_DATA_CHUNK 字节，最后**必发一帧空数据**。
 *  查询失败（槽是空的 / 分片没传够）不挂待发，直接回 ACK 错误码。
 * ===================================================================== */
#define TASK_TX_PER_POLL 8u     /* 每拍最多推 8 帧（= 发队列深度）：队列满会丢帧，留点余量给遥测 */
#define TASK_DATA_CHUNK  64u    /* 一帧 DATA 的数据字节数（PUT 方向上位机可给到 120） */
#define TASK_STATUS_LEN  11u    /* id(2) state(1) running(1) block(2) elapsed(4) flags(1) */

enum { TASK_TX_NONE = 0u, TASK_TX_LIST, TASK_TX_DATA };

static uint8_t  s_task_tx_kind;
static uint8_t  s_task_tx_seq;
static uint8_t  s_task_tx_slot;
static uint16_t s_task_tx_off;
/* STATUS 推送：内容一变就推，没变 1Hz 心跳（§5） */
static uint8_t  s_status_last[TASK_STATUS_LEN];
static uint8_t  s_status_seen;
static uint32_t s_status_ms;

/* 当前解释器状态打包成 T_TASK_STATUS 载荷。没载入过任何程序时 id=0xFFFF、state=0xFF：
 * 上位机据此显示"车上没有载入任务"，而不是把 0 号槽当成在跑。 */
static void pack_task_status(uint8_t *p)
{
  uint8_t loaded = TaskRun_Loaded();
  put_u16(p, (uint16_t)(loaded ? TaskRun_Id() : 0xFFFFu));
  p[2] = (uint8_t)(loaded ? TaskRun_State() : TASK_RUN_OFF);
  p[3] = TaskRun_IsRunning();
  put_u16(&p[4], TaskRun_Block());
  put_u32(&p[6], TaskRun_ElapsedMs());
  p[10] = TaskRun_Flags();
}

/* 目录一帧：u16 id, u16 len, u16 crc, u8 flags, u8 name_len, name...（§5）。
 * 缓冲多留 1 字节：TaskStore_Name() 会在名字后面补 '\0'。 */
static void send_task_list_one(uint8_t seq, uint8_t slot)
{
  uint8_t p[8u + TASK_STORE_NAME_MAX + 1u];
  uint8_t n = TaskStore_Name(slot, (char *)&p[8], (uint8_t)(TASK_STORE_NAME_MAX + 1u));
  put_u16(&p[0], (uint16_t)slot);
  put_u16(&p[2], TaskStore_Len(slot));
  put_u16(&p[4], TaskStore_Crc(slot));
  p[6] = TaskStore_Flags(slot);
  p[7] = n;
  frame_send(T_TASK_LIST_R, seq, p, (uint8_t)(8u + n));
}

/* 读回一帧：u16 id, u16 off, 数据（0 字节 = 整份发完了）。返回本帧带的数据字节数 */
static uint8_t send_task_data_one(uint8_t seq, uint8_t slot, uint16_t *off)
{
  uint8_t p[4u + TASK_DATA_CHUNK];
  const uint8_t *img = TaskStore_Image(slot);
  uint16_t len = TaskStore_Len(slot);
  uint16_t n = 0u;

  put_u16(&p[0], (uint16_t)slot);
  put_u16(&p[2], *off);
  if (img != NULL && *off < len) {
    n = (uint16_t)(len - *off);
    if (n > TASK_DATA_CHUNK) n = TASK_DATA_CHUNK;
    memcpy(&p[4], &img[*off], n);
    *off = (uint16_t)(*off + n);
  }
  frame_send(T_TASK_DATA, seq, p, (uint8_t)(4u + n));
  return (uint8_t)n;
}

/* 任务回包的剩余部分：每拍一帧（与参数 dump 同一个套路）。
 * 计数只算**真发出去的帧**：空槽只是跳过，不算占用配额 —— 否则空目录要把
 * 3 个空槽走完才发收尾帧，白白多等 3 拍。 */
static void task_tx_poll(void)
{
  uint8_t n = 0u;

  while (s_task_tx_kind != TASK_TX_NONE && n < TASK_TX_PER_POLL) {
    if (s_task_tx_kind == TASK_TX_LIST) {
      if (s_task_tx_slot < TASK_STORE_SLOTS) {
        if (TaskStore_Present(s_task_tx_slot) != 0u) {
          send_task_list_one(s_task_tx_seq, s_task_tx_slot);
          n++;
        }
        s_task_tx_slot++;
      } else {
        frame_send(T_TASK_LIST_R, s_task_tx_seq, NULL, 0u);   /* 收尾帧：目录发完 */
        s_task_tx_kind = TASK_TX_NONE;
        n++;
      }
    } else {
      if (send_task_data_one(s_task_tx_seq, s_task_tx_slot, &s_task_tx_off) == 0u) {
        s_task_tx_kind = TASK_TX_NONE;                        /* 收尾帧已发 */
      }
      n++;
    }
  }
}

/* 变化判据**刻意跳过 elapsed_ms**（载荷里第 6..9 字节）：它在跑的时候每毫秒都在变，
 * 整帧 memcmp 会把"变了就推"退化成"每一拍都推"。2026-10-04 真车实测（任务自启在跑）：
 * 链路被灌到 338 帧/s（遥测 100 + status 约 240），230400 的全部带宽被吃满，
 * 桥侧 8 秒丢 1904 帧、SSE 341 事件/s 里 1922 个是 task。现在 id/state/running/
 * block/flags 一变才推，elapsed 靠 1Hz 心跳刷新（上位机本来就是 1Hz 看进度）。 */
static uint8_t status_changed(const uint8_t *a, const uint8_t *b)
{
  uint8_t i;

  for (i = 0u; i < TASK_STATUS_LEN; i++) {
    if (i >= 6u && i <= 9u) continue;          /* elapsed_ms：不参与判变化 */
    if (a[i] != b[i]) return 1u;
  }
  return 0u;
}

/* 状态推送：内容一变就推，没变 1Hz 心跳 */
static void task_status_poll(uint32_t now_ms)
{
  uint8_t st[TASK_STATUS_LEN];

  pack_task_status(st);
  if (s_status_seen == 0u || status_changed(st, s_status_last) != 0u ||
      (uint32_t)(now_ms - s_status_ms) >= 1000u) {
    memcpy(s_status_last, st, TASK_STATUS_LEN);
    s_status_seen = 1u;
    s_status_ms = now_ms;
    frame_send(T_TASK_STATUS, 0u, st, TASK_STATUS_LEN);
  }
}

static void handle_frame(uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len)
{
  /* 任何一帧合法帧都算"链路还活着"：驾驶时靠它喂看门狗，500ms 收不到就自动停车
   * （HostExt_Task 在 host_link_poll 里查，见 host_ext.c） */
  HostExt_DriveKeepAlive(HAL_GetTick());

  switch (type) {
  case T_HELLO_REQ:
    send_hello(seq);
    break;

  case T_PARAM_GET: {
    uint16_t id;
    if (len < 2u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (id == 0xFFFFu) {
      /* 全表 dump 不在这里连发：原来循环 31 次 send_param_report，当时发还是同步阻塞的
       *（115200 下一帧 ≈1.1ms）⇒ 主循环停约 35ms，期间 UI 不刷、遥测不发。
       * 现在发是 DMA 槽位队列（非阻塞），但队列只有 8 帧深，一次倒 31 帧照样丢 ——
       * 仍旧挂个"待发"标记，由 host_link_poll 每拍发几帧。 */
      s_dump_seq  = seq;
      s_dump_i    = 0;
      s_dump_busy = 1u;
    } else {
      int i = spec_index(id);
      if (i < 0) send_ack(seq, ST_BAD_PARAM);
      else send_param_report(seq, id, s_param[i]);
    }
    break;
  }

  case T_PARAM_SET: {
    uint16_t id;
    uint32_t bits;
    float v;
    int i;
    if (len < 6u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    bits = (uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16)
         | ((uint32_t)p[5] << 24);
    memcpy(&v, &bits, 4);
    i = spec_index(id);
    if (i < 0) { send_ack(seq, ST_BAD_PARAM); break; }
    /* 用取反形式：NaN 与任何数比较都是 false，写成 !(v >= min && v <= max) 才能
     * 把 NaN/±Inf 挡住（原来的 v < min || v > max 会让 NaN 通过范围检查）。 */
    if (!(v >= SPECS[i].min && v <= SPECS[i].max)) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    s_param[i] = v;

    /* ★2026-10-13（审计 #10，high）：**把 g_cfg 镜像一起同步**。
     *
     *  这 16 项参数有**两个写者** —— 上位机走这里写参数表 `s_param[]`，而车上菜单写的是
     *  `g_cfg` 里 ×100 的整数镜像（app_ui.c 的 push/pull_pid_params）。但 **nvram 固化时存的是
     *  `g_cfg`**（nvram.c 的 snapshot() 读 g_cfg.xxx）。
     *
     *  原来的语义是"上位机 SET 的参数要**下次开机**才同步进 UI"（app_ui.c 那句注释），于是：
     *      网页改参数 → 固化 → nvram 存的是**没变过的旧 g_cfg** → 断电重启还是旧值 ✗
     *  而协议层**无条件回 ST_OK**，用户完全看不出没存进去。
     *
     *  修法：SET 的当下就把镜像改成同一个值 ⇒ 固化拿到的就是新值，不需要重启。
     *  量化口径与 app_ui.c 的 pull_pid_params() 一字不差（×100 / ×1000 / 四舍五入）。
     *  没有镜像的 id 落到 default，什么都不做。
     *
     *  ⚠ 只加在**这里**：本文件另一个写 s_param 的地方是 `host_param_set_local()`，
     *     它会被 app_ui 的 `push_pid_params()` 调用 ⇒ 在那儿加会造成 g_cfg↔参数表 互相写。 */
    switch (id) {
    case HP_BASE_SPEED:     g_cfg.trace_speed          = (int32_t)(v + 0.5f); break;
    case HP_SPEED_KP:       g_cfg.speed_kp_x100        = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_SPEED_KI:       g_cfg.speed_ki_x100        = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_SPEED_KD:       g_cfg.speed_kd_x100        = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_SPEED_ILIMIT:   g_cfg.speed_ilim           = (int32_t)(v + 0.5f); break;
    case HP_SPEED_FF_SLOPE: g_cfg.speed_ff_slope_x1000 = (int32_t)(v * 1000.0f + 0.5f); break;
    case HP_SPEED_FF_FRIC:  g_cfg.speed_ff_fric        = (int32_t)(v + 0.5f); break;
    case HP_LINE_KP:        g_cfg.line_kp_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_LINE_KI:        g_cfg.line_ki_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_LINE_KD:        g_cfg.line_kd_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_LINE_ILIMIT:    g_cfg.line_ilim            = (int32_t)(v + 0.5f); break;
    case HP_GYRO_KP:        g_cfg.gyro_kp_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_GYRO_KI:        g_cfg.gyro_ki_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_GYRO_KD:        g_cfg.gyro_kd_x100         = (int32_t)(v * 100.0f + 0.5f); break;
    case HP_GYRO_ILIMIT:    g_cfg.gyro_ilim            = (int32_t)(v + 0.5f); break;
    case HP_TRACE_CORNER:   g_cfg.trace_corner         = (v >= 0.5f); break;
    default: break;   /* 其余参数没有 g_cfg 镜像，走参数表自己那条持久化路 */
    }
    s_stats.param_sets++;
    send_ack(seq, ST_OK);
    send_param_report(seq, id, v);       /* 回读，上位机立刻看到生效值 */
    break;
  }

  case T_PARAM_SAVE:
    /* v2：真的固化（payload v5 已含菜单全部设置 + 控制环标定量 + 舵机动作表）。
     * nvram_touch() 只是"立刻排一次保存"；真正擦写仍要等车停稳、屏不忙、
     * 1.5s 防抖到期（nvram_poll 决定），所以这里回 OK 并说明是"排队中"。 */
    (void)HostExt_SaveParams();
    send_ack(seq, ST_OK);
    host_link_send_text("[fw] param save queued (writes when idle)\r\n");
    break;

  case T_CMD: {
    uint8_t cmd = (len >= 1u) ? p[0] : 0u;
    int32_t arg = 0;
    if (len >= 5u) {
      arg = (int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8)
                    | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24));
    } else if (len >= 2u) {
      /* 老上位机只发 1 字节 arg（载荷 2 字节）：**别当成 0**。2026-10-04 真车实测踩过：
       * 旧桥这么发时 arg 恒为 0，下面 CMD_SET_TELEM_HZ 的 `arg<1 → 1` 把遥测钳成
       * 1Hz（从 75 帧/s 掉到 1 帧/s）—— 网页上拖一下"遥测频率"滑块就把链路拖哑了。
       * 取无符号低字节：带 arg 的命令只有 SET_TELEM_HZ(1..200) 与 SET_TELEM_MODE(0..2)。 */
      arg = (int32_t)p[1];
    }
    switch (cmd) {
    case CMD_STOP:
      /* 急停：松驾驶 + 停任务解释器（红色停止按钮必须能停住正在跑的任务） */
      g_stop_src[3]++;                         /* 诊断：CMD_STOP 停的 */
      HostExt_DriveStop();
      TaskRun_Stop();
      send_ack(seq, ST_OK);
      break;
    case CMD_START_A:
    case CMD_START_B:
    case CMD_START_C:
      /* 旧板内置比赛流程（renwu.c）已删除。这里**不能**再假装成功：假装成功会让
       * 上位机以为车在跑流程，而实际一个电机都不会动。明说"不支持 + 该去哪儿"。 */
      send_ack(seq, ST_UNSUPPORTED);
      host_link_send_text("[fw] 内置任务流程已移除：请用任务页（0x67 RUN）跑 Flash 里的任务\r\n");
      break;
    case CMD_CLEAR_FAULT: Motion_ClearFault(); send_ack(seq, ST_OK); break;
    case CMD_SAVE_PARAMS: (void)HostExt_SaveParams(); send_ack(seq, ST_OK); break;
    case CMD_SET_TELEM_HZ:
      if (arg < 1) arg = 1;
      if (arg > 200) arg = 200;
      s_stats.telem_hz = (uint16_t)arg;
      send_ack(seq, ST_OK);
      break;
    case CMD_SET_TELEM_MODE:
      if (arg < 0) arg = 0;
      if (arg > 2) arg = 2;
      s_telem_mode = (uint8_t)arg;
      send_ack(seq, ST_OK);
      break;
    case CMD_RESET:       send_ack(seq, ST_OK); HAL_Delay(5); NVIC_SystemReset(); break;
    /* 下面几个还没实现，明确回"未就绪"，不假装成功 */
    case CMD_CALIB_GYRO:
    case CMD_SET_LOGGING:
    default:
      send_ack(seq, (cmd == CMD_CALIB_GYRO || cmd == CMD_SET_LOGGING)
                     ? ST_NOT_READY : ST_BAD_CMD);
      break;
    }
    break;
  }

  case T_CAL_GET:
    send_cal_report(seq);
    break;

  case T_ACTIONS_GET:
    send_actions(T_ACTIONS, seq);
    break;

  case T_ACTION_SET: {
    int n, a1, a2;
    if (len < 3u) { send_ack(seq, ST_BAD_PARAM); break; }
    n  = (int)p[0];
    a1 = (int)p[1];
    a2 = (int)p[2];
    if (n < 1 || n > Servo_ActionCount()) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    if (a1 > 180 || a2 > 180) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    (void)HostExt_ActionSet(n, a1, a2);
    /* 改完就掰到该动作的终点：用户在台架上要"眼看着角度变"（m13466 的场景） */
    (void)HostExt_ActionPlay(n);
    send_ack(seq, ST_OK);
    send_actions(T_ACTIONS, seq);
    break;
  }

  case T_ACTION_SAVE:
    (void)HostExt_ActionSave();
    send_ack(seq, ST_OK);
    break;

  case T_SERVO_SET: {
    int ch, deg;
    if (len < 3u) { send_ack(seq, ST_BAD_PARAM); break; }
    ch  = (int)p[0];
    deg = (int)(int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
    if (ch < 1 || ch > 2) { send_ack(seq, ST_BAD_PARAM); break; }
    if (deg < 0 || deg > 180) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    (void)HostExt_ServoSet(ch, deg);
    send_ack(seq, ST_OK);
    break;
  }

  case T_SERVO_PLAY: {
    int n = (len >= 1u) ? (int)p[0] : 0;
    if (n == 0) { (void)HostExt_ServoCenter(); send_ack(seq, ST_OK); break; }
    if (HostExt_ActionPlay(n) == 0) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    send_ack(seq, ST_OK);
    break;
  }

  case T_DRIVE: {
    uint8_t mode;
    int a, b, rc;
    if (len < 5u) { send_ack(seq, ST_BAD_PARAM); break; }
    mode = p[0];
    a = (int)(int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
    b = (int)(int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
    rc = HostExt_DriveSet(mode, a, b);
    if (rc == -2) {
      /* 任务流程在跑：抢电机会出事故，明确拒绝 */
      send_ack(seq, ST_NOT_READY);
      host_link_send_text("[fw] drive refused: task running\r\n");
    } else if (rc < 0) {
      send_ack(seq, ST_BAD_PARAM);
    } else {
      send_ack(seq, ST_OK);
    }
    send_drive_report(seq);
    break;
  }

  case T_DRIVE_REPORT:
    send_drive_report(seq);
    break;

  case T_SERVO_DEV: {
    /* 舵机软件中位偏差：调装配误差用，范围 ±100µs（Servo_SetDeviation 自己也会夹） */
    int ch, dev;
    if (len < 3u) { send_ack(seq, ST_BAD_PARAM); break; }
    ch  = (int)p[0];
    dev = (int)(int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
    if (ch < 1 || ch > 2) { send_ack(seq, ST_BAD_PARAM); break; }
    if (dev < -100 || dev > 100) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    (void)HostExt_ServoDeviation(ch, dev);
    send_ack(seq, ST_OK);
    break;
  }

  /* ---------------- v2：上位机任务程序（§5，0x60..0x69） ---------------- */

  case T_TASK_LIST:
    /* 目录：只挂待发，由 poll 一帧帧发（一槽一帧 + 一帧空的收尾） */
    s_task_tx_seq  = seq;
    s_task_tx_slot = 0u;
    s_task_tx_kind = TASK_TX_LIST;
    break;

  case T_TASK_PUT: {
    uint16_t id, off;
    if (len < 5u) { send_ack(seq, ST_BAD_PARAM); break; }
    id  = rd_u16(p);
    off = rd_u16(&p[2]);
    if (id >= TASK_STORE_SLOTS) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    /* 只写 RAM 暂存，不碰 Flash ⇒ 车在动也允许（真正擦写要等 COMMIT） */
    if (TaskStore_Stage(off, &p[4], (uint16_t)(len - 4u)) != TASK_STORE_OK) {
      send_ack(seq, ST_OUT_OF_RANGE);        /* off + 本片 越过 TASK_STORE_CAP */
      break;
    }
    send_ack(seq, ST_OK);
    break;
  }

  case T_TASK_COMMIT: {
    uint16_t id, tlen, tcrc;
    uint8_t  flags = 0u;
    uint8_t  name_len = 0u;
    const char *name = NULL;
    int rc;
    if (len < 6u) { send_ack(seq, ST_BAD_PARAM); break; }
    id   = rd_u16(p);
    tlen = rd_u16(&p[2]);
    tcrc = rd_u16(&p[4]);
    if (id >= TASK_STORE_SLOTS) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    if (len > 6u) {
      /* §5：p[6] = flags（**保留位**：曾经 bit0 = 开机自启，用户 m24999 删了那个功能，
       * 现在车上不解释任何位，原样交给存储层存进目录） */
      flags = p[6];
    }
    if (len > 7u) {
      /* 超集：p[7] = u8 name_len，其后是名字（UTF-8，不补 '\0'）。桥只发 6/7 字节也认。 */
      name_len = p[7];
      if (name_len > TASK_STORE_NAME_MAX || (uint16_t)(8u + (uint16_t)name_len) > (uint16_t)len) {
        send_ack(seq, ST_BAD_PARAM);
        break;
      }
      name = (const char *)&p[8];
    }
    if (TaskRun_IsRunning() != 0u) {
      /* 解释器正在跑：擦一页会把 CPU 按住几十毫秒，正在跑的控制环直接失控 */
      send_ack(seq, ST_BUSY);
      break;
    }
    rc = TaskStore_Commit((uint8_t)id, tlen, tcrc, flags, name, name_len);
    if (rc == TASK_STORE_OK) {
      send_ack(seq, ST_OK);
    } else {
      send_ack(seq, (rc == TASK_STORE_E_BUSY) ? ST_BUSY : ST_BAD_PARAM);
    }
    break;
  }

  case T_TASK_GET: {
    uint16_t id;
    if (len < 2u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = rd_u16(p);
    if (id >= TASK_STORE_SLOTS) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    if (TaskStore_Present((uint8_t)id) == 0u) { send_ack(seq, ST_NOT_READY); break; }
    /* 成功时不先回 ACK：一帧帧 DATA 才是应答（桥按"数据为空的那帧"收尾） */
    s_task_tx_seq  = seq;
    s_task_tx_slot = (uint8_t)id;
    s_task_tx_off  = 0u;
    s_task_tx_kind = TASK_TX_DATA;
    break;
  }

  case T_TASK_DEL: {
    uint16_t id;
    int rc;
    if (len < 2u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = rd_u16(p);
    if (id >= TASK_STORE_SLOTS) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    if (TaskRun_IsRunning() != 0u) { send_ack(seq, ST_BUSY); break; }
    rc = TaskStore_Erase((uint8_t)id);
    send_ack(seq, (rc == TASK_STORE_OK) ? ST_OK
                 : ((rc == TASK_STORE_E_BUSY) ? ST_BUSY : ST_BAD_PARAM));
    break;
  }

  case T_TASK_RUN: {
    uint16_t id;
    uint8_t  from = 1u;          /* ★可选的第三字节：从第几次分派开始（1 起算） */
    int rc;
    if (len < 2u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = rd_u16(p);
    /* ★2026-10-11（用户）：老上位机只发 2 字节、老固件也只读 2 字节 ⇒ 两边都向后兼容。
     *   1 = 正常（等价于没有这个字节）；2 = 略过第 1 条判断、从第 2 条开始，以此类推。
     *   只在这一次 RUN 里生效，不写 NVRAM —— 车自己中键启动那条路永远从第 1 次。 */
    if (len >= 3u) { from = p[2]; }
    if (id >= TASK_STORE_SLOTS) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    if (TaskRun_IsRunning() != 0u) {
      send_ack(seq, ST_BUSY);                /* 已经有一个任务占着电机：先 0x68 STOP */
      break;
    }
    if (HostExt_DriveEnabled() != 0u) {
      send_ack(seq, ST_BUSY);                /* 正在被上位机驾驶：先松手再交任务 */
      host_link_send_text("[fw] task run refused: host driving\r\n");
      break;
    }
    rc = TaskStore_Load((uint8_t)id);        /* 校验格式 + 拷进 RAM（运行期不碰 Flash） */
    if (rc != 0) {
      send_ack(seq, (rc == TASK_STORE_E_EMPTY) ? ST_NOT_READY
                   : ((rc == TASK_STORE_E_BUSY) ? ST_BUSY : ST_BAD_PARAM));
      break;
    }
    if (TaskRun_StartFrom(id, from) != 0) { send_ack(seq, ST_BAD_PARAM); break; }
    send_ack(seq, ST_OK);
    break;
  }

  case T_TASK_STOP:
    TaskRun_Stop();                          /* 幂等，且会停车 */
    send_ack(seq, ST_OK);
    break;

  default:
    send_ack(seq, ST_UNKNOWN_TYPE);
    break;
  }
}

/* =====================================================================
 *  公共接口
 * ===================================================================== */
void host_link_init(void)
{
  uint16_t i;
  for (i = 0; i < NSPEC; i++) s_param[i] = SPECS[i].def;
  memset(&s_stats, 0, sizeof(s_stats));
  /* 遥测默认 200Hz（5ms 一格，与控制环同拍）：链路 2026-10-05 提到 921600 之后
   * 200Hz 只占约 15% 带宽（100Hz 时约 8%），用户 m30228 的预期也是 200Hz。
   * 上位机可以用 CMD_SET_TELEM_HZ 随时改（网页「控制 → 遥测频率」的滑块，范围 1..200Hz）；
   * 注意这个值**只存在 RAM**，每次复位/烧录都回到这里写的默认值（以前是 100，所以
   * 「设过 200、烧完变成 100」是正常的，不是没生效）。 */
  s_stats.telem_hz = 200u;
  s_telem_mode = 1u;          /* 默认发 64 字节 T_TELEM2（新上位机要扩展量） */
  HostExt_Init();

  rx_reset();
  (void)board_dl_init();
  board_dl_start_rx();
  (void)ina226_init();
}

void host_link_poll(uint32_t loop_us)
{
  uint8_t b;
  static uint32_t next_tx;
  uint32_t now = HAL_GetTick();

  while (board_dl_get_byte(&b)) rx_byte(b);

  /* 驾驶看门狗：上位机/串口断了就把车停下（500ms 没有合法帧） */
  HostExt_Task(now);

  if (s_stats.telem_hz) {
    uint32_t period = 1000u / s_stats.telem_hz;
    if (period == 0u) period = 1u;
    if ((int32_t)(now - next_tx) >= 0) {
      /* 落后太多（被写 Flash / 刷屏卡住过）：重新对齐，不吐一串补帧 */
      if ((int32_t)(now - (next_tx + 2u * period)) > 0) next_tx = now;
      /* ★ 累加，不是"用 now 重算"：主循环一拍只有几毫秒，重算会把实际周期拉到拍频的
       *   整数倍上 —— 2026-10-04 真车实测：要 100Hz 却只出 75.5 帧/s（230400 下量到
       *   的 11236 字节/2s 也正好对上）。上位机 `bridge/sim.py:142-146` 早就是累加写法。 */
      next_tx += period;
      if (s_telem_mode != 0u) send_telemetry2(loop_us);
      if (s_telem_mode != 1u) send_telemetry(loop_us);
    }
  }

  /* 全表参数 dump 的剩余部分：每拍发几帧，避免一次性阻塞主循环几十毫秒 */
  if (s_dump_busy) {
    uint8_t n = 0u;
    while (s_dump_busy && n < DUMP_PER_POLL) {
      send_param_report(s_dump_seq, SPECS[s_dump_i].id, s_param[s_dump_i]);
      s_dump_i++;
      n++;
      if (s_dump_i >= NSPEC) s_dump_busy = 0u;
    }
  }

  /* 任务程序回包（目录/读回）同样分帧，每拍一帧；状态推送按"变了就发 + 1Hz 心跳" */
  task_tx_poll();
  task_status_poll(now);
}

void host_link_tick_1hz(void)
{
  static uint32_t last;
  uint32_t now = HAL_GetTick();
  if (now - last < 1000u) return;
  last = now;
  /* 2026-10-15：把"谁停了驾驶会话"每秒发一行到串口（不用调试器就能读）。 */
  {
    char tb[96];
    (void)snprintf(tb, sizeof(tb), "[fw] stops low=%lu wd=%lu off=%lu cmd=%lu\r\n",
                   (unsigned long)g_stop_src[0], (unsigned long)g_stop_src[1],
                   (unsigned long)g_stop_src[2], (unsigned long)g_stop_src[3]);
    host_link_send_text(tb);
  }

  /* ★探针（2026-10-15）：角度环"连续域航向 vs 真实航向"的偏差 —— 实测偏差 29~77°
   * 导致"以为没到位、继续推、实际已冲过头"。yaw=实时取模航向，yawcont=环内累加值，
   * tgt=环内目标（取模），err=tgt-yawcont（连续域误差）。 */
  {
    char yb[96];
    (void)snprintf(yb, sizeof(yb), "[fw] yaw=%d yawcont=%d tgt=%d err=%d\r\n",
                   (int)Yaw, (int)GyroControl_GetYawCont(),
                   (int)GyroControl_GetTargetAngle(),
                   (int)(GyroControl_GetTargetAngle() - GyroControl_GetYawCont()));
    host_link_send_text(yb);
  }

  /* ★探针：速度环三段 + TIM8 寄存器（查"右路负值无输出"用）。 */

  {
    char pb[160];
    (void)snprintf(pb, sizeof(pb),
      "[fw] sp en=%d reqL=%d reqR=%d outL=%d outR=%d loadA=%d loadB=%d regL=%d regR=%d\r\n",
      SpeedControl_IsEnabled(), SpeedControl_Target(0), SpeedControl_Target(1),
      SpeedControl_Output(0), SpeedControl_Output(1),
      g_load_a, g_load_b,
      (int)motor_pwm(TIM_CHANNEL_1, TIM_CHANNEL_2),
      (int)motor_pwm(TIM_CHANNEL_3, TIM_CHANNEL_4));
    host_link_send_text(pb);
  }

  SEGGER_RTT_printf(0, "HOST: rx=%lu tx=%lu crc=%lu resync=%lu sets=%lu hz=%u txdrop=%lu rxover=%lu\r\n",
                    (unsigned long)s_stats.rx_frames, (unsigned long)s_stats.tx_frames,
                    (unsigned long)s_stats.crc_errors, (unsigned long)s_stats.resyncs,
                    (unsigned long)s_stats.param_sets, (unsigned)s_stats.telem_hz,
                    (unsigned long)board_dl_tx_dropped(), (unsigned long)board_dl_rx_overrun());
}

const host_link_stats_t *host_link_stats(void) { return &s_stats; }

/* =====================================================================
 *  v2：给上位机/固化用的参数表元数据与驾驶状态查询
 *  （上位机自己也有一份"哪个 id 叫什么/单位/小数位"的表，两边靠 HELLO 里的
 *   param_crc 对齐；tools/console/tests 会解析本文件的 SPECS[] 断言 id 不漂）
 * ===================================================================== */
uint16_t host_param_count(void)
{
  return (uint16_t)NSPEC;
}

uint16_t host_param_id_at(uint16_t idx)
{
  return (idx < NSPEC) ? SPECS[idx].id : 0u;
}

int host_param_bounds(uint16_t id, float *def, float *min, float *max)
{
  int i = spec_index(id);
  if (i < 0) return 0;
  if (def != 0) *def = SPECS[i].def;
  if (min != 0) *min = SPECS[i].min;
  if (max != 0) *max = SPECS[i].max;
  return 1;
}

uint16_t host_param_table_crc(void)
{
  return (uint16_t)param_table_crc();
}

uint8_t host_drive_mode(void)
{
  return HostExt_DriveMode();
}

uint8_t host_drive_is_enabled(void)
{
  return HostExt_DriveEnabled();
}
