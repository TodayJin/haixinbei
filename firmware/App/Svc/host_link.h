/**
 * @file  host_link.h
 * @brief 上位机通信链路（LPUART1 <-> 无线下载器）：协议编解码 + 参数表 + 命令分发。
 *
 * 协议全文见 docs/上位机通信协议.md（帧 = A5 5A | type | seq | len | payload | CRC16）。
 */
#ifndef __HOST_LINK_H__
#define __HOST_LINK_H__

#include <stdint.h>

/* ---------------- 参数 id（与上位机 protocol.py 的 PARAMS 表逐项对应） ---------- */
#define HP_LINE_KP           0x0201u
#define HP_LINE_KI           0x0202u
#define HP_LINE_KD           0x0203u
#define HP_LINE_ILIMIT       0x0204u
#define HP_BASE_SPEED        0x0301u
#define HP_BASE_SPEED_SLOW   0x0302u
/* 任务解释器的**默认直行基准速度**：TaskRun_Start() 启动时用它设一次速度环目标。
 * 来历：原来这个参数是旧板内置比赛流程（renwu.c，从旧板工程照抄）的直行速度，里面的
 * 900/920/935 是**旧板刻度**（旧板 PWM 满量程 1000），本板满量程 7200，所以这个参数 =
 * "旧板 900"对应的本板 PWM，默认 6480（90%）。旧流程删了，参数保留 —— 任务块里没显式
 * 给速度时，车就按这个基准跑。 */
#define HP_TASK_SPEED        0x0304u
/* 速度环（目标转速 -> PWM）。环已在 speed_control.c 实现，这四项是 PI 增益。 */
#define HP_SPEED_KP          0x0601u
#define HP_SPEED_KI          0x0602u
#define HP_SPEED_KD          0x0603u
#define HP_SPEED_ILIMIT      0x0604u

/* 速度环前馈：out = slope*target ± fric + PI。fric 是摩擦等效压降（PWM 常量），
 * 高速精度主要靠它，PI 只修残差（目标 5000 时 PI 稳态贡献≈0）。
 * 这两个数是 2026-09-30 台架实测拟合出来的（稳态 PWM ≈ 0.662×目标 + 2327），
 * 属于**硬件标定量**：换电机/减速比/电池电压必须重标。
 * 它们同时是 speed_control.c 的兜底值，所以编译期
 * -DSPEED_FF_SLOPE=0.80f -DSPEED_FF_FRIC=0.0f 仍然能精确退化成改动前的老行为（A/B 用）。 */
#ifndef SPEED_FF_SLOPE
#define SPEED_FF_SLOPE       0.662f
#endif
#ifndef SPEED_FF_FRIC
#define SPEED_FF_FRIC        2327.0f
#endif
#define HP_SPEED_FF_SLOPE    0x0605u
#define HP_SPEED_FF_FRIC     0x0606u
#define HP_GYRO_KP           0x0101u
#define HP_GYRO_KI           0x0102u
#define HP_GYRO_KD           0x0103u
#define HP_GYRO_ILIMIT       0x0104u
#define HP_TURN_ANGLE_OFFSET 0x0105u
#define HP_TURN_TOLERANCE    0x0401u
#define HP_TURN_TIMEOUT      0x0402u
#define HP_YAW_RATE_DAMP     0x0403u
#define HP_MARKER_HOLD       0x0404u
#define HP_MARKER_MIN_GAP    0x0405u
#define HP_COUNTS_PER_REV    0x0305u
#define HP_WHEEL_DIA         0x0306u
#define HP_WHEEL_BASE        0x0307u
/* ★2026-10-14：**满 PWM 时每 5ms 的期望编码器计数** —— 速度环把"PWM 刻度目标"换算成
 * "期望计数"用的就是这个刻度（speed_control.c 的 speed_pi_step）。
 *
 * 为什么必须做成参数：它原来是编译期常数 `SPEED_FULL_COUNTS = 25.0f`，来源是**旧电机**
 *   （源码注释：「12V 空载 150rpm -> 4950 计数/s -> 每 5ms 约 25 个计数」）。
 *   用户 2026-10-14 换了电机后实测：目标 4000 与 6000 的**稳态速度几乎一样（288 / 279 mm/s）**
 *   ⇒ 速度**物理饱和**，而环想要的反馈（4000 ⇒ 13.9 计数/5ms、6000 ⇒ 20.8）里，
 *   6000 那个值**永远够不到** ⇒ 环满 PWM 输出、完全失去调节能力。
 *   实测反推：287.9 mm/s ⇒ 13.98 计数/5ms ⇒ **环其实精确跟踪了它的目标（误差 0.6%）**，
 *   是"目标→计数"这个刻度错了 ⇒ 观感就是"PID 完全不对"。
 *
 * 做成参数后：换电机/换减速比只需在上位机改一个数，**不必重烧固件**。
 * 标定法：wheels 模式给一个**够得到**的目标 T，读稳态 vl(mm/s)，则
 *     full_counts = (vl/1000 × 5 / (π×wheel_dia) × counts_per_rev) × 7200 / T
 * ⚠ 用 mm/s 反推前必须先确认 counts_per_rev / wheel_dia 是准的（否则是循环论证）：
 *   手推车走一段已知距离，比对"固件积分出的距离"与卷尺读数。 */
#define HP_SPEED_FULL_COUNTS 0x0308u
#define HP_PWM_MAX           0x0303u
#define HP_VBAT_NOMINAL      0x0501u
#define HP_STALL_CURRENT     0x0502u
#define HP_VBAT_LOW          0x0503u
#define HP_GYRO_SETTLE_RATE  0x0117u /* ★到位锁存的角速度阈值(°/s)，默认 30。
                                      * 锁存判据是 |err|<settle_deg 且 |v|<settle_rate 持续 settle_ms。
                                      * 打开 v_floor 后 v 一直 ≥floor ⇒ 若本阈值仍 ≤floor 则**永远锁不上**
                                      * ⇒ 车被 floor 顶着在目标附近来回微摆（实测抖动 13~22°）⇒ 必须配套放大。 */
#define HP_GYRO_V_FLOOR      0x0116u /* ★逼近段最低允许角速度(°/s)，0=关(默认)。
                                      * 用途：原地转要"维持转动"需 ~90~100°/s，低于它车会在离目标 10~40° 处
                                      * 咬死停住(欠调)；而 P 型外环 err 小 ⇒ 命令也小 ⇒ 必然进慢滚区。
                                      * 打开它(35~60)可让全程不进 stick-slip 区，最后靠停车带一次性刹停。 */
#define HP_GYRO_BRAKE_GAIN   0x0115u /* ★刹车增强系数（>=1）：速率环里"该减速"时把负向误差放大。
                                      * 实测超调根因：该刹车时 (rate_ref-v) 反而变小(仅 ~300) 而需要 ~3000。
                                      * 1.0 = 关闭（与旧行为逐位一致）；默认 2.0。 */

/* ---------------- v2 追加参数：以前硬编码在控制代码里的标定量 ----------------
 * 加这些是因为用户 m13466 要求"PID 页面每个环的参数都能显示、能调、能保存、能固化到
 * 小车 Flash"。这些量的真值从此都在参数表里，控制代码用 host_param_get(id, 兜底)
 * 取；兜底值 = 之前硬编码的实测值，所以不接上位机时行为完全不变。 */
#define HP_GYRO_ANG_KP       0x010Au  /* 角度环外环：角度误差 -> 期望角速度 */
#define HP_GYRO_RATE_KP      0x010Bu  /* 角度环内环：角速度误差 -> 修正量 */
#define HP_GYRO_RATE_KD      0x010Cu  /* 角度环内环微分阻尼 */
#define HP_GYRO_RATE_MAX     0x010Du  /* 期望角速度上限 °/s */
#define HP_GYRO_CORR_LIMIT   0x010Eu  /* 修正量限幅（PWM 刻度） */
#define HP_GYRO_CORR_SLEW    0x010Fu  /* 修正量斜率限幅（PWM/s） */
#define HP_GYRO_LATCH_MOVING 0x011Au  /* ★2026-10-18 行进中是否允许"到位锁存"。
                                       * 1（默认）= 允许（旧行为）：直行时 150ms 内 |err|<settle_deg
                                       *   就锁存、correction=0，角度环不干预（车按机械不对称往右偏）；
                                       * 0 = 行进中不许锁存：角度环一直闭环去纠偏。
                                       * 现场用 0x011A 一条命令对比，不用重烧。 */
#define HP_GYRO_CORR_DUTY    0x011Cu /* 2026-10-19 出力占空比 %（脉冲驱动），0~100。
                                      * 100 = 关（默认，行为与以前逐位一致）。
                                      * <100 = 每 100ms 周期内只在前 duty% 的拍给 corr，其余给 0。
                                      * 为什么需要：corr 上限 = rate_kp*rate_max，而本车电机起转要≈3450、
                                      * 维持只要≈1000 ⇒ 把 rate_max 压小会让环路推不动轮子、转动全靠起转助推
                                      * bang-bang 地踢（实测角速度会超过 rate_max、曲线不线性、趟间差异巨大）。
                                      * 正确做法：rate_max 保持够大（corr 能过起转阈值），再用占空比降低平均推力。
                                      * 建议先试 40~70。 */
#define HP_GYRO_APPROACH_ACC 0x011Bu /* 2026-10-19 刹车曲线（梯形速度规划）减速度 deg/s^2。
                                      * 0 = 关（纯比例 rate_ref = ang_kp*err，与以前逐位一致）。
                                      * >0 = rate_ref = sign(err)*min(rate_max, sqrt(2*a*|err|))
                                      *   => 远距离恒为 rate_max（真·线性），接近目标时按
                                      *      「刚好能停住」的速度走 => 理论零超调。
                                      * 历史上试过又否掉（注释 :904-906），当时失败是因为刹车增强有 bug
                                      * （判据用 rate_ref => 收尾段恒不生效）；刹车修好后曲线才有意义。
                                      * 建议 150~400。只在 pivot（无循迹前馈）时生效。 */
#define HP_STRAIGHT_TRIM     0x0118u  /* ★2026-10-18 直行差速补偿（PWM 刻度，±1000）。
                                      * trim > 0 ⇒ 右轮多给、左轮少给，用来抵消"直行往右偏"。
                                      * 为什么必须有：固件台架表（speed_control.c:47-53）自证同一 PWM 下
                                      * 左右轮差 6~7%（左快），而 motion.c 直行时两轮给同一基准、参数表里
                                      * 没有任何 trim ⇒ 约 7°/s 的恒定偏航扰动没人补。
                                      * 实测：把 ang_kp/rate_kp 放大到 6/40 会直接失稳（原地 90° 超调 78%），
                                      * 所以只能从源头补偿，不能靠加增益硬顶。
                                      * 0 = 关（默认）；只在 |基准速度| ≥ 300 时生效，倒车自动反向。 */
#define HP_GYRO_SETTLE_DEG   0x0110u  /* 停车带半宽 °（|err| 内认为到位） */
#define HP_GYRO_SETTLE_MS    0x0111u  /* 待在停车带里多久算到位 ms */
#define HP_GYRO_CORR_REF     0x0112u  /* 差速天花板随基准速度放大的参考刻度（0 = 关闭，退回旧行为） */
/* ★2026-10-07 起步助推（静摩擦"起转"那一下）的两个旋钮 —— 加它们**只为一件事**：
 *   让"原地转起步一卡一卡"能像 `gyro_corr_limit` 一样**在线扫**（J-Link 只读 / 上位机
 *   `POST /api/params/set` + `push` 改 RAM），不必为了试一个值重烧一次。
 * ⚠ 这两项**不持久化**：不在 `app_ui.c` 的 `g_cfg` / NVRAM 里（与 `task_speed`、
 *   `trace_rate_max` 同类）⇒ 只吃 `host_link.c` 的 SPECS[] + 上位机 push，复位/烧录回默认。
 *   所以**不要**去动 `app_ui.c` 的 `push_pull_pid_params()` 那三处同步（那些只针对持久化的那些项）。
 * 默认值 = 进参数表之前的硬编码值（250 / 60000）⇒ 不 push 时行为与以前**逐位一致**。 */
#define HP_GYRO_BOOST_GAP_MS 0x0113u  /* 起转助推的冷却时间 ms：助推一撤，这么多毫秒内不许再点火。
                                       * 有它是因为"撤了又点"会把车来回射（见 gyro_control.c 的注释）。 */
#define HP_GYRO_BOOST_RATE   0x0114u  /* 起转助推的爬升斜率 PWM/s：60000 ⇒ 60ms 爬到 3600（起转阈值 ≈3450） */
#define HP_TRACE_ANGLE_PER_UNIT 0x0205u /* 串级外环：重心误差 -> 转率 (°/s)/格（用户 m32880 改口径） */
#define HP_TRACE_ANGLE_MAX   0x0206u  /* 串级外环：目标航向相对**当前**航向的前置量上限 °（防积分饱和） */
#define HP_TRACE_RATE_MAX    0x020Au  /* 串级：**循迹时**的期望角速度上限 °/s（pivot 仍用 HP_GYRO_RATE_MAX） */
#define HP_TRACE_CORNER      0x020Bu  /* ★直角主动转弯开关：0 = 关（默认，纯循迹）/ 1 = 开（自动转弯）。
                                       *   实现就在 PID_tracing.c 的直角那段（`trace_cal_channel` 半边 4 路检测
                                       *   + 一次转 100° + 原地对转 + 1.5s 防重触发）—— **一行没删**，只由这个
                                       *   参数挡住；在网页「设置/参数」页改成 1 再 push 即可打开。*/
#define HP_PIVOT_KP          0x0207u  /* 原地 pivot（base=0）循迹 PID：比例 */
#define HP_PIVOT_KI          0x0208u  /* 原地 pivot：积分（实测 0 最好，防 windup） */
#define HP_PIVOT_KD          0x0209u  /* 原地 pivot：微分 */

/* ---------------- 能力位（HELLO 报给上位机，与 tools/console/docs/API.md 一致） ---- */
#define HOST_CAP_TELEM2      (1u << 0)
#define HOST_CAP_ACTIONS     (1u << 1)
#define HOST_CAP_SERVO       (1u << 2)
#define HOST_CAP_DRIVE       (1u << 3)
#define HOST_CAP_CAL         (1u << 4)
#define HOST_CAP_FLASH       (1u << 5)
#define HOST_CAP_WATCHDOG    (1u << 6)
#define HOST_CAP_TASK        (1u << 7)   /* 上位机任务程序：T_TASK_*（0x60..0x69）+ Flash 三槽 */
#define HOST_CAP_HOLD        (1u << 8)   /* ★2026-10-18：T_DRIVE 支持 mode=5（HOLD，绝对目标航向） */
#define HOST_CAP_ALL         (HOST_CAP_TELEM2 | HOST_CAP_ACTIONS | HOST_CAP_SERVO | \
                              HOST_CAP_DRIVE | HOST_CAP_CAL | HOST_CAP_FLASH | HOST_CAP_WATCHDOG | \
                              HOST_CAP_TASK | HOST_CAP_HOLD)

#define HOST_PROTO_VERSION   2u      /* 协议版本：v2 加了扩展帧/动作表/驾驶/看门狗 */
#define HOST_FW_VERSION      0x0003u /* 固件版本：HELLO 按"高字节.低字节.0"显示 ⇒ 0.3.0。
                                      * v3 起 `TASK_OP_IF_STAGE` 的 `p0` 是**满足时间(ms)**
                                      * （0 = 不滤波）——**老固件不读它**，所以烧任务前先看这里。 */

/* ---------------- 生命周期 ---------------- */
void host_link_init(void);                 /* 初始化 LPUART1 + 参数表，发一条启动横幅 */
void host_link_poll(uint32_t loop_us);     /* 主循环里调用：收帧处理 + 按频率发遥测 */

/* ---------------- 给上层用 ---------------- */
float   host_param_get(uint16_t id, float fallback);   /* 控制代码从这里取参数 */
uint8_t host_param_u8(uint16_t id, uint8_t fallback);
uint32_t host_param_u32(uint16_t id, uint32_t fallback);
void    host_param_set_local(uint16_t id, float v);    /* 本地改参（不经串口） */
void    host_link_send_text(const char *s);            /* 调试文本转发到上位机 */
void    host_link_tick_1hz(void);                      /* 每秒报一次链路统计（RTT） */

/* 参数表自省（给上位机 /api/params 用；控制代码用不到） */
uint16_t host_param_count(void);
uint16_t host_param_id_at(uint16_t index);
int      host_param_bounds(uint16_t id, float *def, float *lo, float *hi);
uint16_t host_param_table_crc(void);                   /* 与 HELLO 里报的是同一个值 */

/* 驾驶（上位机远程控制车轮）状态，给遥测/UI 用 */
uint8_t host_drive_mode(void);                         /* 0 关 1 左右轮 2 基准+转向 3 循迹 */
uint8_t host_drive_is_enabled(void);

/* ---------------- 统计（RTT/调试用） ---------------- */
typedef struct {
  uint32_t rx_frames;
  uint32_t tx_frames;
  uint32_t crc_errors;
  uint32_t resyncs;
  uint32_t param_sets;
  uint16_t telem_hz;
} host_link_stats_t;

const host_link_stats_t *host_link_stats(void);

#endif /* __HOST_LINK_H__ */
