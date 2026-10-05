/**
 * @file  PID_tracing.c
 * @brief 循迹环：8 路灰度 -> 位置误差 -> （差速直出 | 目标航向）
 *
 * 数据口径（沿用旧板，不改）：
 *   data = 压线通道的加权重心，范围 1..8，车在正中间时 = 4.5；
 *   丢线时保持上一次的值（all_sensors_lost），避免突然打到满舵。
 *
 * 两种用法：
 *   1) motor_tracing_pid_speed(base)：PID 输出直接当左右轮差速（旧板思路，不用陀螺仪）
 *   2) motor_tracing_cascade(base)：PID 输出变成"目标航向偏移"，
 *      交给角度环（gyro_control.c）算左右轮速度，再交给速度环（speed_control.c）
 */
#include "PID_tracing.h"

#include "motor.h"
#include "pid.h"
#include "jy61p.h"
#include "tracing.h"
#include "trace_cal.h"     /* 单帧自适应：trace_cal_frame() / TRACE_FRAME_xxx */
#include "speed_control.h"
#include "gyro_control.h"
#include "host_link.h"
#include "bsp_board.h"
#include <math.h>

#define TRACE_SETPOINT       4.5f    /* 车在线上正中时的重心值 */
#define TRACE_ANGLE_PER_UNIT_DEFAULT 30.0f  /* 重心每偏 1 格 -> 要求 30°/s 转率（出厂值，2026-10-05 定稿） */
#define TRACE_ANGLE_MAX_DEFAULT      35.0f  /* 目标航向相对**当前**航向的最大前置量（度）（出厂值） */
#define TRACE_ANGLE_PER_UNIT_MIN      0.5f  /* 台架/上位机写坏值时的兜底夹子 */
#define TRACE_ANGLE_PER_UNIT_MAX     60.0f
#define TRACE_ANGLE_MAX_LIMIT_MIN     1.0f
#define TRACE_ANGLE_MAX_LIMIT_MAX    90.0f

/* 串级外环（重心误差 -> 修改全局目标航向）的两个标定量：**真值在 host_link 的参数表里**
 * （HP_TRACE_ANGLE_PER_UNIT / HP_TRACE_ANGLE_MAX），上位机「调参 → 串级」页能改、
 * 能固化进 Flash；下面的 DEFAULT 只是出厂值/坏值兜底。
 *
 * ★2026-10-05 用户 m32880 把外环从"基准航向 + 有上限的偏移"改成"实时修改全局目标航向"
 * （见下面那段注释），两者的量纲不一样了：
 *   · per_unit 从「°/格」变成 **「(°/s)/格」**（重心偏 1 格要求多少转率）；
 *   · aim_max 从「偏移上限」变成 **「目标航向相对当前航向的前置量上限」**（防积分饱和）。
 * 第十轮受控阶梯实测（4/6/8/10/12/15/20，见 docs/自主优化记录.md 第 16 节）的"10 最好"
 * 是**老量纲**下的结论，新量纲下必须重新整定（见第 17 节）。出厂默认先沿用同一个数字。 */
#define TRACE_CROSS_MIN      4       /* ≥4 路同时压线 = 路口（说明书第七章，按线宽可改） */
#define TRACE_LOST_MS        300U    /* 丢线超过这么久就减速找线 */
#define TRACE_LOST_DECAY_MS  500U    /* 丢线超时后，再把"保持的满舵"线性收到 0 的时间 */

/* 外环每拍往目标航向上叠加的角度 = 转率 × 控制周期。控制周期 = Motion_Task() 的 TIM6
 * 5ms（firmware/App/Svc/motion.c:136），motor_tracing_cascade() 就是这个周期被调的。 */
#define TRACE_CASCADE_DT_S   0.005f
#define TRACE_RATE_MAX_DEFAULT 110.0f /* 陀螺环的期望角速度上限读不到时的兜底（°/s）*/

/* 外环为什么必须有微分项（用户 m33254："我是故意摆得斜的来检验你这个能力的?你肯定不可能
 * 直指循迹直线吧"）。
 *
 * 只有 P 项时外环要求 ω = per_unit × e，而"车沿线"的横向运动学是 ė = −v × ψ、ψ̇ = ω
 * ⇒ **ë = −v × per_unit × e，一个零阻尼简谐振荡**：ω_n = √(v × K_p)。
 * 代入实测 v ≈ 0.35 m/s、per_unit = 60（K_p ≈ 123 rad/m，1 格 ≈ 8.5mm）
 * ⇒ ω_n ≈ 6.6 rad/s ⇒ **周期 0.95s** —— 与 w 组真车日志里重心在 2.4↔6.0 格之间以 ~1s
 * 周期来回穿完全吻合（out/_drv_7000_w1_pu60_kd10.txt）。
 *
 * 零阻尼的后果是"摆多少就永远摆多少"：斜着摆车（初始航向偏差 θ0）给出的位置振幅
 * ∝ θ0 × √(v/K_p)，**per_unit 越小振幅越大**（x1/pu25 摆到 6.34 格、pu60 只到 1~2 格；
 * base 6000 时 pu30 丢线 10% 而 pu60 只 1.3%）⇒ 调增益治不好，必须加阻尼项：
 * 让要求的转率也吃"重心变化率"（微分），而不是只吃位置误差。
 *
 *     Δ = clamp(−(per_unit × e + pos_kd × ė), ±rate_max) × dt
 *
 * 二阶特征方程 s² + v·K_d·s + v·K_p = 0，阻尼比 ζ = (K_d/2)·√(v/K_p)：
 * pos_kd = 13 时 ζ ≈ 0.7 @per_unit=60、ζ ≈ 1.0 @per_unit=30（单位 = °/s per (格/s)）——
 * 斜摆/歪着进弯的初值会像二阶欠阻尼那样**衰减**，而不是一直摆。
 * ė 只在**新的一帧**（50Hz，用 trace_pos_frames 当探针）上算，且一阶低通压噪。*/
#define TRACE_POS_RATE_LPF   0.5f     /* 重心变化率的一阶低通系数（每个新帧一次）*/
#define TRACE_FRAME_HZ       50.0f    /* 灰度帧率（20ms 一帧）*/
#define TRACE_POS_KD_DEFAULT 12.0f    /* 外环微分项出厂值（°/s per (格/s)），2026-10-05 定稿 */
#define TRACE_POS_KD_MAX     1000.0f  /* 参数表里是坏值时的兜底夹子 */

/* 转率前馈通道自己的低通 + 死区（2026-10-05 第 8 轮，修用户 ≈m33762「加了以后感觉比之前
 * 还差了, 晃来晃去的」）。详见文件头「前馈为什么要自己滤一道」。
 *   · TRACE_FF_LPF：每个新灰度帧一次的一阶低通，α=0.25 @50Hz ⇒ 截止 ≈2Hz，
 *     10~25Hz 的帧级抖动衰减 4~7 倍；
 *   · TRACE_FF_MIN：|前馈| 低于这个值 (°/s) 就输出精确 0 —— gyro_control.c 里
 *     "误差小就把转率要求清零"和"到位锁存"都靠 ff_rate == 0.0f 判定，低通是渐近的、
 *     永远不等于 0，不清零的话那些保护就再也回不来了。 */
#define TRACE_FF_LPF     0.25f
#define TRACE_FF_MIN     1.0f

/* 循迹外环怎么改"全局目标航向"（用户 m32880 定案，2026-10-05）。
 *
 * 老写法：`目标航向 = s_base_yaw + clamp(-per_unit × err, ±aim_max)`，s_base_yaw 是
 * "进串级那一刻"的航向（只在 |err| < TRACE_BASE_BAND 0.15 格时才重记）。⇒ 在一条弯道上
 * 目标航向最多只能相对进模式时的航向推进 ±aim_max，一顶死就退化成开环直冲：真车
 * base=3000、车上残留的 per_unit=6 / aim_max=12 时 4s 丢线 29.9%（out/_drv_c1.txt）。
 *
 * 新写法：目标航向是一份**全局的、每拍都被修改**的量，只存在 gyro_control.c 的
 * `target_angle` 里（发车时 GyroControl_Start() 把它定成当前航向），循迹环往里叠加增量，
 * 并且**把同一个转率直接前馈给内环速率环**（2026-10-05 第 6 轮，用户 m33454「你继续就行」）：
 *
 *     rate = clamp(-(per_unit × err + pos_kd × ė), ±rate_max)          [°/s，>0 = 向左]
 *     GyroControl_SetTraceRate(rate)        ← 主渠道：直接进内环速率环（低滞后）
 *     GyroControl_AimRelative(rate × dt)    ← 副渠道：积分成目标航向（航向记忆/找线回正）
 *
 *   · per_unit 量纲 = (°/s)/格：误差 1 格 = 要求 10°/s 转率；
 *   · pos_kd 是外环的**微分（阻尼）项**（文件头「为什么外环必须有微分项」），量纲 =
 *     °/s per (格/s)，真值在 HP_LINE_KD；没有它这个环就是零阻尼振荡（斜着摆车永远摆下去）;
 *   · rate_max 借用陀螺环自己的期望角速度上限 s_rate_max(HP_GYRO_RATE_MAX, 110°/s)，
 *     保证"外环要求的转率"不超过"内环跟得上的转率"；
 *   · 防积分饱和只留一条夹子：|目标 − 当前实际航向| ≤ aim_max。车在弯道上时当前航向
 *     跟着目标一起走，所以这条夹子**不会**像老基准那样把弯道能力锁死成 ±十几度；
 *   · 丢线时（data 是冻住的旧值）不用这个误差叠加，免得拿着过期误差把目标角推到天上；
 *     改为按最后看到线的方向持续转（找线，见 TRACE_SEARCH_DELAY_MS），找线同样走前馈。
 *
 * ★为什么要前馈（第 6 轮的实测结论，别退回去）：只把转率积成目标航向时，"外环给转率 →
 * 内环角度环 → 车身真的转"这条链是 τ ≈ 1/ang_kp ≈ 285ms 的一阶滞后（rate_ref = ang_kp×err），
 * 而外环自己的 ω_n = √(v·per_unit) ≈ 5.9 rad/s（周期 1.07s）⇒ **内环滞后正好压在 ω_n 上、
 * 外环几乎没有相位裕度**，实测就是那个 ~1s 的等幅大摆（line_kd 0/13/25 三轮 |e| 0.97/0.78/0.47，
 * 有一点用但远不是"阻尼旋钮"）。极点分析（out\_poles.py）证明外环微分项救不了：
 * Routh 门槛 line_kd > τ·per_unit ≈ 17，过了门槛 ω_n 被推高、阻尼比反而趋近 0.01。
 *
 * ★前馈为什么要自己滤一道（2026-10-05 第 8 轮，用户 ≈m33762「加了以后还感觉比之前还差了,
 * 晃来晃去的」）：上面公式里的 pos_kd × ė 是**对 50Hz 灰重心做差分**算出来的，噪声增益很高。
 * 老结构（只把转率积成目标航向）里这个噪声先变成"目标航向每拍抖 0.3°"，再被内环角度环
 * (ang_kp=3.5) 衰减，落到内环转率要求上只剩 0.8~2.1 °/s；改成前馈后同一个噪声**直接**进
 * 速率环，实测 rate_ref 的帧级抖动 29.5~31.9 °/s（out\_noise.py 的 pstdev，差 15~40 倍），
 * 而 rate_kp=250 × 30°/s = 7500 > corr_limit 7000 ⇒ **光噪声就把修正量打满**，两个轮子
 * 每帧被反向猛拽 = 肉眼看得到的"晃来晃去"。所以前馈这条路必须自己滤：
 *   · 一阶低通 TRACE_FF_LPF（α=0.25 @50Hz ≈ 2Hz 截止）：10~25Hz 抖动衰减 4~7 倍，
 *     而外环真正要用的控制信号（~1Hz 的摆、起步那一下）相位滞后只有十几度；
 *   · 1°/s 死区 TRACE_FF_MIN：小到可以忽略时输出精确 0，把内环那些"ff_rate == 0"的保护
 *     还给 gyro_control.c；
 *   · 副渠道 GyroControl_AimRelative 仍吃**未滤**的 rate_cmd —— 那条路本质是积分，
 *     噪声自己会被抹平，而它的滞后正是外环需要的（积分 = 航向记忆）。
 * 前馈把"期望转率→实际转率"变成低滞后通道，外环退化成干净二阶
 * ë = −v(per_unit·e + line_kd·ė) ⇒ ζ = (line_kd/2)√(v/per_unit)，line_kd=13 时 ζ≈0.7。
 * 前馈是**消费式**的（gyro_control 每拍读走即清），所以每拍都要重新给，本函数开头先给 0。
 *
 * 符号约定（与 gyro_angles 一致）：偏航角**增大 = 向左转**。
 * 线偏到车的右侧（data > 4.5）要向右转 -> 目标航向要减小 -> Δ < 0 ✓。 */

/* 「这帧到底有没有线」的对比度门（用户 m29852）。
 * 单帧自适应把本帧最黑那一路归一化到满量程，所以一片均匀表面（车被支起来、探头底下
 * 没线、地面反光或本来就是黑地面）也会变成"8 路都挺黑"，加权重心正好落在 4.5 = 正中
 * ⇒ err ≈ 0、lost = 0，车就"以为线在正中"直着走。2026-10-05 真车+RAM 实测：探头底下
 * 无线（车支起来）时 8 路读数 200~251、重心 4.68、RAM 里 state 仍是 TRACE_FRAME_OK、
 * whiteish = 0 ⇒ 就是这个 bug。判据：最亮那一路也得有最黑那一路的 TRACE_UNIFORM_MIN
 * 倍以上，才算"有明暗结构 = 有线"；真线必然至少一侧明显比黑带亮。 */
#define TRACE_UNIFORM_MIN    0.75f
#define TRACE_LOST_STOP_MS   2500U   /* 连续丢线超过这么久：停车，不再闷头直冲（用户 m29239/m29852）
                                      * ★2026-10-06：1000 → 2500。直角扫回来本来就要时间，
                                      * 1s 太紧 —— 真车实测 c2/c5 就是"扫到一半被自己停掉"
                                      * （末帧 pwm≈0、车趴在弯口）。*/

/* 丢线找线（用户 m33109：直角弯"拐不过去"）。
 * 老行为：线一丢，目标航向就冻住 -> 车按丢线那一刻的航向**直着冲**，弯越急冲出去越快；
 * 真车 base=7000 实测（out/_drv_7000_v0_pu60_kd4.txt）3.6s 在弯口丢线，之后就一路直行
 * 开出了白色赛道（8 路灰度全黑）。
 * 新行为：先按原航向走 TRACE_SEARCH_DELAY_MS（容忍个别弱帧/过缝），还在丢就朝
 * **最后看到线的方向**持续转（TRACE_SEARCH_RATE ≈ 内环能跟的满转率），一路扫到
 * TRACE_LOST_STOP_MS 还没找到才停车。方向取最后一次有效帧的 err 符号。*/
#define TRACE_SEARCH_DELAY_MS 120U    /* 丢线先保持航向这么久（ms）*/
#define TRACE_SEARCH_RATE     170.0f  /* 找线时的转率（°/s），会被期望角速度上限夹住。
                                       * ★2026-10-06：110 → 170。直角实测扫不回来（转速不够，
                                       * 90° 里只转进去 40~60°），而内环现在给得起
                                       * （trace_rate_max=300、corr_ref=1500 ⇒ corr cap 14000）。*/
/* ★2026-10-06 找线方向的鲁棒化。原来那句是
 *     if (err > 0.05f) s_search_dir = ±1;
 * 用的是**最后一帧的符号**，死区只有 0.05 格 = 灰度重心的噪声量级 ⇒ 车在轻微摆动时丢线，
 * 方向基本是随机的。真车证据（第 32 轮 c1 vs c6：同一套参数、摆位几乎一样、Δyaw 0° vs 9.2°）：
 *     yaw 整趟变化 +60.6° vs −58.3° —— **朝相反方向拐**，参数不可能让同一配置变号。
 * 现在先过一道短 EMA（τ≈80ms@50Hz）再带 0.3 格死区取符号：反映"最近约百毫秒线持续偏在哪
 * 一侧"，单帧噪声翻不动它；丢线期间不更新（这个块只在真的看到线时执行）⇒ 方向被冻住。 */
#define TRACE_SEARCH_DIR_LPF   0.25f  /* err 的短 EMA 系数（每个有效灰度帧一次） */
#define TRACE_SEARCH_DIR_DEAD  0.30f  /* 死区（格）—— 0.05 太小，会被重心噪声翻向 */

/* ★2026-10-06 看见线时的转率上限（与"找线转率"必须分开）。
 * 为什么：直角修好之后用户 m 报「拐回来之后，它一直震荡在直线上」。查 out/_drv_7000_fix3.txt：
 * 拐过弯那段 corr 在两个方向之间打到 **±14000**、车速跟着在 20~250mm/s 抽、pos 以 ~750ms 周期
 * 在 1.9↔6.8 之间扫 —— **继电器极限环**，正是 memory 里「cap 越大『最小一次补力矩』越粗」
 * 那条（第 21 轮 cap>7200 的代价）。
 * 机理：`rate_ref` 被 s_trace_rate_max(300) 夹住 + rate_kp=40 ⇒ corr 最大能到 12000，
 * 而 `corr = rate_kp×(rate_ref − v)`：v 只要反向一点，corr 就跳到全尺度 ⇒ bang-bang。
 * ⇒ 看见线时把**需求**夹在 120（corr ≤ 40×120 = 4800，落在实测"cap 3000~7000 不起振"
 * 的范围里）；**找线时仍走 TRACE_SEARCH_RATE(170)** —— 找线本来就是一次粗扫，那里"粗"是需要的。 */
#define TRACE_RATE_MAX_VISIBLE 120.0f

/* ★2026-10-06 基准速度的斜坡。三个工况（看见线 7000 / 路口 3500 / 找线 0）之间原来是**硬切**，
 * 而循迹模式下轮速环的斜坡被特意放成 100000 PWM/s（motion.c:121 —— 为了让角度环的修正能
 * 立刻反向，那里不能动）⇒ **基准一跳，左右轮目标就跟着跳**，出弯那一刻等于给车一个加速度
 * 阶跃，实测就是用户 m 说的「拐回来之后在直线上轻微摆」。
 * 这里只给**基准**加一道一阶跟随（修正量那条路一点不动，仍然瞬时反向）。
 * 副作用是发车也变成软启动（基准从 0 爬上去），与速度环自带的软启动叠加，符合预期。 */
#define TRACE_BASE_TAU_S   0.10f   /* 基准一阶时间常数（s）：7000→0 约 0.3s 走到 95% */

#define TRACE_EDGE_SAT       0.9f    /* 最外一路黑度 ≥ 0.9×最黑 = 黑带被阵边界切掉（外推的唯一入口）*/
#define TRACE_BARW_INIT      3.0f    /* 黑带宽度初值（格=传感器间距），前面几帧就会自己收敛 */
#define TRACE_BARW_TAU       0.02f   /* 宽度跟踪系数（50Hz -> 约 1s 时间常数）*/
#define TRACE_POS_MED_N      3U      /* 位置中值窗（帧）：丢单帧脉冲，只滞后 1 帧 = 20ms */

static float   s_setpoint = TRACE_SETPOINT;
static float   data       = TRACE_SETPOINT;   /* 当前重心 */
static float   last_data  = TRACE_SETPOINT;
static uint8_t all_sensors_lost = 0;
static uint8_t s_lost_stop       = 0;   /* 丢线停车已经执行过（每次丢线只执行一次）*/
static int8_t  s_search_dir      = 0;   /* 最后一次看到线时它偏在哪一侧：+1 右 / -1 左（丢线找线用）*/
static float   s_err_dir         = 0.0f;/* err 的短 EMA（τ≈80ms@50Hz）：丢线找线判方向用，见 TRACE_SEARCH_DIR_LPF */
static float   s_base_f          = 0.0f;/* 基准速度的一阶平滑值（PWM 刻度），见 TRACE_BASE_TAU_S */
static float   last_pid_output  = 0.0f;

/* 外环微分项（阻尼）的状态：重心变化率 ė（格/s），只在新的灰度帧上更新 */
static float    s_pos_rate    = 0.0f;   /* ė 的一阶低通值 */
static float    s_prev_data_f = TRACE_SETPOINT;
static uint32_t s_rate_frame  = 0U;     /* 上次算微分时的 trace_pos_frames */
static uint8_t  s_rate_valid  = 0U;     /* 已经有过一帧（第一帧不产生微分冲击）*/
static uint8_t  s_rate_resync = 1U;     /* 上一帧是丢线帧：重新压线那一下 data 会跳变，微分项重新起步 */
static float    s_ff_lp       = 0.0f;   /* 转率前馈的一阶低通值（只在新的一帧上更新，≈2Hz）*/

/* 边缘外推（用户 m07972 第 1 条）*/
static float   s_bar_w    = TRACE_BARW_INIT;
static uint8_t s_bw_valid = 0U;

/* 位置中值滤波 + 抖动统计（用户 m07972 第 4 条）*/
static float   s_ph[TRACE_POS_MED_N];
static uint8_t s_ph_n      = 0U;
static float   s_prev_raw  = 0.0f;
static float   s_prev_filt = 0.0f;

/* 可见状态（给台架/真车读，不参与控制；声明在 PID_tracing.h）*/
float    trace_bar_w        = TRACE_BARW_INIT;  /* 跟踪到的黑带宽度（格）*/
float    trace_pos_jit_raw  = 0.0f;             /* Σ|重心 - 上帧重心|（滤波前）*/
float    trace_pos_jit_filt = 0.0f;             /* 同上，滤波后 */
uint32_t trace_pos_frames   = 0U;               /* 统计帧数（均值 = 上面两个 / 它）*/
uint32_t trace_edge_cnt     = 0U;               /* 累计触发外推的帧数 */
uint8_t  trace_edge_extrap  = 0U;               /* 本帧是否触发外推 */

/* 消抖：连续 2 次一样才认（每 5ms 采一次 -> 10ms 消抖） */
static uint8_t s_stable_raw = 0;
static uint8_t s_pend_raw   = 0;
static uint8_t s_pend_n     = 0;

/* 路口 */
static uint8_t  s_cross       = 0;
static uint32_t s_cross_at    = 0;
static uint8_t  s_cross_cnt   = 0;
static uint32_t s_lost_since  = 0;

/* 串级：本环"发车"了没有。0 = 下一拍把全局目标航向钉成当前航向
 * （用户 m32880：发车时目标角度 = 当前角度），之后每拍只往那个目标上叠增量。 */
static uint8_t s_base_valid = 0;

static void apply_speeds(float left, float right)
{
    int li = (int)left;
    int ri = (int)right;

    if (li >  MOTOR_PWM_MAX) li =  MOTOR_PWM_MAX;
    if (li <  MOTOR_PWM_MIN) li =  MOTOR_PWM_MIN;
    if (ri >  MOTOR_PWM_MAX) ri =  MOTOR_PWM_MAX;
    if (ri <  MOTOR_PWM_MIN) ri =  MOTOR_PWM_MIN;

    if (SpeedControl_IsEnabled()) {
        SpeedControl_SetTarget(li, ri);     /* 交给速度环（编码器闭环） */
    } else {
        Load(li, ri);                       /* 速度环没开就直接写 PWM */
    }
}

/**
 * @brief 读 8 路灰度 -> 加权重心 data / all_sensors_lost / 路口
 *
 * 两条路（判线口径 2026-10-05 用户 m31902 整改后，详见 trace_cal.h「判线口径」）：
 *   ① 标定阈值 + 离阈值的距离（trace_cal_frame 回的 8 路权重：0 = 没越过阈值、1 = 到标定
 *      黑值）：权重直接当重心权重（1..8，正中 4.5），过本帧半高算这一路压线；阈值是标定
 *      出来的，白侧权重恒 0，所以整片路面变亮/变暗不会把重心往中间拽。
 *      "整片一个样"（离地/全黑地面）由下面的 TRACE_UNIFORM_MIN 明暗结构判据兜（见 ⓪）。
 *   ② 老的中点阈值二值法（没标定 / 没模拟量 / trace_adapt_enable = 0）：消抖 + 8 个 0/1 取平均。
 */
void read_tracing_data_pid(void)
{
    float   w[TRACE_CAL_CH];
    trace_frame_t fr = trace_cal_frame(w);
    float   num = 0.0f, sum = 0.0f, wmax = 0.0f, wmin = 0.0f;
    uint8_t raw = 0;
    int     i, on;
    uint32_t now = HAL_GetTick();
    uint8_t lost;

    if (fr == TRACE_FRAME_OK) {
        /* ---- ① 连续加权重心（L1..L4 = 1..4，R1..R4 = 5..8） ---- */
        for (i = 0; i < 8; i++) {
            if (w[i] > wmax) wmax = w[i];
            if (i == 0 || w[i] < wmin) wmin = w[i];
        }
        on  = 0;
        num = 0.0f;
        sum = 0.0f;
        for (i = 0; i < 8; i++) {
            /* 过本帧半高 = 这一路算压线：和柱子上的那条分界线、界面底部的 0/1 完全一致 */
            if (w[i] * 2.0f > wmax) on++;
            num += w[i];
            sum += w[i] * (float)(i + 1);
        }
        /* ---- ⓪ 先判"这帧到底有没有线"（用户 m29852）----
         * 均匀表面（探头底下没线 / 车被拿走 / 整片一色）也会有一套稳定的权重，
         * 重心可能正好落在 4.5 = 正中 ⇒ 车"以为线在正中"直着走。最亮那一路也得
         * 暗不到最黑那一路的 TRACE_UNIFORM_MIN 倍以上，才算有明暗结构 = 有线；
         * 否则判丢线。权重本身已经按标定阈值折算过，这里只看"有没有结构"。 */
        if (wmax <= 0.0f || wmin > TRACE_UNIFORM_MIN * wmax) {
            lost = 1U;
            on   = 0;   /* 均匀帧以前 on = 8，还会顺手误报"路口" */
        } else if (num > 0.0f) {
            float pos   = sum / num;   /* 加权重心：1..8 */
            float cover = 0.0f;
            float m;

            for (i = 0; i < 8; i++) cover += w[i];   /* w[] 已归一化，Σ = 可见覆盖长度（格）*/

            /* ---- ①.5 边缘外推（用户 m07972 第 1 条）----
             * 线跑出阵外时，可见的那部分把重心顶在 1（或 8），误差封顶 -> aim 也就顶在 ±35°。
             * 用黑带宽度把线心推回阵外：可见覆盖长度 cover = Σ 归一化黑度，
             *     右边界被切：线心 = 0.5 + cover - 黑带宽度/2
             *     左边界被切：线心 = 8.5 - cover + 黑带宽度/2
             * 只在"最外那一路黑度 ≥ 0.9×最黑"时启用 —— 这恰好等价于"黑带的外边界已经越过
             * 阵边界"（那一路全被盖住）；没被切时最外路必然盖不满，重心本身就是准的，
             * 一个计数都不动。用户说线基本跑不出阵，所以平时这条永不生效（trace_edge_cnt 可查）。
             * 黑带宽度用慢速 EMA 学，且只用没被切的帧更新（被切的帧量不到完整宽度）。*/
            trace_edge_extrap = 0U;
            if (wmax > 0.0f) {
                uint8_t cut_l = (w[0] >= TRACE_EDGE_SAT * wmax) ? 1U : 0U;
                uint8_t cut_r = (w[7] >= TRACE_EDGE_SAT * wmax) ? 1U : 0U;

                if (!cut_l && !cut_r) {
                    if (!s_bw_valid) { s_bar_w = cover; s_bw_valid = 1U; }
                    else             { s_bar_w += (cover - s_bar_w) * TRACE_BARW_TAU; }
                }
                if (s_bar_w < 1.0f) s_bar_w = 1.0f;
                if (s_bar_w > 6.5f) s_bar_w = 6.5f;
                trace_bar_w = s_bar_w;

                if (cut_l) {
                    float p = 0.5f + cover - s_bar_w * 0.5f;
                    if (p < pos) { pos = p; trace_edge_extrap = 1U; }
                } else if (cut_r) {
                    float p = 8.5f - cover + s_bar_w * 0.5f;
                    if (p > pos) { pos = p; trace_edge_extrap = 1U; }
                }
                if (trace_edge_extrap) trace_edge_cnt++;
                if (pos < -2.0f) pos = -2.0f;
                if (pos > 11.0f) pos = 11.0f;
            }

            /* ---- ①.6 位置 3 帧中值（用户 m07972 第 4 条）----
             * 要求"滤一下波但别那么滞后"：中值只滞后 1 帧（20ms），单帧脉冲直接丢掉，
             * 斜坡信号不像低通那样越滤越迟。抖动统计把它和滤波前都累起来（除以帧数得均值）。*/
            if (s_ph_n < TRACE_POS_MED_N) {
                s_ph[s_ph_n++] = pos;
                m = pos;
            } else {
                float a, b, c;
                s_ph[2] = s_ph[1];
                s_ph[1] = s_ph[0];
                s_ph[0] = pos;
                a = s_ph[0]; b = s_ph[1]; c = s_ph[2];
                m = (a < b) ? ((b < c) ? b : ((a < c) ? c : a))
                            : ((a < c) ? a : ((b < c) ? c : b));
            }

            if (trace_pos_frames > 0U) {
                trace_pos_jit_raw  += fabsf(pos - s_prev_raw);
                trace_pos_jit_filt += fabsf(m   - s_prev_filt);
            }
            s_prev_raw  = pos;
            s_prev_filt = m;
            trace_pos_frames++;

            data      = m;
            last_data = data;
            lost      = 0U;
        } else {
            /* 8 路权重全是 0：本帧一个位置信息都没有，别再假装"线在正中" */
            lost = 1U;
        }
    } else if (fr == TRACE_FRAME_WEAK) {
        /* 模拟量在，但这帧对比度不够（没信息）：当丢线，data 保持上一帧 */
        on   = 0;
        lost = 1U;
    } else if (trace_cal_mode == TRACE_MODE_DIGITAL) {
        /* ---- ② 老的固定阈值 + 消抖（只有用户显式把「灰度接口」设成数字量时才拿
         *      裸 PA0~PA7 当真值，见 trace_cal.h:130-138 的设计）---- */
        for (i = 0; i < 8; i++) {
            if (trace_read_channel((uint8_t)i)) raw |= (uint8_t)(1U << i);
        }
        if (raw == s_stable_raw) {
            s_pend_n = 0;
        } else {
            if (raw == s_pend_raw) {
                if (s_pend_n < 2U) s_pend_n++;
            } else {
                s_pend_raw = raw;
                s_pend_n   = 1U;
            }
            if (s_pend_n >= 2U) {
                s_stable_raw = raw;
                s_pend_n     = 0U;
            }
        }
        raw = s_stable_raw;

        on  = 0;
        num = 0.0f;
        sum = 0.0f;
        for (i = 0; i < 8; i++) {
            if (raw & (uint8_t)(1U << i)) {
                num += 1.0f;
                sum += (float)(i + 1);
                on++;
            }
        }
        if (num > 0.0f) {
            data      = sum / num;
            last_data = data;
        }
        lost = (on == 0) ? 1U : 0U;
    } else {
        /* ---- ③ 没有模拟量、又不是用户显式选的数字口：判丢线 ----
         * 裸 PA0~PA7 在这台车上恒为高（模拟量模式下模块根本不驱动它们），当掩码读只会
         * 得到 0xFF ⇒ 重心 4.5 + lost = 0 = "以为线在正中"直着走 —— 用户 m29852 现场
         * 那 236 个样本全是这个状态。data 保持上一帧，丢过头由串级停车。 */
        on   = 0;
        lost = 1U;
    }
    /* 丢线时 data 保持不变（= 上一次的重心），方向就不会突然反打 */
    all_sensors_lost = lost;

    if (all_sensors_lost) {
        if (s_lost_since == 0U) s_lost_since = now;
    } else {
        /* 刚重新找到线：把循迹 PID 清一次。丢线期间 data 保持不变、误差也不变，
         * 积分照样在累；不清的话重新压线那一拍会把攒下的积分一次性放出来，
         * 表现为"找回线就猛打一下方向"。 */
        if (s_lost_since != 0U) PID_Reset();
        s_lost_since = 0U;
    }

    /* 路口：≥TRACE_CROSS_MIN 路同时压线，连续 2 次才认，认完 200ms 内不重复报 */
    if (on >= TRACE_CROSS_MIN) {
        if (s_cross_cnt < 2U) s_cross_cnt++;
        if (s_cross_cnt >= 2U && !s_cross && (uint32_t)(now - s_cross_at) > 200U) {
            s_cross    = 1U;
            s_cross_at = now;
        }
    } else {
        s_cross_cnt = 0U;
        if (on <= TRACE_CROSS_MIN - 2) s_cross = 0U;
    }
}

uint8_t tracing_cross(void)
{
    return s_cross;
}

/* 给遥测用：算法自己判的丢线（不是 PA0~PA7 裸脚那个永远失真的"丢线"）。 */
uint8_t tracing_lost(void)
{
    return all_sensors_lost;
}

void tracing_cross_clear(void)
{
    s_cross = 0U;
}

void show_tracing_data_pid(void)
{
    /* 显示交给 KK_UI */
}

/**
 * @brief 循迹环 + 速度环（差速直出，不用陀螺仪）
 */
void motor_tracing_pid_speed(int base_speed)
{
    float pid_output, l, r;

    read_tracing_data_pid();

    /* base=0 就是"原地对转"（pivot）：左右轮反向、车不前进。这时被控对象和
     * 前进循迹完全不同，用 pid.c 里实测标出来的 pivot 那组增益；非 0 还是走
     * 参数表里的前进增益。模式切换时 PID_SetPivot() 内部会清积分/微分历史。 */
    PID_SetPivot(base_speed == 0);

    if (all_sensors_lost) {
        /* 丢线分两段：短时间保持上一次的转向（等它扫回来）；超过 TRACE_LOST_MS 之后
         * 必须把"保持的转向"收掉 —— last_pid_output 是丢线前那一刻的值，如果那次正好是
         * 满舵（追大偏差时就是 7200），一直保持就会变成"原地打转停不下来"。
         * 实测（2026-10-02 台架）：设定值从 4.5 切到 2.0 时线甩出探头，reqL 卡在 7200
         * 转了 4 秒以上、yaw 绕了 360°+。这里按 TRACE_LOST_DECAY_MS 线性收到 0。 */
        uint32_t lost_ms = (s_lost_since != 0U) ? (uint32_t)(HAL_GetTick() - s_lost_since) : 0U;

        if (lost_ms > TRACE_LOST_MS) {
            uint32_t over = lost_ms - TRACE_LOST_MS;
            float k = (over >= TRACE_LOST_DECAY_MS)
                        ? 0.0f
                        : (1.0f - (float)over / (float)TRACE_LOST_DECAY_MS);
            pid_output = last_pid_output * k;
            base_speed = base_speed / 2;              /* 丢太久：减速找线 */
        } else {
            pid_output = last_pid_output;             /* 短暂丢线：保持上一次的转向 */
        }
    } else {
        pid_output    = PID_Compute(s_setpoint, data);
        last_pid_output = pid_output;
    }

    l = (float)base_speed - pid_output;
    r = (float)base_speed + pid_output;

    apply_speeds(l, r);
}

/**
 * @brief 设置三级串级的外环标定量（重心误差 -> 转率 / 目标航向相对当前航向的前置量上限）
 *
 * 这两个量的真值现在放在 host_link 的参数表里（HP_TRACE_ANGLE_PER_UNIT / _MAX），
 * 所以这个 setter 就是"写参数表"；超范围的写不进（参数表自己带上下限）。
 * 量纲见文件头那段「循迹外环怎么改全局目标航向」：per_unit = (°/s)/格、
 * aim_max = 前置量上限(°)。出厂兜底 10.0f / 35.0f。
 */
void PID_Tracing_SetCascadeGains(float deg_per_unit, float aim_max_deg)
{
    if (deg_per_unit >= TRACE_ANGLE_PER_UNIT_MIN && deg_per_unit <= TRACE_ANGLE_PER_UNIT_MAX) {
        host_param_set_local(HP_TRACE_ANGLE_PER_UNIT, deg_per_unit);
    }
    if (aim_max_deg >= TRACE_ANGLE_MAX_LIMIT_MIN && aim_max_deg <= TRACE_ANGLE_MAX_LIMIT_MAX) {
        host_param_set_local(HP_TRACE_ANGLE_MAX, aim_max_deg);
    }
}

/* 读值时兜一层：参数表里是 0/NaN/巨大值时用出厂值，绝不把车带飞 */
static float cascade_per_unit(void)
{
    float v = host_param_get(HP_TRACE_ANGLE_PER_UNIT, TRACE_ANGLE_PER_UNIT_DEFAULT);
    if (!(v >= TRACE_ANGLE_PER_UNIT_MIN && v <= TRACE_ANGLE_PER_UNIT_MAX)) {
        v = TRACE_ANGLE_PER_UNIT_DEFAULT;
    }
    return v;
}

static float cascade_aim_max(void)
{
    float v = host_param_get(HP_TRACE_ANGLE_MAX, TRACE_ANGLE_MAX_DEFAULT);
    if (!(v >= TRACE_ANGLE_MAX_LIMIT_MIN && v <= TRACE_ANGLE_MAX_LIMIT_MAX)) {
        v = TRACE_ANGLE_MAX_DEFAULT;
    }
    return v;
}

/* 外环微分项增益（°/s per (格/s)）。参数表里复用了 HP_LINE_KD —— 老 mode-2 循迹 PID
 * （motor_tracing_pid_speed）在现在的固件里没有任何调用者，那套 Kp/Ki/Kd 是死的；
 * 但"循迹 D 项"这个名字与量纲正好是这里要的，所以 2026-10-05 起它归串级外环用（用户 m33254）。*/
static float cascade_pos_kd(void)
{
    float v = host_param_get(HP_LINE_KD, TRACE_POS_KD_DEFAULT);
    if (!(v >= 0.0f && v <= TRACE_POS_KD_MAX)) {
        v = TRACE_POS_KD_DEFAULT;
    }
    return v;
}

void PID_Tracing_GetCascadeGains(float *deg_per_unit, float *aim_max_deg)
{
    if (deg_per_unit != 0) { *deg_per_unit = cascade_per_unit(); }
    if (aim_max_deg  != 0) { *aim_max_deg  = cascade_aim_max(); }
}
/**
 * @brief 三层串级：灰度误差 -> 修改全局目标航向 -> 角度环 -> 左右轮目标速度 -> 速度环
 *
 * 目标航向只有一份，归角度环持有（gyro_control.c 的 target_angle，发车时被
 * GyroControl_Start() 定成当前航向）；本函数每拍按灰度重心误差把它往线所在的方向推。
 * 详细口径见文件头「循迹外环怎么改全局目标航向」（用户 m32880 定案）。
 */
void motor_tracing_cascade(int base_speed)
{
    float err;

    read_tracing_data_pid();

    /* 本拍默认没有转率前馈：只有下面"真的在看线"和"找线"两个分支会给出值。
     * 前馈是消费式的（gyro_control 每拍读走即清），所以每拍都必须重新给，
     * 别指望上一拍的值还在。 */
    GyroControl_SetTraceRate(0.0f);

    err = data - s_setpoint;                 /* >0：线在车的右侧 */

    if (all_sensors_lost) {
        /* 丢线期间有效帧不再产生（trace_pos_frames 不前进），重新压线那一帧 data 会从冻住的
         * 旧值跳回来 ⇒ 标一下，让微分项重新起步，别把这个跳变当成"重心在飞"。*/
        s_rate_resync = 1U;
        s_ff_lp       = 0.0f;   /* 前馈低通也重新起步：丢线期间它冻在旧值上，压回线时别拿旧值冲一下 */
    }

    if (!s_base_valid) {
        /* 发车：全局目标航向 = 当前航向。GyroControl_Start() 进模式时已经做过一次，
         * 这里再钉一遍，保证不管从哪条路（上位机/任务/台架）进串级，起点都是"现在朝哪"。*/
        s_base_valid = 1U;
        GyroControl_SetTargetAngle(NormalizeAngle(Yaw));
    } else if (!all_sensors_lost) {
        /* 只在这一拍**真的看到线**时才改目标航向：丢线时 data 是冻住的旧值，
         * 继续积分就是拿着一个过期误差把目标角一路推到天上（积分饱和）。*/
        float per_unit = cascade_per_unit();
        float pos_kd   = cascade_pos_kd();
        float rate_max = (s_trace_rate_max > 0.0f) ? s_trace_rate_max : TRACE_RATE_MAX_DEFAULT;
        float rate_cmd;
        float d;
        float ff_out;
        uint8_t new_frame = (trace_pos_frames != s_rate_frame) ? 1U : 0U;

        /* 重心变化率 ė（格/s）：只在新的一帧上更新（50Hz），一阶低通压噪。
         * 它给外环提供阻尼 —— 没有它外环就是零阻尼振荡（见文件头「为什么必须有微分项」）。*/
        if (new_frame) {
            float dp = 0.0f;

            if (s_rate_valid && !s_rate_resync) dp = data - s_prev_data_f;
            s_rate_valid  = 1U;
            s_rate_resync = 0U;
            s_prev_data_f = data;
            s_rate_frame  = trace_pos_frames;
            s_pos_rate += (dp * TRACE_FRAME_HZ - s_pos_rate) * TRACE_POS_RATE_LPF;
        }

        /* 正负号与 P 项一致：ė > 0 = 线在往车的右边跑 = 车偏左 = 要往右转 = rate_cmd < 0 */
        rate_cmd = -(per_unit * err + pos_kd * s_pos_rate);   /* °/s，>0 = 向左 */
        /* 看见线时**用自己的（小）上限**，别用找线那个 300：见 TRACE_RATE_MAX_VISIBLE 注释。 */
        {
            float vlim = TRACE_RATE_MAX_VISIBLE;
            if (vlim > rate_max) vlim = rate_max;             /* 不越过参数表给的上限 */
            if (rate_cmd >  vlim) rate_cmd =  vlim;
            if (rate_cmd < -vlim) rate_cmd = -vlim;
        }

        /* 两条路一起给（2026-10-05 第 6 轮起）：
         *   ① 期望转率直接前馈进内环速率环 —— "跟得上线"的主渠道，低滞后；
         *   ② 同一个量积成目标航向增量 —— 外环的"航向记忆"（丢线期间不再改，见上面的分支）。
         * 为什么必须前馈：只推目标航向时，外环→内环→车身是一条 τ≈1/ang_kp≈285ms 的一阶
         * 滞后，正压在外环 ω_n=√(v·per_unit)≈5.9rad/s 上 ⇒ 几乎没有相位裕度，表现为实测
         * 的 1s 等幅大摆；此时外环微分项也治不好（Routh 门槛 + 极点分析见 out\_poles.py）。
         *
         * ★第 8 轮修的是"晃"：前馈这条路把 D 项的帧级噪声**直接**灌进速率环，实测
         * rate_ref 抖动 30°/s（老结构只有 1~2°/s），乘上 rate_kp=250 就超过 corr_limit
         * ⇒ 光噪声把修正量打满，轮子每帧被反向猛拽。所以前馈先过低通（见 TRACE_FF_LPF）。*/
        if (new_frame) {
            s_ff_lp += (rate_cmd - s_ff_lp) * TRACE_FF_LPF;
        }
        ff_out = (s_ff_lp > TRACE_FF_MIN || s_ff_lp < -TRACE_FF_MIN) ? s_ff_lp : 0.0f;
        GyroControl_SetTraceRate(ff_out);
        d = rate_cmd * TRACE_CASCADE_DT_S;
        GyroControl_AimRelative(d);

        /* 记住"线持续偏在哪一侧"，丢线找线要用（见 TRACE_SEARCH_DIR_LPF 的注释：
         * 原来直接取最后一帧符号、死区 0.05 格，方向会被重心噪声随机翻向）。 */
        s_err_dir += (err - s_err_dir) * TRACE_SEARCH_DIR_LPF;
        if (s_err_dir >  TRACE_SEARCH_DIR_DEAD) s_search_dir =  1;
        if (s_err_dir < -TRACE_SEARCH_DIR_DEAD) s_search_dir = -1;
    }

    if (all_sensors_lost && s_lost_since != 0U) {
        uint32_t lost_ms = (uint32_t)(HAL_GetTick() - s_lost_since);

        if (lost_ms > TRACE_LOST_STOP_MS) {
            /* 丢线丢到这个份上（传感器死了/车被拿走/真的完全没线）：停车，别再闷头直冲
             * （用户 m29239/m29852）。重新压线时 read_tracing_data_pid() 会 PID_Reset()，
             * 下一拍 s_lost_stop 清掉就接着跑。 */
            if (!s_lost_stop) {
                s_lost_stop = 1U;
                GyroControl_Stop();
                SpeedControl_Stop();
                Load(0.0f, 0.0f);
            }
            return;
        }
        /* ★2026-10-06 直角/丢线的速度策略（改过三版，这里是第三版，前两版都被真车否掉）：
         *   ① 只写 `base/2` → 内侧轮起始只到 0，靠 170°/s 的转率硬拖；
         *   ② 再把 corr cap 抬到 14000 去"硬顶出反转"→ **直线上被环变成继电器**：
         *      用户 m 报「拐回来之后一直震荡在直线上」，日志里 corr 两个方向打到 ±14000、
         *      车速 20~250mm/s 抽、pos 以 ~750ms 周期在 1.9↔6.8 扫。找线也会反复介入。
         *   ③ ★现在：真正开始找线时**把基准打成 0** ⇒ `left = −corr, right = +corr`
         *      = **原地对转**，正好是已经调好的 pivot 工况（base=0 ⇒ cap = corr_limit = 3000、
         *      差速 6000、实测 ~121°/s）。既拿到了"内侧轮变负"，又不用放着大 cap 去污染
         *      看见线时的环路；`corr_ref` 也就可以退回 3000。 */
        if (s_cross != 0U) base_speed = base_speed / 2;   /* 刚进路口：先减速，给探头看到拐后的腿 */
        if (lost_ms > TRACE_SEARCH_DELAY_MS) base_speed = 0;  /* 开始找线 ⇒ 原地对转（pivot 工况） */

        /* 找线：还在丢就朝"线最后偏在哪一侧"持续转（见 TRACE_SEARCH_DELAY_MS 注释）。
         * 直角弯就靠这一下拐过去 —— 目标航向冻住的话车会直着冲出赛道。
         * ★方向不再只靠 s_search_dir：它可能整趟都没被置过（车一直很正）⇒ 退化成按
         * err 短 EMA 的符号挑一边，总比"不动等着被停车"强。 */
        {
            int8_t dir = s_search_dir;
            if (dir == 0) dir = (s_err_dir >= 0.0f) ? 1 : -1;

            if (lost_ms > TRACE_SEARCH_DELAY_MS) {
                float rate  = TRACE_SEARCH_RATE;
                float rate_max = ((s_trace_rate_max > 0.0f) ? s_trace_rate_max : TRACE_RATE_MAX_DEFAULT);
                float rate_cmd = -(float)dir * rate;  /* >0 = 向左 */

                if (rate_cmd >  rate_max) rate_cmd =  rate_max;
                if (rate_cmd < -rate_max) rate_cmd = -rate_max;
                /* 找线也走"前馈 + 目标航向"两条路：前馈让车当下就按这个转率转起来
                 * （丢线时航向误差可能还很小，光推目标的话要等误差长出来才动）。 */
                GyroControl_SetTraceRate(rate_cmd);
                GyroControl_AimRelative(rate_cmd * TRACE_CASCADE_DT_S);
            }
        }
    }
    s_lost_stop = 0U;

    /* 防积分饱和：目标航向不许离开**当前实际航向**超过 aim_max。弯道上当前航向跟着
     * 目标一起走，所以这条夹子不会限制"一共能转多少度"，只限制"目标领先车头多少度"。*/
    {
        float aim_max = cascade_aim_max();
        float lead    = GyroControl_HeadingError();      /* 目标 - 当前，±180 */

        if (lead >  aim_max) GyroControl_SetTargetAngle(NormalizeAngle(Yaw + aim_max));
        if (lead < -aim_max) GyroControl_SetTargetAngle(NormalizeAngle(Yaw - aim_max));
    }

    /* 外两层：目标航向 -> 角度环 -> 左右轮目标速度。
     * ★基准先过一道一阶斜坡（见 TRACE_BASE_TAU_S）：三个工况之间不再硬切，避免给车一个
     *   加速度阶跃；**修正量那条路（corr）不过这道斜坡**，角度环该立刻反向还是立刻反向。 */
    if (s_base_f < 0.0f) {
        s_base_f = (float)base_speed;   /* 本趟第一拍：直接吸附（**不做软启动**，保持既有起步行为） */
    } else {
        s_base_f += ((float)base_speed - s_base_f) * (TRACE_CASCADE_DT_S / (TRACE_BASE_TAU_S + TRACE_CASCADE_DT_S));
        if (fabsf((float)base_speed - s_base_f) < 20.0f) s_base_f = (float)base_speed;  /* 贴住就吸附，别留指数长尾 */
    }
    if (s_base_f < 0.0f) s_base_f = 0.0f;
    if (s_base_f > (float)MOTOR_PWM_MAX) s_base_f = (float)MOTOR_PWM_MAX;
    base_speed = (int)(s_base_f + 0.5f);
    GyroControl_SetBaseSpeed(base_speed, base_speed);
    GyroControl_Task();
}

void motor_tracing_reset_cascade(void)
{
    s_base_valid    = 0U;
    s_search_dir    = 0;
    s_err_dir       = 0.0f;
    s_base_f        = -1.0f;  /* 哨兵：本趟第一拍直接吸附到目标基准（不做软启动） */
    last_pid_output = 0.0f;
    s_lost_since    = 0U;
    s_lost_stop     = 0U;
    data            = TRACE_SETPOINT;
    last_data       = TRACE_SETPOINT;
    all_sensors_lost = 0U;
    /* 微分项状态：发车这一刻没有历史帧，第一帧只用来"对表"，不产生 D 冲击 */
    s_pos_rate    = 0.0f;
    s_ff_lp       = 0.0f;
    s_prev_data_f = TRACE_SETPOINT;
    s_rate_frame  = trace_pos_frames;
    s_rate_valid  = 0U;
    s_rate_resync = 1U;
}

/* ============= 后台循迹环的基准速度（2026-09-30 m02112 同步） =============
 * 旧板 PWM 满量程只有 1000，所以旧代码里写死的是 1000（满油门）和 200（20%）；
 * 本板满量程 7200。照抄的 1000 在本板只有 13.9% 占空比（低于静摩擦死区 ~2500）⇒ 推不动。
 * 现在跟随“当前循迹”的两个参数：正常档 = 菜单里的“循迹速度”（HP_BASE_SPEED，默认 6480 = 旧 900），
 * 慢速档 = HP_BASE_SPEED_SLOW（默认 2500，不是旧 200×7.2=1440，因为 1440 仍在死区以下）。 */
static int trace_base_pwm(void)
{
    int base = (int)host_param_get(HP_BASE_SPEED, 6480.0f);
    if (base < 0)             base = 0;
    if (base > MOTOR_PWM_MAX) base = MOTOR_PWM_MAX;
    return base;
}

static int trace_base_pwm_slow(void)
{
    int base = (int)host_param_get(HP_BASE_SPEED_SLOW, 2500.0f);
    if (base < 0)             base = 0;
    if (base > MOTOR_PWM_MAX) base = MOTOR_PWM_MAX;
    return base;
}

void motor_tracing_pid(float target)
{
    s_setpoint = target;
    motor_tracing_pid_speed(trace_base_pwm());
}

/**
 * @brief 遥测只读快照（给上位机的 trace 组）
 *        `data` = 本拍重心（1..8，车在正中 4.5），`last_pid_output` = 循迹环本拍
 *        给左右轮的转向偏置。都不参与控制，只是让上位机看见环里在算什么。
 */
float PID_Tracing_TracePos(void)
{
    return data;
}

float PID_Tracing_PidOutput(void)
{
    return last_pid_output;
}

void motor_tracing_pid_0(float target)
{
    s_setpoint = target;
    motor_tracing_pid_speed(trace_base_pwm_slow());
}

void motor_tracing_pid_0_task(void)
{
    s_setpoint = TRACE_SETPOINT;
    motor_tracing_pid_speed(trace_base_pwm_slow());
}

/* ---------------------------------------------------------------------------
 * 后台循迹环的开关与时基（2026-09-30 m02112 同步）
 *
 * 旧板：任务流程（`renwu.c`，已删除）里写 `PID_Tracing_Timer_Start(); HAL_Delay(1000); PID_Tracing_Timer_Stop();`
 * ——那 1 秒里主循环阻塞在 HAL_Delay，真正循迹的是 TIM2 中断里的 motor_tracing_pid_0_task()
 * （TIM2：PSC=7199 / ARR=999 @72MHz = 10Hz）。
 *
 * 新板没有多余的周期定时器：TIM6(5ms)=Motion_Task、TIM7(5ms)=Servo_Task，而且都由
 * bsp_periodic_start() 在初始化时统一启动。这里原来写的是 HAL_TIM_Base_Start_IT(&htim2)——
 * 而 htim2 只是 TIM6 的别名，于是 Start 没事（TIM6 本来就在跑）、**Stop 会把整个 5ms 运动节拍
 * 停掉**（角度环/速度环一起停），语义完全不对。
 *
 * 现在 Start/Stop 只翻一个软件开关，真正的执行交给 Motion_Task()（TIM6 5ms 中断）：
 * 每 PID_TRACE_BG_DIV(20) 拍跑一次 = 100ms = 旧板 10Hz。必须分频——同一个 PID
 * （pid.c 的 PID_Compute，含 Kd·Δe 和 Ki·Σe）采样率快了 20 倍，D 项会强 20 倍、I 项弱 20 倍。
 * ------------------------------------------------------------------------- */
static uint8_t s_bg_on  = 0U;
static uint8_t s_bg_div = 0U;

void PID_Tracing_Timer_Start(void)
{
    s_bg_on  = 1U;
    s_bg_div = 0U;
}

void PID_Tracing_Timer_Stop(void)
{
    s_bg_on  = 0U;
    s_bg_div = 0U;
}

uint8_t PID_Tracing_BackgroundOn(void)
{
    return s_bg_on;
}

uint8_t PID_Tracing_BackgroundTask(void)
{
    if (!s_bg_on) return 0U;

    if (++s_bg_div < PID_TRACE_BG_DIV) return 0U;
    s_bg_div = 0U;

    motor_tracing_pid_0_task();
    return 1U;
}

