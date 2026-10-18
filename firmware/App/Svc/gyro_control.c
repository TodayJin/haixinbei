/**
 * @file  gyro_control.c
 * @brief 角度环：目标航向(JY61P 偏航角) -> 左右轮目标速度
 *
 * 三层串级里的中间层：
 *   上层（循迹环）给"目标航向"，本层算"左右轮目标速度"，
 *   下层（速度环 speed_control.c）再把速度变成 PWM。
 * 速度环没开（比如编码器还没装）时，退回旧板行为：直接 Load()。
 *
 * 符号约定：偏航角增大 = 向左转（与 app_ui 的 gyro_angles 一致）。
 *   correction > 0 -> 左轮减速、右轮加速 -> 左转。
 */
// gyro_control.c
#include "gyro_control.h"
#include "motor.h"
#include "jy61p.h"
#include "host_link.h"      /* 角度环参数统一放在 host_link 的参数表里（HP_GYRO_*） */
#include "speed_control.h"
#include "bsp_board.h"      /* MOTOR_PWM_MAX / MOTOR_PWM_MIN */
#include <math.h>

// PID控制器实现
PID_Controller gyro_pid;

extern TIM_HandleTypeDef htim4;

extern TIM_HandleTypeDef htim2;

// 控制状态标志
static uint8_t gyro_control_enabled = 0;

// PID参数不再硬编码：从 host_link 参数表实时取（菜单「PID -> 角度环」
// 或上位机都能改），fallback 取参数表默认值（host_link.c 的 SPECS[]）。
// 2026-10-01 原地台架实测：由旧刻度换算来的 720/72 在本板必自激，150/15 稳定。
#define GYRO_KP_FALLBACK 150.0f   /* 实测选定（旧 720 自激） */
#define GYRO_KI_FALLBACK 0.0f     /* 默认不用积分：积分会把车顶在停车带边上（实测 2.1~2.4°）不肯锁存 */
#define GYRO_KD_FALLBACK 15.0f    /* ≈Kp/10，压过冲 */
#define GYRO_DT          0.005f    /* TIM6 5ms 一拍 */

/* 角度环输出（= 左右轮差速速度目标，PWM 刻度）上限。
 * 2026-09-30/10-01 原地台架实测：不设限时大误差会命令一个接近满速的轮速
 * （corr = Kp×err，Kp=720 时 6° 就是 4320 → 前馈 5187），车冲过头后刹不住、
 * 原地来回甩好几圈。现在当成"安全网"：正常指令由 s_corr_* 三档自己定上限
 * （加速档 3600 是最大的），这里给 4000 兜底，别把加速档夹掉。
 * 台架可 SWD 改（不用重烧）：s_corr_limit。
 * ★2026-10-05 第 21 轮真车 A/B：默认 **2000 → 3000**（上面这句"给 4000 兜底"的原意才对得上）。
 *   见 `firmware/App/Svc/host_link.c` 里 `HP_GYRO_CORR_LIMIT` 那段的实测表。 */
volatile float s_corr_limit = 3000.0f;

/* 差速天花板随基准速度放大的参考刻度（2026-10-04，用户 m24215）。
 *
 * 为什么：修正量（= 左右轮差速）的上限 s_corr_limit 是**绝对刻度**，而基准速度能到
 * MOTOR_PWM_MAX(7200) ⇒ 满速时差速只占基准的 2000/7200 = 28%（基准 3000 时是 67%）。
 * 偏航角速度 ω ≈ Δv/轮距 只由 Δ 决定，所以缓慢速度与满速能达到的 ω 上限一样（≈63°/s），
 * 但转弯半径 R = v/ω 随速度线性变大（基准 3000 ≈ 0.2m → 7200 ≈ 0.47m）。
 * 用户的原话就是"速度拉满，按前进+左转就转不动了"——不是算错了差值，是天花板没跟着涨。
 *
 * 做法：上限按基准速度线性放大
 *     cap = s_corr_limit × max(1, |base| / s_corr_ref)      再夹到 ±MOTOR_PWM_MAX
 * 基准 ≤ s_corr_ref 时 max() 取 1 ⇒ 与旧行为**逐位一致**：原地转（base=0）不受影响，
 * 小误差贴上去时仍是 2000 的温柔刹车，台架调好的收尾/锁存特性一点没换。
 * s_corr_ref = 0 ⇒ 关闭放大，完全退回旧行为（台架 A/B 对照用）。
 * 默认 3000 = 台架那套标定参数的工作点。 */
volatile float s_corr_ref = 3000.0f;

/* ------------------------------------------------------------------
 * 双环滑模（2026-10-01 台架内窥实测后重写，替换掉"起转脉冲"那一版）
 *
 * 实测这台小车的摩擦是**强滞回**：从静止起步要 corr ≈ 3450
 * （速度环前馈 0.662×3450+2327 ≈ 4610 PWM）才动，而一旦滚动起来
 * corr ≈ 1000（PWM ≈ 3000）就能维持。差了三倍多。
 *
 * 线性 PID 在"死区 + 滞回"上不可能同时做到推得动和刹得住：
 *   · 误差小的时候 corr = Kp×err 只有几百（Kp=150 时 5° 才 750）⇒ 推不动车，
 *     只能干等积分慢慢爬过 3450（实测 1.5s 以上）——这就是"稳定时间很长"；
 *   · 而一旦顶到起转阈值，车带着动能窜出去又刹不住——这就是"每次都超一点点"。
 * 台架数据（2026-10-01，Δ=+20°）看得很清楚：修正量被固定值顶住 200~300ms，
 * 车转了 50° 才被反向拉回来，误差一路冲到 −9° 再慢慢爬回来。
 *
 * 中间试过"三档开关"（s_law=1）：外环 角度误差→期望角速度，内环按
 * e_rate = rate_ref − Gyro_Z 在 spin/hold/brake 三个固定值之间切。
 * 起步确实快（194ms 就动），但**台架 20° 阶跃实测出现 0.7s 周期的极限环**：
 * 速率在 +15 ~ −30°/s 之间摆、误差 ±2° 抖不停。原因就是这台车摩擦滞回太大：
 * 车一旦慢下来，内环立刻切回"加速档"，而轮子从静止重新起转要 200ms 以上，
 * 一个开关周期里车已经转过去好几度 ⇒ 只能靠抖。
 *
 * 现在（s_law=2，默认）改成**连续速率环**，不再分档：
 *     v_f      = 一阶低通后的角速度（τ≈12ms；陀螺的瞬时读数在堵转/打滑时抖得厉害，
 *                不滤一下内环会跟着尖峰乱打）
 *     rate_ref = clamp(s_ang_kp × err, ±s_rate_max)      ← 外环：误差 → 期望角速度
 *     ff       = 摩擦前馈（只在"命令方向和实际转向一致"时给，见下）
 *     corr     = clamp(ff + s_rate_kp × (rate_ref − v_f), ±s_corr_limit)
 * 比例项是一条**宽比例带**：速率差 30°/s 就顶到 3600（起转/反打都够猛），
 * 速率差 5°/s 只有几百（贴上去时很温柔）⇒ 既不抖、又不会慢吞吞。
 * 摩擦前馈解决"维持滚动"：滚起来只要 ≈1000 却要 3450 才起转，这部分由前馈补掉。
 *   !! 前馈只补"正在按命令方向滚动"那部分。反着滚时**绝不能给**：那时摩擦力本来
 *   就在帮着把车拉回来，给了反而把刹车抵消（实测反冲后退时 corr 只剩 +268，
 *   推不过 −17.6°/s 的倒退，车一路飘出 2.4°）。
 * 起转助推：车停着（v_f 很小）而误差还在死区外时，比例项往往只有几十~几百，
 *   根本推不动（起转要 ≈3450）。所以再叠一个**按斜率爬上来**的助推量（60ms 到 3600），
 *   车一动（v_f 超过阈值）立刻清零。早先那版是"停住就直接顶到 3600"，结果每次
 *   过零/回头都被它重新点着，20° 阶跃后半段变成了 0.5s 周期的来回摆 —— 所以
 *   梯度上升 + 一见动就撤，这两条缺一不可。
 * 到位判据：|err| < s_err_stop **且** |v_f| < s_ff_rate 才松手（还在转就先拽停，
 *   不然会在 7°/s 上滑过头）。
 * 全部量都能从台架 SWD 直改（不用重烧）：s_law=0 老 PID、1 三档开关、2 连续速率环、3（默认）同上+到位锁存。 */
volatile uint8_t s_law       = 3;      /* 3 = 连续速率环 + 到位锁存（默认，2026-10-02 定）；0 = 老 位置 PID + 陀螺阻尼（对照用）；1 三档开关；2 连续速率环 */
volatile float s_ang_kp      = 10.0f;  /* (°/s)/°：角度误差 → 期望角速度。
                                        * 2026-10-02 台架定 3.5（原 4.0）；2026-10-05 深夜**原地转圈**
                                        * 整定改 7（与 rate_kp 160 / rate_kd 0 配对）；**2026-10-06 定 10**：
                                        * ang_kp 是唯一能压缩到位时间的旋钮，三轮交错 A/B（ak5/fin1/fin2
                                        * 共 19 趟、两个转向各半）一致 ⇒ 10-90% 0.47s → 0.29s、
                                        * 到 90% 0.67s → 0.50s；窗 p-p 与换向不变（~3° / 0~1 次），
                                        * 峰值更收敛（需与 settle_deg 0.8 配对）。
                                        * ⚠ 与循迹共用（PID_tracing 每拍改目标航向）⇒ 赛道需复验。
                                        * 注意这里只是兜底初值 —— 每拍都会被参数表 HP_GYRO_ANG_KP 覆盖。 */
volatile float s_rate_max    = 120.0f; /* 期望角速度上限 °/s（2026-10-05 与参数表默认对齐；原地转圈扫 60~300 无差别） */
volatile float s_trace_rate_max = 300.0f; /* **循迹专用**的期望角速度上限 °/s（ff_rate != 0 时用它，pivot 仍用 s_rate_max）。
                                           * 为什么必须分开：corr = s_rate_kp × (rate_ref − v) ⇒ corr 上限 = rate_kp × rate_max。
                                           * pivot 的 40×120 = 4800，而 base=7000 时"把内侧轮刹到 0"就要 7000、要变负数还更多
                                           * ⇒ 直角这种大转弯永远跟不住。见 host_link.c 的 HP_TRACE_RATE_MAX 注释。 */
volatile float s_rate_band   = 10.0f;  /* s_law=1 内环滞环带宽 °/s（防抖） */
volatile float s_corr_spin   = 3600.0f;/* s_law=1 加速档：> 起转阈值(≈3450) */
volatile float s_corr_hold   = 1300.0f;/* s_law=2 摩擦前馈；s_law=1 维持档 */
volatile float s_corr_brake  = 2200.0f;/* s_law=1 反刹档 */
volatile float s_err_stop    = 2.0f;   /* |err| < 它 ⇒ corr = 0（松手） */
volatile float s_rate_kp     = 40.0f;  /* s_law=2/3 速率环比例：PWM刻度/(°/s)。
                                        * 2026-10-02 台架定 250（原 80）；2026-10-05 真车循迹降到 80
                                        * （速率环 P 是 3~4Hz 横向极限环的根因）；同夜**原地转圈**整定改
                                        * 160（30° 阶跃上升 0.281s）；**2026-10-06 又 160 → 40**：
                                        * 160 时小误差处 corr = 160×7×err 正好落在静摩擦起转阈值
                                        * （≈3450）附近 ⇒ 到位后 corr 反复打满 ±3000 来回踢
                                        * （"顶夹"占 27~88%、摆动 p-p 中位 6.2°）；40 之后顶夹 8~11%、
                                        * p-p 2.6~3.1°、换向 0~1，代价只是到位慢 0.17s。
                                        * 兜底初值，每拍由参数表覆盖。 */
volatile float s_ff_rate     = 3.0f;   /* s_law=2 摩擦前馈的方向判定阈值 °/s */
volatile float s_stuck_rate  = 2.0f;   /* s_law=2 "真的停住了"的角速度阈值 °/s */
volatile float s_boost_rate  = 60000.0f;/* s_law=2 起转助推的爬升斜率 PWM/s（60000 ⇒ 60ms 到 3600）。
                                        * ★2026-10-07 起**每拍由参数表 HP_GYRO_BOOST_RATE(0x0114) 覆盖**
                                        *（范围 10000..200000，见 host_link.c 的 SPECS 注释）⇒
                                        * 能在 J-Link/上位机上在线扫"助推爬多快"，默认值就是这里的
                                        * 60000.0f，不 push 参数时行为逐位不变。 */
volatile float s_rate_lpf    = 0.35f;  /* s_law=1/2 角速度反馈的一阶低通系数（每 5ms 一拍，0.35 ⇒ τ≈12ms） */
volatile float s_settle_deg  = 0.8f;   /* s_law=3 停车带宽（原版 s_err_stop 的 2°）+ 锁存判据。
                                        * 2026-10-06 由 2.0 → 1.0 → 0.8：起转助推在 |err| < 它时不武装
                                        * （见 :673），rate_kp=40 时最后 1~2° 没人推 ⇒ 系统性少转 1.4°。
                                        * 0.8 是 ak5 组唯一"五趟窗 p-p 全 ≤3.0°、yaw 换向 0 次"的候选，
                                        * 也把 ang_kp=10 的偶发冲过从 +7.7% 收到 +5.0%；0.5 会放大 corr 抖动。 */
volatile float s_settle_rate = 30.0f;  /* s_law=3 带内"真停住了"的角速度阈值 °/s（2026-10-02 加）：
                                        * ①带内且 |v| < 它 ⇒ corr = 0 直接松手滑行，不再用 ±limit
                                        *   的满权限刹车来回抽（原来只在 |v| < s_ff_rate(3°/s)
                                        *   才松手，而实测尾摆的角速度常年在 20~50°/s ⇒ 永远抽着）；
                                        * ②到位锁存的速率判据（原来写死 s_ff_rate*2 = 6°/s，
                                        *   实测同样永远不满足 ⇒ 锁存几乎从不触发，"到点停死"没实现）。
                                        * 台架扫值 15/20/25/30/40（各 2 次，Kp250/限幅2000）：
                                        * 30 的两次收尾误差 0.53/0.48°、超调 ≤0.44°；
                                        * 40 更快但 4 次里有一次冲过头 4.13°；15~25 收尾常停
                                        * 在 1.4~1.8°（松手早 ⇒ 靠摩擦停住，离目标还差一点）。 */
volatile float s_corr_slew   = 300000.0f;/* corr 斜率限幅 PWM/s（4000 用 13ms；再小会拖晚刹车） */
volatile float s_v_floor     = 0.0f;   /* s_law=3 逼近段最低允许角速度 °/s，0 = 关（默认）。
                                        * 试过 30~35：实测车身偏航的**有效**刹车能力在
                                        * 小角度下只有 ~150~300°/s²（轮子打滑，不是刻度问题），
                                        * 停 3° 内需要进场速度 < √(2·a·3) ≈ 30°/s，
                                        * 而 30°/s 本身就已经是 stick-slip 区 ⇒ 下限只会
                                        * 换来 ±2~3° 的来回摆（两次实测都是），故默认关闭。
                                        * 能收敛的还是"慢慢爬 + 静摩擦把车钉住"。 */
volatile float s_rate_kd    = 0.0f;    /* s_law=3 速率微分阻尼 PWM/(°/s²)，0 = 关。
                                        * 2026-10-02 台架扫 0/3/4/5 曾定 4（Kd=4 的 backDeg 最小）；
                                        * 2026-10-05 深夜**原地转圈**整定推翻它 ⇒ 改 **0**：
                                        * D 项是对陀螺做差分，是抖动的元凶 —— 逼近段 `corr` 反复打到
                                        * **反向满轨**（t=0.70 实测 yaw 速率只有 10~20°/s 而 rref=31，
                                        * P 项算不出 −3000，只有 Kd·dv/dt 要 ~750°/s² 才解释得通）；
                                        * 也解释了"降 rate_kp 反而更抖"（Kp 越小 D 占比越大）。
                                        * kd 4/0/10/20 中位数上升 0.797/0.687/1.367/到不了 90%，
                                        * 换向 5.1/2.4/7.1/7.5 次·s⁻¹ ⇒ kd=0 全面最好。 */
volatile float s_acc_lpf    = 0.35f;   /* s_law=3 角加速度反馈的低通系数 */
volatile float s_decel       = 600.0f; /* 仅 s_law=2（对照）的 √ 允许速度曲线用：实测车身偏航
                                        * 在 corr=4000 时加减速 ≈850~1070°/s²，取 600 留余量 */
volatile float s_open_corr   = 0.0f;   /* s_law=4 开环台架专用：直接把这个值当 corr 输出 */
volatile float s_settle_ms   = 150.0f; /* s_law=3 误差连续待在停车带里多久才认"到位"（ms）：
                                        * 只看速率（|v| < s_settle_rate）会漏判（几拍就穿过去了），
                                        * 所以要"带内 + 慢"持续够久；带外一点点放宽 6 倍见下。 */
/* 低通后的角速度（内环反馈用） */
static float   s_bias_z    = 0.0f;   /* 2026-10-18 陀螺零偏（°/s），GyroControl_CalibrateBias() 标定 */
static float s_rate_f    = 0.0f;
/* 起转助推的最小间隔（ms）：助推一撤就进冷却，冷却期内不许再点火。
 * 没有它，车在死区边缘一停，助推又满幅点着，把车再射出去 ——
 * 实测 60° 台阶尾部就是 ±2~4°、±50°/s 的来回穿梭（Boost 见动就撤也没用，
 * 因为"撤"和"再点"之间只隔一两拍）。
 * ⇒ **这 250ms 是防"撤了又点 ⇒ 来回射"的，不能盲目调小**：要试更小的值就在
 *   上位机上 push（`HP_GYRO_BOOST_GAP_MS`，范围 0..1000ms）扫着试，随时 push 回 250。
 * ★2026-10-07 起**每拍由参数表 HP_GYRO_BOOST_GAP_MS(0x0113) 覆盖**（默认 = 这里的
 *   250.0f ⇒ 不 push 时行为逐位不变）。用户报的"操控页原地转一卡一卡"就落在
 *   起步半秒：角速度 0→36°/s→掉回 0 停 ~0.3s→再冲到 75，正好是这个冷却在起作用
 *   （第一次助推没把车"带住"就得等冷却结束再点一次）—— 所以它必须能扫、也能回退。 */
volatile float s_boost_gap   = 250.0f;
volatile float s_brake_gain  = 2.0f;   /* ★刹车增强：只在"该减速"时放大 (rate_ref-v)，加速段不动 */
static float  s_boost_cool = 0.0f;     /* 冷却剩余 ms */
static float  s_boost_mag = 0.0f;
/* corr 的斜率限幅状态（上一拍的输出），以及"到位锁存"状态。
 * 锁存解决的就是用户听到/看到的"到点之后还一直小幅晃"：车其实停在 0.02° 里没动，
 * 但死区门 |rate_f| < s_ff_rate 在边界抖动，电机每隔几百 ms 被点一下
 * （实测尾段 CCR 仍有 0/2760/0/2708 这种脉冲）。一旦判定到位就彻底松手，
 * 直到 target_angle 变了才解锁（所以台架直接写 RAM 改目标也能解锁）。 */
static float   s_corr_f        = 0.0f;
static uint8_t s_settled       = 0;
static float   s_settled_target = 0.0f;
/* s_law=3 速率微分阻尼的状态：上一拍速率 + 低通后的角加速度 */
static float s_v_prev      = 0.0f;
static float s_a_f         = 0.0f;
/* s_law=3：误差连续待在停车带里的拍数（到位锁存判据） */
static uint16_t s_settle_cnt   = 0;
/* 遥测用只读快照（给上位机的 angle.rate_ref / angle.corr）：一拍更新一次，
 * 不参与任何控制判断，只是把本拍内部量暴露出去。 */
static float s_dbg_rate_ref = 0.0f;
static float s_dbg_corr     = 0.0f;
/* 循迹串级给进来的"期望偏航角速度"前馈（°/s）。消费式：GyroControl_Task() 每拍读走并清零，
 * 详见 GyroControl_SetTraceRate() 的注释。单独一个变量而不是塞进参数表：它是**每拍**变的
 * 控制中间量，不是配置。名字带 trace 以区别于 s_ff_rate（那是 s_law=2 的摩擦前馈阈值）。 */
static volatile float s_trace_ff = 0.0f;

/**
 * @brief 陀螺读数消毒：NaN / ±Inf / 离谱值一律当 0
 *
 * 为什么需要：JY61P 解析出来必然是 int16，正常不会出事；但 Gyro_Z 是 volatile
 * （台架用 SWD 直改），一旦写进 NaN，NaN 的所有比较都是 false ⇒ 后面每个
 * `if (x > limit)` 的限幅全部失效，NaN 会一路穿到 `(int)correction` —— 那是
 * C 标准里的未定义行为。用"取反写"的区间判据一次挡掉 NaN 和 ±Inf。
 */
static float gyro_sane(float v)
{
    return (v >= -1.0e6f && v <= 1.0e6f) ? v : 0.0f;
}

// 基础速度变量
static int base_speed_left = 0;
static int base_speed_right = 0;

// 添加目标角度变量，用于记录启动时的角度作为目标角度
static float target_angle = 0.0f;
static uint8_t angle_initialized = 0;

/* ★2026-10-07 连续域（不取模）目标与航向，专给 HOST_DRIVE_BASE 的"累计转角"用。
 * 为什么需要：`CalculateAngleError()` 按 ±180 取模走**最近路**，当上位机要的累计转角
 * 领先车头超过 180° 时它会指反 —— 车突然倒拐（真车实测翻号点 183/201/204/209°，
 * 见 out/_g_hold120.txt / _g_hold180.txt / _pw_before180.txt）。连续域里没有"最近路"，
 * 误差就是"还差多少度"，方向永远是对的。
 * 只在连续域模式（s_cont != 0）下参与控制；其余模式走原路径，行为逐位不变。 */
static float   target_cont = 0.0f;   /* 连续域目标角（不取模） */
static float   s_yaw_cont  = 0.0f;   /* 连续域航向：把 ±180 的 Yaw 逐拍解卷绕累加 */
static float   s_yaw_last  = 0.0f;   /* 上一拍的原始 Yaw（±180） */
static uint8_t s_cont      = 0U;     /* 1 = 用连续域误差 */

// 转向角度偏移量（可从参数表 HP_TURN_ANGLE_OFFSET 改）
#define TURN_ANGLE_OFFSET 90.0f

// 转到指定航向的状态
static uint8_t  turn_active = 0;
static uint32_t turn_start  = 0;

/**
 * @brief 角度规范化函数，将角度限制在 [-180, 180] 范围内
 *
 * 2026-10-02 加固：原来是无界的 `while (angle > 180) angle -= 360;`。
 * 这个函数在 TIM6 的 5ms 控制中断里被调用，如果 angle 是 ±Inf（SWD 写坏一个
 * volatile 参数、或上游哪一拍算出 Inf），两个 while 永远不成立不了 ⇒ **中断死循环**，
 * 车会保持上一拍输出一路冲出去。现在：非法值（NaN/±Inf/超范围）直接返回 0，
 * 合法值走 fmodf（常数时间，不再随角度大小线性变慢）。
 */
float NormalizeAngle(float angle)
{
    float r;

    if (!(angle >= -1.0e9f && angle <= 1.0e9f)) {
        return 0.0f;                     /* NaN 与 ±Inf 都走这里 */
    }

    r = fmodf(angle, 360.0f);            /* |r| < 360 */
    if (r > 180.0f)  r -= 360.0f;
    if (r < -180.0f) r += 360.0f;
    return r;
}

/**
 * @brief 计算角度差值，考虑角度环绕特性
 */
float CalculateAngleError(float target, float actual)
{
    float error = target - actual;
    error = NormalizeAngle(error);
    return error;
}

/**
 * @brief 初始化PID控制器
 */
void PID_Init(PID_Controller* pid, float kp, float ki, float kd)
{
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    pid->target = 0.0f;
    pid->actual = 0.0f;
    pid->error = 0.0f;
    pid->last_error = 0.0f;
    pid->integral = 0.0f;
    pid->output = 0.0f;
}

/**
 * @brief PID计算（角度环：误差按角度环绕算，积分限幅来自参数表）
 */
float PID_Compute_z(PID_Controller* pid, float target, float actual, float dt)
{
    pid->target = target;
    pid->actual = actual;

    // 使用角度差值计算，考虑角度环绕特性
    pid->error = CalculateAngleError(target, actual);

    // 积分
    pid->integral += pid->error * dt;

    // 积分限幅防止积分饱和（限幅值也来自参数表）
    {
        float ilim = host_param_get(HP_GYRO_ILIMIT, 1000.0f);
        if(pid->integral >  ilim) pid->integral =  ilim;
        if(pid->integral < -ilim) pid->integral = -ilim;
    }

    // 微分：用陀螺角速度，而不是 (err-last_err)/dt 差分。
    // JY61P 的 Yaw 大约 10ms 才更新一次，而本环是 5ms 一拍 ⇒ 差分出来的"微分"
    // 在相邻两拍之间交替出现 0 和两倍值（相当于把 Kd 打了对折、还带一堆尖峰）。
    // Gyro_Z 是模块直接给的偏航角速度(°/s)，符号与 d(Yaw)/dt 一致，拿它当微分源
    // 又稳又准：Kd 终于是一个"看得见效果"的参数（实测 Kd 15 就把过冲从 9.6° 压到 4.4°）。
    float derivative = -Gyro_Z;

    // PID计算
    pid->output = pid->kp * pid->error +
                  pid->ki * pid->integral +
                  pid->kd * derivative;

    pid->last_error = pid->error;

    return pid->output;
}

/**
 * @brief 初始化陀螺仪控制
 */
void GyroControl_Init(void)
{
    PID_Init(&gyro_pid,
             host_param_get(HP_GYRO_KP, GYRO_KP_FALLBACK),
             host_param_get(HP_GYRO_KI, GYRO_KI_FALLBACK),
             host_param_get(HP_GYRO_KD, GYRO_KD_FALLBACK));

    angle_initialized = 0;
    gyro_control_enabled = 0;
    turn_active = 0;
}

/**
 * @brief 启动陀螺仪控制：把"现在"的航向记成目标
 */
/* 2026-10-18 用户要求：在"模块测试 → IMU"里做零偏标定。
 * 车必须静止（校准期间不动）。采样 ms 毫秒的 Gyro_Z 求均值 => s_bias_z，
 * 之后所有角速度都用 (Gyro_Z - s_bias_z)，消掉温漂/残余偏置造成的"自己转"。
 * 返回标定到的偏置（°/s），0 表示失败（陀螺没数据）。 */
float GyroControl_CalibrateBias(uint32_t ms)
{
    if (ms < 200U)   ms = 200U;    /* 太短采不到几个点 */
    if (ms > 5000U)  ms = 5000U;   /* 太长会卡主循环：5s 封顶 */
    uint32_t t0 = HAL_GetTick();
    double   sum = 0.0;
    uint32_t n = 0U;
    while ((HAL_GetTick() - t0) < ms) {
        float g = Gyro_Z;
        if (g == g && g > -4000.0f && g < 4000.0f) {   /* NaN 与明显异常跳过 */
            sum += (double)g;
            n++;
        }
        HAL_Delay(1);
    }
    if (n < 20U) return GyroControl_BiasZ();   /* 采样太少：保持上次的值，别把好的零偏冲掉 */
    s_bias_z = (float)(sum / (double)n);
    if (s_bias_z > 5.0f)  s_bias_z =  5.0f;   /* 夹住，防止误标出一个巨大的偏置 */
    if (s_bias_z < -5.0f) s_bias_z = -5.0f;
    return s_bias_z;
}

float GyroControl_BiasZ(void)
{
    return s_bias_z;
}

void GyroControl_Start(void)
{
    target_angle = NormalizeAngle(Yaw);
    angle_initialized = 1;
    /* 连续域从"现在"重新起算：目标是当前航向、误差 0；默认退回取模路径 */
    s_yaw_cont  = Yaw;
    s_yaw_last  = Yaw;
    target_cont = Yaw;
    s_cont      = 0U;
    gyro_pid.integral = 0.0f;
    gyro_pid.last_error = 0.0f;
    s_rate_f    = gyro_sane(Gyro_Z);   /* 低通初值：别从 0 开始，否则头几拍会虚假地"慢慢转" */
    s_boost_mag = 0.0f;
    s_boost_cool = 0.0f;
    s_corr_f = 0.0f;
    s_settled = 0;
    s_settled_target = target_angle;
    s_settle_cnt = 0;
    s_v_prev = 0.0f;
    s_a_f = 0.0f;
    gyro_control_enabled = 1;
}

void GyroControl_Stop(void)
{
    gyro_control_enabled = 0;
    s_cont = 0U;                   /* 退出时一定要退回取模路径（任务/循迹不许吃到连续域） */
    turn_active = 0;
    s_boost_mag = 0.0f;
    s_boost_cool = 0.0f;
    s_corr_f = 0.0f;
    s_settled = 0;
    s_settle_cnt = 0;
    s_v_prev = 0.0f;
    s_a_f = 0.0f;
    s_trace_ff = 0.0f;             /* 循迹前馈也别留着（虽然 Task 每拍都消费，这里干净退出） */
    // 不再主动停止电机，让主控制逻辑接管
}

uint8_t GyroControl_IsEnabled(void)
{
    return gyro_control_enabled;
}

void GyroControl_SetTargetAngle(float deg)
{
    s_cont = 0U;                   /* 取模路径（任务/循迹/菜单都走这条，行为与以前一致） */
    target_angle = NormalizeAngle(deg);
    angle_initialized = 1;
    gyro_pid.integral = 0.0f;      /* 换了目标就清积分，别把上一个目标的存量带过来 */
    s_settled = 0;                 /* 换了目标就解锁到位锁存 */
}

/**
 * @brief 连续域目标（不取模）：给上位机操控页的"累计转角"用
 *
 * 与 `GyroControl_SetTargetAngle` 的唯一区别是 `s_cont = 1` ⇒ `GyroControl_Task()` 用
 * `target_cont − s_yaw_cont`（未取模）当误差，而不是 `CalculateAngleError()`（±180 最近路）。
 * 过 ±180° 时不再翻号（见头文件里那段说明）。非法值（NaN/Inf）直接忽略，保持上一拍目标。
 */
void GyroControl_SetTargetAngleCont(float deg)
{
    if (!(deg >= -1.0e6f && deg <= 1.0e6f)) {
        return;                    /* NaN / ±Inf：宁可不动目标，也不要把环写坏 */
    }
    target_cont  = deg;
    target_angle = NormalizeAngle(deg);    /* 遥测/调试仍看取模值 */
    angle_initialized = 1;
    gyro_pid.integral = 0.0f;
    s_settled = 0;
    s_cont = 1U;
}

float GyroControl_GetYawCont(void)
{
    return s_yaw_cont;
}

float GyroControl_GetTargetAngle(void)
{
    return target_angle;
}

float GyroControl_HeadingError(void)
{
    if (s_cont != 0U) {
        return target_cont - s_yaw_cont;   /* 连续域：未取模的"还差多少度" */
    }
    return CalculateAngleError(target_angle, NormalizeAngle(Yaw));
}

/**
 * @brief 遥测专用只读快照（给上位机的 angle 组）
 *
 * 都不是分析控制必需的量，只是把**本拍**内部用到的期望角速度与最终修正量
 * 暴露出来，让上位机能看见"环里现在在算什么"，调参时不用猜。
 */
float GyroControl_DebugRateRef(void)
{
    return s_dbg_rate_ref;
}

float GyroControl_DebugCorr(void)
{
    return s_dbg_corr;
}

/**
 * @brief 在当前目标上叠加一个角度（正 = 向左）
 */
void GyroControl_AimRelative(float delta_deg)
{
    target_angle = NormalizeAngle(target_angle + delta_deg);
}

/**
 * @brief 循迹串级专用：把外环算出的**期望偏航角速度**直接前馈进速率环（°/s，正 = 向左）
 *
 * 为什么必须有这条路（2026-10-05 加，用户 m33254「我是故意摆得斜的来检验你这个能力」逼出来的）：
 *   只靠"推目标航向 + 航向误差 P(s_ang_kp=3.5)"来跟线时，从"外环要的转率"到"车真的转"之间
 *   是一个 τ ≈ 1/s_ang_kp ≈ 285ms 的一阶滞后（rate_ref = s_ang_kp·err）。而外环自己的
 *   ω_n = √(v·per_unit) ≈ √(33 格/s × 1.047 rad/s/格) ≈ 5.9 rad/s（周期 ≈1.07s，与实测
 *   那个 1s 大摆完全吻合）⇒ **内环的滞后正好落在 ω_n 附近，外环几乎没有相位裕度**，表现为
 *   等幅大摆。此时加外环微分项也救不了：闭环特征方程 τs³ + s² + v·K_d·s + v·K_p = 0 的
 *   Routh 条件是 K_d > τ·K_p（=285ms×60 ≈ 17），过了门槛 ω_n 被推得更高、共轭对的阻尼比
 *   反而趋于 0.01（见 out\_poles.py）——实测 line_kd 0/13/25 三档 |e| 0.97/0.78/0.47 也就是
 *   "有一点但很不明显"。
 *   直接前馈之后，"期望转率→实际转率"变成低滞后通道，外环退化成干净的二阶：
 *   ë = −v(per_unit·e + line_kd·ė) ⇒ ζ = (line_kd/2)·√(v/per_unit)，line_kd=13 时 ζ≈0.7。
 *
 * 语义：**消费式** —— GyroControl_Task() 每拍取走这个值并清零，所以只会影响"设置它的那一拍"，
 * 别的模式（转弯任务/台架/上位机）永远不会吃到陈旧的前馈值。
 */
void GyroControl_SetTraceRate(float rate_deg_s)
{
    if (rate_deg_s == rate_deg_s && rate_deg_s >= -1000.0f && rate_deg_s <= 1000.0f) {
        s_trace_ff = rate_deg_s;
    } else {
        s_trace_ff = 0.0f;                 /* NaN/越界一律当没给 */
    }
}

float GyroControl_GetTraceRate(void)
{
    return s_trace_ff;
}

/**
 * @brief 转到指定航向；到位/超时用 GyroControl_TurnUpdate() 维护
 */
void GyroControl_TurnTo(float deg, uint32_t now_ms)
{
    GyroControl_SetTargetAngle(deg);
    gyro_pid.integral = 0.0f;
    gyro_pid.last_error = 0.0f;
    turn_active = 1U;
    turn_start  = now_ms;
}

uint8_t GyroControl_IsTurning(void)
{
    return turn_active;
}

/**
 * @brief 到位判定：|误差| < HP_TURN_TOLERANCE，或超过 HP_TURN_TIMEOUT
 */
void GyroControl_TurnUpdate(uint32_t now_ms)
{
    float tol, timeout;

    if (!turn_active) return;

    tol     = host_param_get(HP_TURN_TOLERANCE, 2.0f);
    timeout = host_param_get(HP_TURN_TIMEOUT, 1500.0f);

    if (fabsf(GyroControl_HeadingError()) < tol ||
        (float)(now_ms - turn_start) > timeout) {
        turn_active = 0U;
    }
}

uint8_t GyroControl_TurnDone(void)
{
    return (turn_active == 0U) ? 1U : 0U;
}

/**
 * @brief 设置基础速度
 */
void GyroControl_SetBaseSpeed(int left, int right)
{
    /* ★2026-10-18：**直行差速补偿**（HP_STRAIGHT_TRIM / 0x0118）放在这里，
     * 而不是某一两个调用点 —— 这样**所有会走直线的路径**都自动获得补偿：
     *   · MOTION_GYRO（角度环保持航向，网页按 W / 任务的"角度环+速度"）
     *   · MOTION_TRACE_GYRO（三级串级循迹）← 用户反馈"循迹不往中间走"就是因为这里原来没补
     *   · MOTION_TRACE（直驱差速循迹，虽然它不走 SetBaseSpeed，但由 PID_tracing 自己走速度环）
     *
     * 为什么必须有：台架表（speed_control.c:47-53）自证同一 PWM 下左右轮差 6~7%（左快）
     * ⇒ 恒定约 7~11°/s 的右偏扰动；角度环增益受 JY61P 更新率限制（实测放大到 6/40 就失稳），
     * 只能从**源头**做开环补偿。本车实测最优值 370（2.5 秒漂移从 −22° 降到 ±0.5°）。
     *
     * 规则：只对"基准速度足够大"的直行生效（|base| ≥ 300，否则原地转会被推歪）；
     *       倒车自动反向（偏航方向相反）。trim > 0 ⇒ 右轮多给、左轮少给（补右偏）。 */
    {
        int trim = (int)host_param_get(HP_STRAIGHT_TRIM, 0.0f);
        if (trim != 0) {
            int sb = (left + right) / 2;      /* 用两者平均值判断"是否在直行" */
            if (sb >= 300 || sb <= -300) {
                int t = (sb < 0) ? -trim : trim;
                left  -= t;
                right += t;
            }
        }
    }
    base_speed_left = left;
    base_speed_right = right;
}

/**
 * @brief 本拍生效的修正量（差速）上限：`s_corr_limit` 按基准速度放大（见 s_corr_ref 的注释）
 *
 * 单独做成函数是为了主机侧单元测试能直接断言这条曲线（firmware/test/t_gyrotest.c），
 * 不必去反推某一拍的控制输出。非负、夹到 MOTOR_PWM_MAX；参数被写坏成 NaN 时退回 0。
 */
float GyroControl_CorrLimit(void)
{
    float cap = gyro_sane(s_corr_limit);
    float ref = gyro_sane(s_corr_ref);
    int   b   = base_speed_left;
    int   b2  = base_speed_right;

    if (!(cap == cap)) cap = 0.0f;           /* NaN 兜底（gyro_sane 已经挡了一层） */
    /* 负限幅按**绝对值**用：旧的夹限幅写法是两个 if（`>lim` 和 `< -lim`），
     * lim 为负时它们的效果就是 |lim|。这里保持这个语义，行为才真的逐位不变。 */
    if (cap < 0.0f) cap = -cap;
    if (b  < 0) b  = -b;
    if (b2 < 0) b2 = -b2;
    if (b2 > b) b = b2;                      /* 两轮基准里取大的那个 */
    if (ref > 1.0f && (float)b > ref) cap += (float)b;   /* ★2026-10-16(E.2): 原 cap*=b/ref 让软上限恒等于 base ⇒ 内侧轮永远到不了 0 以下（与 :582-589 注释自相矛盾）。改成 corr_limit + base，内侧轮可反转到负；精细循迹时 corr 只有一两千、离夹子很远，不受影响。 */
    /* ★2026-10-06 夹子从 MOTOR_PWM_MAX 放宽到 MOTOR_PWM_MAX + |base|（用户 m 明确放行：
     *   「如果这个转弯要特别大，比如碰见90°的直角之类的，它的左轮或者右轮是可以变成负数的」）。
     *
     * 为什么需要：左轮目标 = base − corr（本文件 :886）。旧夹子把 corr 钉在 7200，
     * 于是 base=7000 时左轮最低只到 **−200 ≈ 停住**、**内侧轮永远无法反转** ⇒ 最大差速
     * 只有 7200（≈135 °/s）。而直角/大偏差要的是"先把车头拧过来"，那时**要的是差速本身**：
     * 只有两轮反向对转（差速 14400）才给得出 ≈240 °/s。放宽后 base=7000 时 corr 可到
     * 14200 ⇒ 左轮到底 −7200（全反转）。
     *
     * 为什么不会污染精细循迹：corr = rate_kp×(rate_ref − v)，小偏差时它只有一两千，
     * 离夹子很远 ⇒ 放宽夹子**只在"环自己要上万"的大转弯时才生效**。
     * （另外：横向拉回工况的最优点确实是 left=0 —— 因为 A·B = right²−left²，
     *   A+B=2·right 被单轮满量程夹住时 left=0 最优。两个工况由环的需求量自己区分，
     *   所以这里只放宽**上限**、不改变任何工作点。）
     *
     * 安全性不变：真正的轮量程边界是下面 left/right 各自夹到 ±MOTOR_PWM_MAX（:890-893），
     * 那两行才是"轮子不会超量程"的保证；这一行只是**差速**的上限。
     * 原地转（base=0）时 b=0 ⇒ 硬上限仍是 7200 ⇒ 与旧行为**逐位相同**，pivot 不受影响。 */
    {
        float cap_hard = (float)MOTOR_PWM_MAX + (float)b;
        if (cap > cap_hard) cap = cap_hard;
    }
    return cap;
}

/**
 * @brief 角度环任务（TIM6 5ms 一拍，由 Motion_Task 调用）
 */
void GyroControl_Task(void)
{
    float actual_angle, correction, damp;
    float corr_lim;                       /* 本拍生效的差速上限（见 GyroControl_CorrLimit） */
    int left_speed, right_speed;
    float ff_rate;                        /* 本拍来自循迹串级的转率前馈，消费式（见 SetTraceRate） */
    float err_now;                        /* 本拍误差：连续域模式 = 未取模的差；否则 = ±180 最近路 */

    if (!gyro_control_enabled) return;

    /* 角速度反馈先过一阶低通：陀螺的瞬时读数在堵转/打滑时抖得厉害，
     * 不滤一下内环会跟着尖峰乱打（台架上 24ms 一点能看到同一工况下
     * 读数在 +65 和 −24 之间跳）。τ≈12ms，相对 5ms 的控制拍只引入很小的相位滞后。 */
    {
        float a = gyro_sane(s_rate_lpf);  /* NaN/±Inf 的系数会让 s_rate_f 永久变 NaN */
        float g = gyro_sane(Gyro_Z - s_bias_z);   /* 2026-10-18：减掉标定过的零偏 */
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        s_rate_f += (g - s_rate_f) * a;
        if (!(s_rate_f >= -1.0e6f && s_rate_f <= 1.0e6f)) s_rate_f = 0.0f;  /* 自愈 */
    }

    // 每拍重新取一遍参数：菜单/上位机改完立刻生效
    gyro_pid.kp = host_param_get(HP_GYRO_KP, GYRO_KP_FALLBACK);
    gyro_pid.ki = host_param_get(HP_GYRO_KI, GYRO_KI_FALLBACK);
    gyro_pid.kd = host_param_get(HP_GYRO_KD, GYRO_KD_FALLBACK);

    /* 角度环自己的标定量也进参数表（上位机「调参 → 角度环」要能显示/改/固化）。
     * 这里把 volatile 变量同步成"参数表的镜像"：兜底值就是变量当前值，所以
     * 参数表里没有这些 id 时行为不变。台架要在线改这几个量，改参数表的槽位
     * （s_param[]），直接写这些变量会在下一拍被覆盖。 */
    s_ang_kp     = host_param_get(HP_GYRO_ANG_KP,     s_ang_kp);
    s_rate_kp    = host_param_get(HP_GYRO_RATE_KP,    s_rate_kp);
    s_rate_kd    = host_param_get(HP_GYRO_RATE_KD,    s_rate_kd);
    s_rate_max   = host_param_get(HP_GYRO_RATE_MAX,   s_rate_max);
    s_trace_rate_max = host_param_get(HP_TRACE_RATE_MAX, s_trace_rate_max);
    s_corr_limit = host_param_get(HP_GYRO_CORR_LIMIT, s_corr_limit);
    s_corr_ref   = host_param_get(HP_GYRO_CORR_REF,   s_corr_ref);
    s_corr_slew  = host_param_get(HP_GYRO_CORR_SLEW,  s_corr_slew);
    s_settle_deg = host_param_get(HP_GYRO_SETTLE_DEG, s_settle_deg);
    s_settle_ms  = host_param_get(HP_GYRO_SETTLE_MS,  s_settle_ms);
    /* ★2026-10-07 起步助推的两个旋钮也进参数表，好让它们能像 gyro_corr_limit 一样**在线扫**。
     * 取法和上面十项完全一样：兜底值 = 变量当前值 ⇒ 参数表里没这两个 id（老固件/台架）时
     * 一个字节都不变；参数表里有（正常固件就是）时取到的是 SPECS 默认值 250.0f / 60000.0f，
     * 与原硬编码值**逐位相同** ⇒ 不 push 参数时行为与以前逐位一致。
     * 扫描路径：J-Link 只读 / 桥 `POST /api/params/set` + `push`（改 RAM，复位回默认）。
     * 为什么放在这里（5ms 热路径每拍读）而不是"变化时同步"：一是不引入第二份状态
     * （`s_boost_mag`/`s_boost_cool` 已经够多了，再加"上一次的值"就多一种失配方式），
     * 二是 `host_param_get()` 只是一张全表线性扫描 + 有限性判据，本函数**已经在**每拍读
     * 上面那十项了 ⇒ 多两个 id 是同一量级的常数开销，不改变热路径的量级；
     * 三是"改完立刻生效"这条台架习惯（:621 的注释）对所有参数都成立、不会漏。 */
    s_boost_gap  = host_param_get(HP_GYRO_BOOST_GAP_MS, s_boost_gap);
    s_boost_rate = host_param_get(HP_GYRO_BOOST_RATE,   s_boost_rate);
    s_brake_gain = host_param_get(HP_GYRO_BRAKE_GAIN,  s_brake_gain);
    if (!(s_brake_gain >= 1.0f && s_brake_gain <= 10.0f)) s_brake_gain = 1.0f;  /* 坏值=关闭 */
    s_v_floor    = host_param_get(HP_GYRO_V_FLOOR,     s_v_floor);
    if (!(s_v_floor >= 0.0f && s_v_floor <= 120.0f)) s_v_floor = 0.0f;  /* 坏值=关 */
    s_settle_rate = host_param_get(HP_GYRO_SETTLE_RATE, s_settle_rate);
    if (!(s_settle_rate >= 0.0f && s_settle_rate <= 300.0f)) s_settle_rate = 30.0f;  /* 坏值=默认 */

    /* 差速上限：按上一步回读到的 s_corr_limit / s_corr_ref 与基准速度算一次，本拍各律共用。
     * 放在参数回读之后，保证"上位机改完参数同一拍就按新值算"。 */
    corr_lim = GyroControl_CorrLimit();

    s_dbg_rate_ref = 0.0f;

    /* 取走循迹串级这一拍给的转率前馈（读走即清 ⇒ 别的模式永远看不到陈旧值）。
     * 有它时"到位锁存"和停车带松手都要让路：车是被线推着连续走的，不是"转到位就停"。 */
    ff_rate = s_trace_ff;
    s_trace_ff = 0.0f;
    if (!(ff_rate >= -1000.0f && ff_rate <= 1000.0f)) ff_rate = 0.0f;

    actual_angle = NormalizeAngle(Yaw);

    /* ★2026-10-07 连续域航向（逐拍把 ±180 的 Yaw 解卷绕）：
     * 5ms 一拍车身最多转几度，所以"按最近路取小量再累加"不会误判整圈。
     * 只有 s_cont（连续域目标）模式用它，其余模式只维护不参与控制。 */
    {
        float dyaw = NormalizeAngle(Yaw - s_yaw_last);
        s_yaw_last  = Yaw;
        s_yaw_cont += dyaw;
        if (!(s_yaw_cont >= -1.0e7f && s_yaw_cont <= 1.0e7f)) {
            s_yaw_cont = Yaw;              /* 自愈：被写坏就从当前航向重来 */
        }
    }

    /* 本拍误差：连续域模式**不取模**（方向永远对），其余模式与以前逐位相同 */
    err_now = (s_cont != 0U) ? (target_cont - s_yaw_cont)
                             : CalculateAngleError(target_angle, actual_angle);

    /* 换目标就解锁"到位锁存"，并清掉各律的微分/助推状态。
     * 放在各律之前：同一拍里所有律看到同一份状态；台架直接写 RAM 改 target_angle 也算换目标。 */
    if (target_angle != s_settled_target) {
        s_settled        = 0;
        s_settle_cnt     = 0;
        s_settled_target = target_angle;
        s_a_f            = 0.0f;
        s_v_prev         = s_rate_f;
    }

    if (s_law == 0) {
        /* 老路（对照用）：PID + 陀螺微分 + 阻尼 + 限幅。
         * 目标角速度由 PID 连续给，静摩擦死区里推不动车，实测整定慢、还容易冲过头。 */
        correction = PID_Compute_z(&gyro_pid, target_angle, actual_angle, GYRO_DT);
        damp = host_param_get(HP_YAW_RATE_DAMP, 1.0f);
        correction -= damp * gyro_sane(Gyro_Z);
        if (correction >  corr_lim) correction =  corr_lim;
        if (correction < -corr_lim) correction = -corr_lim;
    } else if (s_law == 1) {
        /* 三档开关（对照用，实测有 0.7s 极限环）：外环 角度误差 → 期望角速度，
         * 内环 期望角速度 → spin/hold/brake 三档定值。 */
        float err = err_now;
        float rate_ref, e_rate;

        gyro_pid.target   = target_angle;
        gyro_pid.actual   = actual_angle;
        gyro_pid.error    = err;
        gyro_pid.integral = 0.0f;      /* 双环不用积分；切回 PID 时也是干净的 */

        if (fabsf(err) < s_err_stop) {
            correction = 0.0f;         /* 到位了：松手滑行 */
        } else {
            rate_ref = s_ang_kp * err;
            if (rate_ref >  s_rate_max) rate_ref =  s_rate_max;
            if (rate_ref < -s_rate_max) rate_ref = -s_rate_max;

            e_rate = rate_ref - s_rate_f;
            if (e_rate > s_rate_band) {
                correction =  s_corr_spin;
            } else if (e_rate < -s_rate_band) {
                correction = -s_corr_brake;
            } else {
                correction = (rate_ref >= 0.0f) ? s_corr_hold : -s_corr_hold;
            }
        }

        if (correction >  corr_lim) correction =  corr_lim;
        if (correction < -corr_lim) correction = -corr_lim;
        gyro_pid.output = correction;
        gyro_pid.last_error = err;
    } else if (s_law == 2) {
        /* s_law=2 连续速率环（对照用）：宽比例带的速率比例项 + 起转助推。
         * 实测这个律在 60° 台阶上会进 ±10°、±110°/s 的极限环：
         * 刹车阶段只有 Kp=80 的比例权限（跟 800°/s² 的曲线需要 5300+ 的 corr），
         * 于是提前松刹 → 以 110°/s 冲过目标 → 曲线符号翻转后又全速加速回来。
         * 保留它做对照，默认律见 s_law=3。 */
        float err = err_now;
        float rate_ref;

        gyro_pid.target   = target_angle;
        gyro_pid.actual   = actual_angle;
        gyro_pid.error    = err;
        gyro_pid.integral = 0.0f;

        /* 目标一变就解锁（台架直接写 RAM 改 target_angle 也走这条路） */
        if (target_angle != s_settled_target) s_settled = 0;

        if (s_settled) {
            correction = 0.0f;         /* 已锁存：彻底松手，直到换目标 */
        } else if (fabsf(err) < s_settle_deg) {
            /* 停车带：进这个带就只刹车、不再给方向。
             * 这里是"到点直接停死"的关键 —— 以前是 |err|<s_err_stop(2°)
             * 才松手，等于让车在最后 2° 里慢慢爬，而轮子慢滚必然 stick-slip。 */
            correction = s_rate_kp * (0.0f - s_rate_f);
        } else {
            /* √ 刹车曲线：v² = 2·a·s 反推该以多快驶向目标。
             * 旧版 rate_ref = s_ang_kp·err 在 err≈25° 就开始收速（100°/s ÷ 4），
             * 末段 20° 全在"低于起转阈值的慢速区"里爬 ⇒ 用户反馈的"一卡一卡"。
             * 现在离得远就顶着 s_rate_max 冲，近了才按 √(2·a·|err|) 收，
             * 最后一段是"高速进来、一次性刹住、停死"。 */
            float brk = sqrtf(2.0f * s_decel * fabsf(err));
            if (brk > s_rate_max) brk = s_rate_max;
            rate_ref = (err > 0.0f) ? brk : -brk;

            /* 这里**不加**摩擦前馈（s_corr_hold 只给 s_law=1 用）。
             * 试过"按运动方向补 1300 维持滚动"，结果是个 0.35s 周期的极限环：
             * 前馈把轮子一直推着滚，比例项只剩几十，压不到命令速率上
             * （实测命令 −9.7°/s、实际 −10.4°/s 时 corr = −1074，越推越快），
             * 于是 corr 在 ±4000 之间来回砸、车 ±4° 打摆。
             * 速度环自己那套前馈（0.662×目标+2327）已经在补滚动摩擦了，
             * 角度环这里只需要"速率 P + 起转助推"。 */
            correction = s_rate_kp * (rate_ref - s_rate_f);
        }

        /* 起转助推（静摩擦）：车停着（|v_f| < s_stuck_rate）而误差还在停车带外时，
         * 比例项往往只有几十~几百，根本推不动（起转要 ≈3450）。所以叠加一个
         * **按斜率爬上来**的助推量（s_boost_rate=60000 ⇒ 60ms 爬到 3600，
         *  ★2026-10-07 起这两个量每拍从参数表读：`HP_GYRO_BOOST_RATE` / `HP_GYRO_BOOST_GAP_MS`），
         * 一旦车动起来立刻清零。
         * 为什么不能像早先那版"停住就一步顶到 3600"：那个每次过零/回头都被重新
         * 点着，20° 阶跃后半段变成 0.5s 周期的来回摆（实测 ±2~4° 抖不停）。
         * 梯度上升 + 见动就撤，两条缺一不可。 */
        if (s_boost_cool > 0.0f) s_boost_cool -= GYRO_DT * 1000.0f;
        if (!s_settled && fabsf(err) > s_settle_deg &&
            fabsf(s_rate_f) < s_stuck_rate && s_boost_cool <= 0.0f) {
            s_boost_mag += s_boost_rate * GYRO_DT;
            if (s_boost_mag > s_corr_spin) s_boost_mag = s_corr_spin;
        } else {
            if (s_boost_mag > 0.0f) s_boost_cool = s_boost_gap;   /* 刚撤掉 ⇒ 进冷却 */
            s_boost_mag = 0.0f;
        }
        if (s_boost_mag > 0.0f) {
            float boost = (err > 0.0f) ? s_boost_mag : -s_boost_mag;
            if ((err > 0.0f) ? (correction < boost) : (correction > boost)) correction = boost;
        }

        /* corr 斜率限幅：把 ±4000 的 bang-bang 变成斜坡。
         * 实测 60° 台阶末段 corr 在 ±3600 之间砸（一拍内反向 2000+），
         * 电机听到的是一串脉冲；限幅后同样的修正量走 50ms 斜坡。 */
        {
            float slew = s_corr_slew * GYRO_DT;
            if (correction - s_corr_f >  slew) correction = s_corr_f + slew;
            if (correction - s_corr_f < -slew) correction = s_corr_f - slew;
        }
        if (correction >  corr_lim) correction =  corr_lim;
        if (correction < -corr_lim) correction = -corr_lim;
        s_corr_f = correction;

        /* 到位锁存：进停车带且基本不转了 ⇒ 永久松手（换目标才解锁） */
        if (!s_settled && fabsf(err) < s_settle_deg && fabsf(s_rate_f) < s_ff_rate) {
            s_settled = 1;
            s_settled_target = target_angle;
        }

        gyro_pid.output = correction;
        gyro_pid.last_error = err;
    } else if (s_law == 3) {
        /* ===== s_law=3（默认）：带下限的线性允许速度曲线 + 比例速率环 + 停车带 + 到位锁存 =====
         * 结构就是老 s_law=2（唯一实测好的那版：60° 超调 0~2°、尾摆 ≤0.6°），
         * 只改两处 —— 都是冲用户 m05052 那两句话去的：
         *  ① "中间一卡一卡"：老曲线的 v_ref = s_ang_kp·|err| 误差小了就趋于 0，
         *     车被迫在"低于起转阈值"的慢速区爬完最后十几度 —— 那正是轮子的
         *     stick-slip 区（密集实测：角速度 3.8~104°/s 之间来回跳、corr 在
         *     ±3600 之间砸、一拍突变>30°/s 出现 15 次）。给 v_ref 加下限
         *     s_v_floor（35°/s）后全程不进慢滚区，最后 s_settle_deg（1.5°）由
         *     停车带**一次性刹住**：35°/s 按实测 a≈600~1000°/s² 只需 0.6~1.0°。
         *  ② "到点一直小幅度晃"：到位后**锁存**（s_settled），corr 恒 0、电机彻底
         *     松手，直到目标变化。老版没锁存，死区门 |v| < s_ff_rate 在边界上抖
         *     一下就会重新点火（实测尾段 4249ms 里 CCR 还有 0/2760/0/2708 的脉冲）。
         * 试过并被数据否掉的替代：√(2a·|err|) 刹车曲线（纯比例跟不住它的减速度 ⇒
         * 提前松刹、110°/s 冲过头 11°）；"超速就满权限反刹"的继电器（阈值附近没
         * 死区，巡航段 corr 每拍 ±4000 翻、角速度 34↔128°/s，比之前更糟）；曲线
         * 减速度前馈 s_kacc·Δv_ref/dt（v_ref 被 ±s_rate_max 夹住 ⇒ 误差过零时
         * a_cmd=±44000，前馈自己变成继电器，出 ±23°/174°/s 的剧烈极限环）。 */
        float err = err_now;
        /* ★2026-10-15（用户："控制它转，它都不转，左右轮交替动原地蹭"）——**角速度反馈符号修正**。
         *  证据（读代码）：
         *    · 老律 s_law=0 用的是 `float derivative = -Gyro_Z;`（本文件 :334）⇒ **取负**；
         *    · 而 s_law=2/3 的反馈是 s_rate_f，它由 `s_rate_f += (Gyro_Z - s_rate_f)*a`（:623-628）
         *      直接拿 Gyro_Z，**没有取负** ⇒ 两律对同一物理量的符号约定相反 ✗
         *  后果：内环 `correction = rate_kp*(rate_ref - v)` 一开车转起来 v 就给出反号
         *        ⇒ correction 立刻反号 ⇒ 两轮交替猛推 ⇒ 原地蹭、yaw 不前进（实测就是如此）。
         *  修法：s_law=3 这一路的反馈取负，与 s_law=0 的 `-Gyro_Z` 约定统一。
         *  ⚠ 只改本分支的局部变量，不动 s_rate_f 本身（settle 判定 :1001 与循迹 :710 另有用处）。 */
        float v   = -s_rate_f;

        gyro_pid.target   = target_angle;
        gyro_pid.actual   = actual_angle;
        gyro_pid.error    = err;
        gyro_pid.integral = 0.0f;

        /* 锁存后车又被推动（地面、撞线、残余速度）漂出停车带 ⇒ 解锁重新纠。
         * 原来 s_settled 的释放只有换目标一条路（:262/:277/:294/:393/:456），
         * 一旦锁在偏了的位置上就再也不管了。用 1.5× 带宽做回差，避免锁存/解锁抖。 */
        if (s_settled && fabsf(err) > s_settle_deg * 1.5f) {
            s_settled    = 0;
            s_settle_cnt = 0;
            /* 锁存期间 s_v_prev/s_a_f 不更新，解锁那一拍 (v - s_v_prev)/0.005 会是
             * 一个几十拍前的旧值差 ⇒ 阻尼项瞬间打满。解锁时把它们对到当前值。 */
            s_v_prev     = v;
            s_a_f        = 0.0f;
        }

        if (s_settled) {
            correction = 0.0f;                     /* 已锁存：彻底松手，直到换目标 */
        } else {
            /* 允许速度：s_ang_kp·err（= 唯一实测能收敛的形状），顶到 ±s_rate_max；
             * 停车带里 rate_ref 直接给 0（只刹车、不再给方向）——**有前馈时不能这么做**：
             * 循迹时车要一直按外环的转率转，停车带清掉 rate_ref 就等于把前馈吃了。 */
            /* ★2026-10-06 P 项符号保护（**只动 ff≠0 的循迹路；ff==0 的 pivot 逐位不变**）：
             *   当 P 项与前馈**反号**且 |P|>|ff| 时，rate_ref 会取 err 的符号 = **前馈的反号**
             *   ⇒ 外环要左转、环却往右推，而且**持续到 err 收敛**（不是自限的过冲反打）。
             *   残留的两条路径正好踩它：① 重新压线那一拍（保持段攒回的 lead 与新的 ff 反号）；
             *   ② 扫描掉头那刻（err 钉在 +35，掉头瞬间新 ff 反向）。
             *   反号门槛只有 |ff|/s_ang_kp = 12°，而 lead 常态就是 12~35° ⇒ 很容易踩上。
             *   ⇒ 这种拍把 P 项掐掉、只留前馈：**方向始终听外环的**（多 agent 对抗性审查结论）。 */
            /* ★2026-10-06 P 项符号保护**加门限**：只在 `|ff| > 60°/s` 时才掐 P 项。
             *   为什么加门限：用户 m 报"正常行驶小弯的过程中，它的回中速度很慢，导致我转弯不及时，
             *   直接跑出去了" —— 而**回正/回中靠的正是那个与前馈反号的 P 项**，一刀切掉就等于
             *   缓弯/直线上没有回正力矩。缓弯 ff≈30°/s、直角/找线 ff=120~170°/s ⇒ 加 60 门限两边都保住。
             *   （原注：P 与前馈反号且 |P|>|ff| 时 rate_ref 会取 err 符号 = 前馈反号，能持续反向。） */
            float p_term = s_ang_kp * err;
            if (ff_rate != 0.0f && fabsf(ff_rate) > 60.0f &&
                ((p_term > 0.0f) != (ff_rate > 0.0f)) &&
                fabsf(p_term) > fabsf(ff_rate)) {
                p_term = 0.0f;
            }
            float rate_ref = p_term + ff_rate;
            /* ★2026-10-19 刹车曲线（梯形速度规划，默认关）：详见 host_link.h 的
             * HP_GYRO_APPROACH_ACC 说明。只在 pivot（无循迹前馈）时用，循迹路径逐位不变。 */
            {
                float acc = host_param_get(HP_GYRO_APPROACH_ACC, 0.0f);
                if (acc > 1.0f && ff_rate == 0.0f && fabsf(err) > s_settle_deg) {
                    float v_curve = sqrtf(2.0f * acc * fabsf(err));
                    float rmax_c  = s_rate_max;
                    if (v_curve > rmax_c) v_curve = rmax_c;
                    rate_ref = (err > 0.0f) ? v_curve : -v_curve;
                }
            }
            /* 期望角速度上限**分两种工况**：pivot（无前馈）用 s_rate_max，循迹（有前馈，
             * 来自 PID_tracing 的串级）用 s_trace_rate_max。为什么必须分开：
             * corr = s_rate_kp×(rate_ref − v) ⇒ **corr 的上限 = rate_kp × rate_max**。
             * pivot 那套 40×120 = 4800 在 base=7000 时连"把内侧轮刹到 0"（需要 7000）都不够，
             * 更别说用户要的"直角时内侧轮可以变负"。分开之后 ff_rate == 0 这条路**逐位不变**。 */
            float rmax = (ff_rate == 0.0f) ? s_rate_max : s_trace_rate_max;
            if (fabsf(err) < s_settle_deg && ff_rate == 0.0f) {
                rate_ref = 0.0f;
            } else {
                if (rate_ref >  rmax) rate_ref =  rmax;
                if (rate_ref < -rmax) rate_ref = -rmax;
                if (s_v_floor > 0.0f && fabsf(rate_ref) < s_v_floor) {
                    rate_ref = (rate_ref >= 0.0f) ? s_v_floor : -s_v_floor;  /* 可选下限，默认关 */
                }
            }
            s_dbg_rate_ref = rate_ref;      /* 只给遥测看 */
            /* ★2026-10-15 刹车增强（用户："还是会超调"）：
             * 实测根因：车朝目标跑到 v≈180°/s，随着 err 减小 rate_ref 也降（err=45 ⇒ rate_ref=90），
             * 而 v 因惯性还有 ~120 ⇒ corr = rate_kp×(90−120) = **仅 300**，而刹住需要 ≈3000 ⇒ 冲过 20~30°。
             * 修法：只在"e 与 rate_ref 反号"（= 车正朝目标方向但已超速 ⇒ 这是刹车不是转向）时把 e 放大，
             * 加速段/稳态/锁存逻辑一字不改。s_brake_gain = 1.0 时与旧行为逐位一致。 */
            {
                float e_brake = rate_ref - v;
                /* ★2026-10-19（用户："要稳、不要超调"，实测收尾滑过头）：**判据写错了**。
                 * 原来用 _brake * rate_ref < 0 判断"是不是在刹车"，可是**最后一次进停车带时
                 * rate_ref 被上面 :969-970 置成 0** ⇒ 乘积恒为 0 ⇒ 条件永不成立
                 * ⇒ **刹车增强在最关键的那一段完全不生效** ✗ ⇒ 车带着惯性滑过去 ⇒ 超调。
                 * 这也是"同一组参数有时 0% 有时 20~40%"的原因：滑行距离由摩擦力（电压/地面）决定。
                 * 正确判据：修正量是否**反着当前运动方向**出力 —— 用实际角速度 v，与 rate_ref 无关。
                 *   e_brake * v < 0  ⇔  correction 与 v 反号 ⇔ 这一拍是在"刹"，不是在"推"。 */
                if (e_brake * v < 0.0f) e_brake *= s_brake_gain;
                correction = s_rate_kp * e_brake;
            }
            /* 速率微分阻尼（PD）：对"角速度正在变化"这件事加阻尼。
             * 用它是因为这台车的执行器是继电器性质的（低于起转阈值完全不动，
             * 一过阈值就窜），纯比例在"命令速率 + 实际速率"差得多时只能给 ±4000，
             * 于是命令本身一拍翻一次、轮子打滑 ⇒ 用户看到的一卡一卡。
             * dv/dt 项在加速时抵消一部分比例项 ⇒ 命令更早收敛、翻得没那么狠。
             * 默认 0（关）：先做台架扫值，证明有收益再写进默认。 */
            if (s_rate_kd > 0.0f) {
                float a_raw = (v - s_v_prev) / GYRO_DT;      /* °/s² */
                float b = gyro_sane(s_acc_lpf);
                if (b < 0.0f) b = 0.0f;
                if (b > 1.0f) b = 1.0f;
                s_a_f += (a_raw - s_a_f) * b;                /* 一阶低通，别被陀螺尖峰带跑 */
                if (!(s_a_f >= -1.0e6f && s_a_f <= 1.0e6f)) s_a_f = 0.0f;  /* 自愈 */
                correction -= s_rate_kd * s_a_f;
            }
            s_v_prev = v;
            if (fabsf(err) < s_settle_deg && fabsf(v) < s_settle_rate && ff_rate == 0.0f) {
                correction = 0.0f;   /* 带内 + 已经转得很慢 ⇒ 松手滑行（见 s_settle_rate 注释）。
                                      * 试过"朝目标滑行就不刹"（err*v > 0 也松手，指望摩擦把最后
                                      * 1~2° 补上）：收尾误差没变小，反而多出 4.7° 的冲过头 ⇒ 否。 */
            }
        }

        /* 起转助推：停车带外、车真的停住了、而比例量又推不动（起转要 ≈3450）时
         * 按斜率补一脚；一动就撤、撤了进冷却（见 s_boost_gap 注释）。
         * ★2026-10-07 起 `s_boost_rate` / `s_boost_gap` 每拍由参数表
         * `HP_GYRO_BOOST_RATE(0x0114)` / `HP_GYRO_BOOST_GAP_MS(0x0113)` 覆盖（在 Task 开头，
         * 与上面十项同一处）⇒ 上位机 push 完**同一拍**就按新值跑，默认值 = 原硬编码值。
         * 有循迹前馈时不补：前馈本身就能给出远超起转阈值的量（s_rate_kp×ff），
         * 而且外环的转率指令可能和"朝目标角"的方向相反，补这一脚就是打架。 */
        if (s_boost_cool > 0.0f) s_boost_cool -= GYRO_DT * 1000.0f;
        if (!s_settled && ff_rate == 0.0f && fabsf(err) > s_settle_deg &&
            fabsf(v) < s_stuck_rate && s_boost_cool <= 0.0f) {
            s_boost_mag += s_boost_rate * GYRO_DT;
            if (s_boost_mag > s_corr_spin) s_boost_mag = s_corr_spin;
        } else {
            if (s_boost_mag > 0.0f) s_boost_cool = s_boost_gap;
            s_boost_mag = 0.0f;
        }
        if (s_boost_mag > 0.0f) {
            float boost = (err > 0.0f) ? s_boost_mag : -s_boost_mag;
            if ((err > 0.0f) ? (correction < boost) : (correction > boost)) correction = boost;
        }

        /* corr 斜率限幅：斜坡化，免得电机听到一串方波。
         * 注意别把它当"平滑"用过头：+4000→−4000 用 80000 时要 100ms，
         * 比"77°/s 下 100ms 误差 = 7.7°"还大 ⇒ 刹车会晚。默认 300000（27ms）。 */
        {
            float slew = s_corr_slew * GYRO_DT;
            if (correction - s_corr_f >  slew) correction = s_corr_f + slew;
            if (correction - s_corr_f < -slew) correction = s_corr_f - slew;
        }
        if (correction >  corr_lim) correction =  corr_lim;
        if (correction < -corr_lim) correction = -corr_lim;
        s_corr_f = correction;

        gyro_pid.output = correction;
        gyro_pid.last_error = err;
    } else {
        /* s_law=4：**开环台架专用** —— 直接把 s_open_corr 当 corr 输出（只过斜率限幅）。
         * 用来测被控对象的真实能力：给一个恒定 corr，看车身偏航角加速度/减速度有多少、
         * 有没有延迟和轮子打滑。不参与正常控制。 */
        correction = s_open_corr;
        {
            float slew = s_corr_slew * GYRO_DT;
            if (correction - s_corr_f >  slew) correction = s_corr_f + slew;
            if (correction - s_corr_f < -slew) correction = s_corr_f - slew;
        }
        if (correction >  corr_lim) correction =  corr_lim;
        if (correction < -corr_lim) correction = -corr_lim;
        s_corr_f = correction;
        gyro_pid.output = correction;
    }

    /* 到位锁存（各律共用；s_law=4 开环台架除外）：误差连续待在停车带里够久、
     * 且角速度确实小 ⇒ 认到位，之后彻底松手（corr=0）直到换目标。
     * 这是"到点直接停死、不再小幅晃"的关键：没有它，死区门会在边界上抖动，
     * 电机被反复点着（实测尾部 CCR 仍出现几百甚至 2760 的脉冲，车不转但听得见）。
     * 两级判据：带内(s_settle_deg)只要 s_settle_ms；**带外一点点(s_settle_deg×2)放宽 6 倍**，
     * 否则车正好停在 2.0~2.4°（差一点没进带）时会一直抽着找那零点几度（实测 Ki≠0 就是这样）。
     *
     * 循迹前馈期间必须让路：车是被线连续推着走的，角误差/角速度在转弯中途或过中线那一瞬
     * 很容易短暂落进停车带，一锁存就把前馈连同 correction 一起吃掉（车会"跟丢了不动"）。
     * 所以有前馈时每拍强制解锁，并跳过整个锁存块。 */
    /* ★2026-10-18：行进中是否让锁存生效，由参数 0x011A 决定（**默认 0**）。
     *
     * 用户需求（原话）："切换到角度环模式时，它就是锁住当前的角度一直往前走；
     *   除非下达转角度命令改变目标角度，或者它在循迹。"
     * ⇒ 行进中**必须**一直闭环，不许锁存。
     *
     * 为什么以前会失效：锁存只要 |err|<settle_deg 且 |角速度|<settle_rate 持续 settle_ms
     * 就置 s_settled=1 并把 correction 钉成 0。直行时起步目标=当前航向、err≈0 ⇒ 150ms 后
     * 立刻锁存 ⇒ 方向盘等于松手 ⇒ 车按两轮机械不对称（台架 6~7%，约 7°/s）一路往右偏；
     * 而解锁要 |err|>1.5×settle_deg(≈2.25°)，起转助推又要求 |角速度|<2°/s（车在漂 7°/s）
     * ⇒ 永不点火 ⇒ 看起来就是"没有锁角度"。
     *
     * 锁存的语义只对"原地转到点后别再微抖"成立，所以现在只在 base==0 时生效。
     * 参数保留是为了万一某台车需要退回去（0x011A=1）。 */
    {
        uint8_t moving = (base_speed_left != 0 || base_speed_right != 0) ? 1U : 0U;
        uint8_t latch_moving = (host_param_get(HP_GYRO_LATCH_MOVING, 0.0f) > 0.5f) ? 1U : 0U;
        if (ff_rate != 0.0f || (moving != 0U && latch_moving == 0U)) {
            s_settled    = 0;
            s_settle_cnt = 0U;
        }
    }

    if (s_law != 4U && ff_rate == 0.0f &&
        (host_param_get(HP_GYRO_LATCH_MOVING, 0.0f) > 0.5f ||
         (base_speed_left == 0 && base_speed_right == 0))) {
        float err_l = err_now;

        if (fabsf(err_l) < s_settle_deg * 2.0f && fabsf(s_rate_f) < s_settle_rate) {
            float need_ms = (fabsf(err_l) < s_settle_deg) ? s_settle_ms : (s_settle_ms * 6.0f);

            if (s_settle_cnt < 60000U) s_settle_cnt++;
            if (!s_settled && ((float)s_settle_cnt * GYRO_DT * 1000.0f) >= need_ms) {
                s_settled        = 1;
                s_settled_target = target_angle;
            }
        } else {
            s_settle_cnt = 0;
        }

        if (s_settled) {
            correction = 0.0f;
            s_corr_f   = 0.0f;
        }
    }

    /* 最后一道安全网：把修正量夹到一个有限范围再转 int。
     * 任何一个参数被写坏成 NaN/Inf 都不会跑到 (int) 强转那里去。 */
    if (!(correction >= -1.0e6f && correction <= 1.0e6f)) correction = 0.0f;
    s_dbg_corr = correction;                  /* 只给遥测看 */

    // 根据PID输出调整左右轮速度
    left_speed  = base_speed_left  - (int)correction;   /* 原式(2026-10-15 实测确认: 配合角速度反馈取负后, 负指令b<0 => yaw 单调增大, 平滑无摆动) */
    right_speed = base_speed_right + (int)correction;

    // 限制速度范围
    if(left_speed >  MOTOR_PWM_MAX) left_speed =  MOTOR_PWM_MAX;
    if(left_speed <  MOTOR_PWM_MIN) left_speed =  MOTOR_PWM_MIN;
    if(right_speed >  MOTOR_PWM_MAX) right_speed =  MOTOR_PWM_MAX;
    if(right_speed <  MOTOR_PWM_MIN) right_speed =  MOTOR_PWM_MIN;

    // 到位判定（只在"转到指定航向"时用）
    GyroControl_TurnUpdate(HAL_GetTick());

    // 应用速度：速度环开着就走速度环（编码器闭环），否则直接写 PWM
    if (SpeedControl_IsEnabled()) {
        SpeedControl_SetTarget(left_speed, right_speed);
    } else {
        Load(left_speed, right_speed);
    }
}

/**
 * @brief 旧接口：启动陀螺仪控制定时器
 *
 * 旧板这里是 HAL_TIM_Base_Start_IT(&htim4)，TIM4 的中断里直接调 GyroControl_Task()。
 * 新板只有 TIM6(5ms)=Motion_Task、TIM7(5ms)=Servo_Task 两个周期 tick，且都由
 * bsp_periodic_start() 在初始化时统一启动 —— 再在这里 Start/Stop 硬件定时器会误伤：
 * htim4 只是 TIM7 的别名，**Stop 会把舵机缓动 Servo_Task 停掉**。
 * 所以这个接口在新板上是空操作：上层调 Toggle_Gyro_Control() 起停的是
 * gyro_control_enabled 标志，角度环由 Motion_Task() 在 TIM6 上按 5ms 调用。
 */
void Gyro_Timer_Start(void)
{
}

/**
 * @brief 旧接口：停止陀螺仪控制定时器（新板空操作，见 Gyro_Timer_Start 的说明）
 */
void Gyro_Timer_Stop(void)
{
}

/**
 * @brief 向左转（调整目标角度）
 */
void GyroControl_TurnLeft(float angle)
{
    target_angle = NormalizeAngle(target_angle + angle);   // 偏航角增大 = 向左
    gyro_pid.integral = 0.0f;
}

/**
 * @brief 向右转（调整目标角度）
 */
void GyroControl_TurnRight(float angle)
{
    target_angle = NormalizeAngle(target_angle - angle);   // 偏航角减小 = 向右
    gyro_pid.integral = 0.0f;
}

/**
 * @brief 左转固定角度（HP_TURN_ANGLE_OFFSET，默认 90 度）
 */
void GyroControl_TurnRightFixed(void)
{
    GyroControl_TurnLeft(host_param_get(HP_TURN_ANGLE_OFFSET, TURN_ANGLE_OFFSET));
}

/**
 * @brief 右转固定角度（HP_TURN_ANGLE_OFFSET，默认 90 度）
 */
void GyroControl_TurnLeftFixed(void)
{
    GyroControl_TurnRight(host_param_get(HP_TURN_ANGLE_OFFSET, TURN_ANGLE_OFFSET));
}
