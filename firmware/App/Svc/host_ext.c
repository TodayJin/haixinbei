/**
 * @file  host_ext.c
 * @brief 上位机扩展功能（驾驶 / 动作表 / 舵机 / 固化）实现
 *
 * 见 host_ext.h 的分工说明。这里只做"把请求翻译成对控制模块的调用"，
 * 不自己实现任何控制律 —— 所以本文件没有任何 PID/滤波代码，只有状态机。
 *
 * 安全（用户 m13466 明确要求"确保现在是无人化的，你不能动这些电机舵机"是
 * 台架约束，真车跑起来必须有保护）：
 *   1) 任务解释器在跑（`TaskRun_IsRunning() != 0`）时**拒绝任何驾驶请求**，避免和任务
 *      抢电机；停任务后重新连接即可；
 *   2) 链路看门狗：500ms 没收到任何合法帧就自动停车（`HostExt_Task()`）；
 *   3) 母线低压保护：电池被拔掉（母线读到 ~0.9V 而不是 0）连续 500ms ⇒ 停驾驶会话 +
 *      停任务 + 回 IDLE，于是"再插上电池"也不会自己跑（用户 m29239）；
 *   4) 所有角度/脉宽最终都由 servo.c / speed_control.c 自己夹限幅。
 */
#include "host_ext.h"

#include "gyro_control.h"
#include "jy61p.h"
#include "motion.h"
#include "nvram.h"
#include "PID_tracing.h"
#include "servo.h"
#include "speed_control.h"
#include "task_run.h"      /* 任务解释器在跑时不许抢电机（判据 = TaskRun_IsRunning） */
#include "trace_cal.h"

/* ------------------------------------------------------------------ 状态 */

static uint8_t  s_mode;             /* HOST_DRIVE_* */
static int      s_a, s_b;           /* 最近一次驾驶指令的原样参数（遥测/回报用） */
static float    s_base_yaw;         /* 进 HOST_DRIVE_BASE 时的航向（b 是相对它的偏转） */
static float    s_base_yaw_cont;    /* 同上，但**连续域**（不取模）——见 HostExt_DriveSet 的 BASE 分支 */
static uint32_t s_base_leave_ms;    /* 离开 HOST_DRIVE_BASE 的时刻（短暂掉帧要沿用旧基准，见 BASE 分支） */
static uint32_t s_base_frame_ms;    /* ★2026-10-16：上一帧 BASE 到达的时刻（0 = 还没有过）——见 BASE 分支 */
static uint8_t  s_base_first = 1U;  /* 上电后第一次进 BASE 必须重记基准 */
static uint32_t s_keepalive_ms;     /* 最近一次收到合法帧的时刻 */
static uint8_t  s_trips;            /* 看门狗自动停车的次数（诊断） */
static uint8_t  s_cur_action;       /* 最近一次下发的动作号（0 = 没下发过） */

/* 母线低压保护的状态（见 host_ext.h 的说明） */
static uint16_t s_vbus_mv;          /* 最近一次真读到的母线电压 mV（host_link 报进来） */
static uint32_t s_vbus_ms;          /* 上面那个读数的时刻（0 = 从没读到过） */
static uint32_t s_vbus_low_ms;      /* 本轮连续低压的起点（0 = 还没开始算） */
static uint8_t  s_vbus_disarmed;    /* 本轮低压已经停过机了（不重复动手/计数） */
static uint8_t  s_vbus_disarms;     /* 低压自动停机的次数（诊断） */

/* ★2026-10-15：**谁把驾驶会话停掉的** —— 各来源分别计数。
 * 起因：逐帧实测 `pwm==0` 的帧占 39%，其中 `host_drive=0`（会话被清）占 89%，
 * 但按代码看几条路径都不该触发（trips=0、vbus 最低 10.6V 远高于 3V 阈值）
 * ⇒ 不靠读代码猜，让固件自己报是哪一条。
 *   [0] 低压保护  [1] 驾驶看门狗  [2] 显式 OFF  [3] CMD_STOP（暂未接，host_link 侧留待下轮）
 * 烧录后可用 SWD 读 g_stop_src，或看 host_link 每秒发的文本。 */
volatile uint32_t g_stop_src[4] = {0U, 0U, 0U, 0U};

/* ★2026-10-18：HOLD 模式（绝对目标航向）的状态。
 * 见 host_ext.h 里 HOST_DRIVE_HOLD 的说明。这里刻意**不保存任何"基准航向"** ——
 * 那正是 BASE 分支重抓累加的根源。固件只记"目标是多少"，上位机负责算目标。 */
static float    s_hold_target;      /* 锁存的绝对目标航向 °，(-180,180] */
static uint8_t  s_hold_valid;       /* 1 = 目标有效 */
static int      s_hold_base;        /* 最近一次下发的基准速度刻度（只用于诊断） */
static uint32_t s_hold_dead_ms;     /* 角度环"不在了"的起始时刻（0 = 环正常） */
static uint8_t  s_hold_reasserts;   /* 目标被别处改掉又补回来的次数（健康时应恒为 0） */

/* 把角度归到 (-180, 180]（本文件自用，避免为一个取模去动 gyro_control 的内部函数） */
static float hold_norm180(float a)
{
    while (a > 180.0f)  a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

static void hold_clear(void)
{
    s_hold_valid   = 0U;
    s_hold_target  = 0.0f;
    s_hold_base    = 0;
    s_hold_dead_ms = 0U;
}

/* 起环 + 写目标。顺序不能反：从别的模式切进来时 GyroControl_Start() 会把目标重置成当前航向。 */
static void hold_push(int base, float target)
{
    Motion_SetSpeed(base);
    Motion_SetMode(MOTION_GYRO);              /* 同模式幂等：不重开环、不重置目标 */
    GyroControl_SetTargetAngle(target);       /* 绝对值：重复写不累加 */
    s_hold_target  = hold_norm180(target);
    s_hold_base    = base;
    s_hold_valid   = 1U;
    s_hold_dead_ms = 0U;
}

void HostExt_Init(void)
{
    s_mode          = HOST_DRIVE_OFF;
    s_a             = 0;
    s_b             = 0;
    s_base_yaw      = 0.0f;
    s_base_yaw_cont = 0.0f;
    s_base_leave_ms = 0U;
    s_base_frame_ms = 0U;
    s_base_first    = 1U;
    s_keepalive_ms  = HAL_GetTick();
    s_trips         = 0U;
    s_cur_action    = 0U;
    s_vbus_mv       = 0U;
    s_vbus_ms       = 0U;
    s_vbus_low_ms   = 0U;
    s_vbus_disarmed = 0U;
    s_vbus_disarms  = 0U;
    hold_clear();
    s_hold_reasserts = 0U;
}

/* ------------------------------------------------------------------ 驾驶 */

uint8_t HostExt_DriveMode(void)
{
    return s_mode;
}

uint8_t HostExt_DriveEnabled(void)
{
    return (s_mode != HOST_DRIVE_OFF) ? 1U : 0U;
}

int HostExt_DriveGet(int *a, int *b)
{
    if (a != NULL) *a = s_a;
    if (b != NULL) *b = s_b;
    return (s_mode != HOST_DRIVE_OFF) ? 1 : 0;
}

uint8_t HostExt_DriveWatchdogTrips(void)
{
    return s_trips;
}

/* ★2026-10-18：HOLD 状态 —— 0 = 没在保持，1 = 正在转，2 = 已到位锁存。
 * "到位"沿用角度环自己的锁存（gyro_control.c 的 s_settled：|err|<settle_deg 且 |v|<settle_rate
 * 持续 settle_ms ⇒ corr=0），这里按 |误差| 近似判断，够界面显示用。 */
uint8_t HostExt_HoldState(void)
{
    if (s_mode != HOST_DRIVE_HOLD || s_hold_valid == 0U) {
        return 0U;
    }
    if (GyroControl_IsEnabled() == 0U) {
        return 0U;
    }
    {
        float e = GyroControl_HeadingError();
        if (e < 0.0f) e = -e;
        return (e <= 1.5f) ? 2U : 1U;
    }
}

void HostExt_DriveStop(void)
{
    /* 用户 m29239：WHEELS 直给模式下 Motion 一直停在 IDLE，而 motion.c 的
     * `Motion_SetMode()` 对同模式是早退 ⇒ 原来这里只调 Motion_SetMode(MOTION_IDLE)
     * 等于什么都没做，速度环照旧使能、车照旧跑（"停车命令停不下来，只能拔电池"）。
     * 所以显式停速度环（它里面会 Load(0,0)），再让 Motion 回 IDLE。 */
    SpeedControl_Stop();
    Motion_SetMode(MOTION_IDLE);
    s_mode = HOST_DRIVE_OFF;
    s_a    = 0;
    s_b    = 0;
    hold_clear();          /* ★2026-10-18：真停车才清 HOLD 目标；松手不清（松手对固件不可见） */
}

/* ------------------------------------------------ 母线低压保护（用户 m29239） */

void HostExt_NoteVbusMv(uint16_t mv)
{
    s_vbus_mv = mv;
    s_vbus_ms = HAL_GetTick();
}

uint8_t HostExt_LowVoltageDisarms(void)
{
    return s_vbus_disarms;
}

/* 电池被拔掉 ⇒ 把"会驱动电机的东西"全关掉。
 *
 * 为什么需要它：他停车的手法是拔电机电池，而拔电池并不让 MCU 掉电（MCU/上位机/J-Link
 * 走 USB，INA226 还在，母线实测 0.9V）。速度环的使能是**跨电池断电存活**的：拔了电池
 * 车只是没力，一合上电池车立刻按残留目标全速跑 —— 就是"一上电他就跑，得拔电才停"。
 * 这里连续 HOST_VBUS_LOW_MS 读到低压就 ① 停驾驶会话 ② 停任务解释器 ③ 回 MOTION_IDLE，
 * 三条一起保证"电池再插回来也不会自己跑"。电压回到 HOST_VBUS_OK_MV 以上只复位监测。
 */
static void low_voltage_guard(uint32_t now)
{
    if (s_vbus_ms == 0U) return;                                    /* 从没读到过 */
    if ((uint32_t)(now - s_vbus_ms) > HOST_VBUS_STALE_MS) return;    /* 读数过期：宁可不动手 */
    if (s_vbus_mv == 0U) return;                                    /* 读失败（host_link 记 0） */

    if (s_vbus_mv >= HOST_VBUS_OK_MV) {          /* 电池在（且不虚） */
        s_vbus_low_ms   = 0U;
        s_vbus_disarmed = 0U;
        return;
    }
    if (s_vbus_mv >= HOST_VBUS_LOW_MV) {         /* 3~9V：不健康，但不算"没接" */
        s_vbus_low_ms = 0U;
        return;
    }
    if (s_vbus_disarmed) return;                 /* 这一轮已经动过手 */

    if (s_vbus_low_ms == 0U) {                   /* 第一次读到低压：开始计时 */
        s_vbus_low_ms = (now != 0U) ? now : 1U;
        return;
    }
    if ((uint32_t)(now - s_vbus_low_ms) < HOST_VBUS_LOW_MS) return;

    s_vbus_disarmed = 1U;
    s_vbus_disarms++;
    TaskRun_Stop();                              /* 任务先停：不然它下一块 SET_MODE 又把环开起来 */
    g_stop_src[0]++;                             /* ★诊断：低压保护停的 */
    HostExt_DriveStop();                         /* 会话清掉 + SpeedControl_Stop() */
    Motion_SetMode(MOTION_IDLE);                 /* 兜底：非 IDLE 模式也真转换回 IDLE */
}

void HostExt_DriveKeepAlive(uint32_t now_ms)
{
    s_keepalive_ms = now_ms;
}

void HostExt_Task(uint32_t now_ms)
{
    low_voltage_guard(now_ms);      /* 跟驾驶会话无关：没在驾驶时也要盯着电池 */

    /* ★2026-10-18 HOLD 模式的存活检查（必须在 500ms 看门狗之前）：
     *  HOLD 靠上位机继续发帧 + 桥的 200ms 保活维持，所以这里只关心环还在不在。
     *  ① 任务解释器抢电机 ⇒ 主动让出（不然两边打架）
     *  ② 环没了（陀螺失联保护 / 别人 Stop / Motion 掉出 GYRO）⇒ 失效即退出，**绝不自己恢复**
     *     否则陀螺失联保护停完车，HOLD 会每拍把环重新拉起来 ⇒ 反复重启。
     *  连续 HOST_HOLD_DEAD_MS 才判，躲开 TIM6 切换模式时的瞬时读。 */
    if (s_mode == HOST_DRIVE_HOLD) {
        float tgt_now = s_hold_target;
        if (TaskRun_IsRunning() != 0U) {
            g_stop_src[1]++;
            HostExt_DriveStop();
            return;
        }
        if (Motion_GetMode() == MOTION_GYRO && GyroControl_IsEnabled() != 0U &&
            Motion_GetFault() == MOTION_FAULT_NONE) {
            s_hold_dead_ms = 0U;
            /* 目标被别处改掉（任务/循迹/别的模式）就补回来。健康时应恒为 0 次。 */
            if (s_hold_valid != 0U) {
                float d = hold_norm180(GyroControl_GetTargetAngle() - tgt_now);
                if (d > 0.05f || d < -0.05f) {
                    GyroControl_SetTargetAngle(tgt_now);
                    s_hold_reasserts++;
                }
            }
        } else {
            if (s_hold_dead_ms == 0U) {
                s_hold_dead_ms = (now_ms != 0U) ? now_ms : 1U;
            } else if ((uint32_t)(now_ms - s_hold_dead_ms) >= HOST_HOLD_DEAD_MS) {
                g_stop_src[1]++;
                HostExt_DriveStop();
                return;
            }
        }
    }

    if (s_mode == HOST_DRIVE_OFF) {
        return;
    }
    if ((uint32_t)(now_ms - s_keepalive_ms) < HOST_DRIVE_TIMEOUT_MS) {
        return;
    }
    /* 链路断了（线掉了 / 上位机崩了 / 串口拔了）：绝不能把最后一条指令留着。
     * 用户 m13466 的场景是"人在旁边调车"，车自己冲出去是最大的风险。 */
    s_trips++;
    g_stop_src[1]++;                             /* ★诊断：驾驶看门狗停的 */
    HostExt_DriveStop();
}

#define HOST_BASE_REGRAB_MS 1000U /* 离开 BASE 超过这么久才重记基准航向（短暂掉帧沿用旧基准）。
                                        * ★2026-10-15 实测：150ms 仍会被串口偶发空档踩到 ⇒ 车转到 36° 时基准被重捕、
                                        * 目标变成 36+90=126 ⇒ 末值停在 126 不回（与实测数值完全吻合）。
                                        * 提到 1000ms：只有用户真的松手/退出模式才会重记基准。 */

int HostExt_DriveSet(uint8_t mode, int a, int b)
{
    if (TaskRun_IsRunning() != 0U) {
        return -2;                     /* 任务解释器在跑：不抢电机 */
    }

    s_a = a;
    s_b = b;
    HostExt_DriveKeepAlive(HAL_GetTick());

    /* ★2026-10-15：记下"离开 BASE"的时刻。见 BASE 分支的短暂掉帧保护。 */
    if (s_mode == HOST_DRIVE_BASE && mode != HOST_DRIVE_BASE) {
        s_base_leave_ms = HAL_GetTick();
    }

    switch (mode) {
    case HOST_DRIVE_OFF:
        g_stop_src[2]++;                         /* ★诊断：显式 OFF 停的 */
        HostExt_DriveStop();
        return 0;

    case HOST_DRIVE_WHEELS:
        /* 速度环单环：不角度环、不循迹（只有 IDLE 分支会只跑速度环）。
         *
         * ★2026-10-15（实测抓到的真 bug）：这个分支**必须幂等**。
         *  原来每次收到驾驶帧都 `Motion_SetMode(MOTION_IDLE)` —— 而 IDLE 分支内部会
         *  `GyroControl_Stop() + SpeedControl_Stop()` —— 然后才 Enable(1) 重新开环。
         *  可是**喂狗必须每 <HOST_DRIVE_TIMEOUT_MS(500ms) 重发一次驾驶帧**（上位机与我
         *  的测试脚本都是 ~80~200ms 重发）⇒ **速度环每 80ms 被停掉再开一次** ✗。
         *  逐帧实测（trace.py，200Hz）：`pwm==0` 的帧占 39%，其中 `motor(使能)=0` 占 89%；
         *  表现就是"满油门 7200 ↔ 全松 0"的 bang-bang，且实测转速与目标无关
         *  （2200/2500/2800/3000 四个目标的最大转速都是 ~525~645）⇒ 环根本来不及稳。
         *  这也解释了用户"速度快不匹配"：上位机操控页同样在周期性喂狗。
         *  修法与 HOST_DRIVE_BASE 分支一致：**只在模式变化时**停/起，其它时候只更新目标。 */
        if (s_mode != HOST_DRIVE_WHEELS) {
            Motion_SetMode(MOTION_IDLE);          /* 会停掉角度环/速度环（只做一次）*/
            SpeedControl_SetRamp(HOST_DRIVE_RAMP);
            SpeedControl_Enable(1U);
        }
        SpeedControl_SetTarget(a, b);             /* 每帧都更新目标（这样才跟得上）*/
        s_mode = HOST_DRIVE_WHEELS;
        return 0;

    case HOST_DRIVE_HOLD: {
        /* b = 绝对目标航向 × 10。范围检查在 host_link.c 也做了（协议层早退）。 */
        float tgt = (float)b * 0.1f;
        if (!(tgt >= -180.0f && tgt <= 180.0f)) {
            return -1;                 /* 同时挡住 NaN */
        }
        tgt = hold_norm180(tgt);
        {
            float d = hold_norm180(tgt - s_hold_target);
            /* ★只在 ①首帧 ②目标真变了 ③环被别处停了 时才写目标。
             * 每帧无条件写会清积分和到位锁存 ⇒ 到点后微抖（见 host_ext.h 的 HOST_HOLD_EPS_DEG）。 */
            if (s_mode != HOST_DRIVE_HOLD || s_hold_valid == 0U ||
                d > HOST_HOLD_EPS_DEG || d < -HOST_HOLD_EPS_DEG ||
                Motion_GetMode() != MOTION_GYRO || GyroControl_IsEnabled() == 0U) {
                hold_push(a, tgt);
            } else {
                Motion_SetSpeed(a);        /* 只跟基准速度（纯赋值，不碰目标） */
                s_hold_base = a;
            }
        }
        s_mode = HOST_DRIVE_HOLD;
        return 0;
    }

    case HOST_DRIVE_BASE:
        Motion_SetSpeed(a);
        Motion_SetMode(MOTION_GYRO);   /* 角度环 + 速度环（两种形态里的"障碍流程"） */
        /* ★2026-10-16 根治"阶梯式转不停"（用户："在 IMU 页看它一直震荡"）：
         * 长测逐帧证据 —— 目标设 90°，车先完美停在 90.2°（峰峰 0.1°），4 秒后自己又转了 +88.4° 停在 178.4°，
         * 依此类推 90→178→268…（台阶式转个不停）。唯一可能是 `s_base_yaw_cont` 被重抓成了当前车头。
         * 原判据是 `s_mode != HOST_DRIVE_BASE`（只要中途被别处改成 IDLE/别的模式，下一帧就重抓基准），
         * 于是"会话被打断一次"⇒ 目标 = 当前车头 + b ⇒ **已转过的角度被重复计入** ⇒ 一圈圈转下去。
         * 新判据只看"距上一帧 BASE 有多久"：**只要上位机还在连续发 BASE 帧，就绝不重记基准** ✓
         * （b 是相对"最近一次真正重新进入 BASE"那一刻的车头，语义更符合上位机"设一个绝对朝向"的用法。） */
        if (s_base_frame_ms == 0U ||
            (uint32_t)(HAL_GetTick() - s_base_frame_ms) > HOST_BASE_REGRAB_MS) {
            /* ★2026-10-15（用户实测"转过去超过目标很多再回来"，逐帧抓到真因）：
             *  —— **短暂掉帧不许重捕基准** ——
             * 原来只要离开 BASE 一拍就重记基准 ⇒ 只要上位机发帧慢了一拍（掉帧/刷新空档/
             * 看门狗边界），下一帧就把"当前车头"当成新基准 ⇒ 目标 = 当前车头 + b，
             * **已经转过的角度被重复计入目标** ⇒ 车多转"已转过的角度" ⇒ 必然大幅超调再回来。
             * 逐帧证据（IMU 页设 90°，30/80ms 发帧）：环内航向比真实航向少 48°，
             * 且差值恒定 48.0±0.5°（从重捕那一刻起）⇒ 峰值转到 +151°，超调 +61°。
             * 现在：只有**离开 BASE 超过 HOST_BASE_REGRAB_MS(150ms)** 才重记基准；短暂掉帧沿用旧基准。 */
            if (s_base_first != 0U ||
                (uint32_t)(HAL_GetTick() - s_base_leave_ms) > HOST_BASE_REGRAB_MS) {
                s_base_yaw = Yaw;                              /* 进模式那一刻的航向（遥测/诊断用） */
                s_base_yaw_cont = GyroControl_GetYawCont();    /* 连续域基准（未取模） */
                s_base_first = 0U;
            }
        }
        s_base_frame_ms = HAL_GetTick();   /* ★记下本帧时刻：下一帧若在 1s 内到达就沿用同一基准 */
        /* ★2026-10-07：b 是上位机的**累计转角**（可到 ±540°），必须走连续域目标。
         * 走 `GyroControl_SetTargetAngle(NormalizeAngle(...))` 时，只要累计目标领先车头
         * 超过 180°，角度环的 `CalculateAngleError`（±180 取模 + 最近路）就会把"继续往左"
         * 解释成"往右 160°" ⇒ 车突然倒拐、来回拐。真车实测翻号点：183.4/201.5/203.9/209.0°，
         * 与"设定 180°/s 但车只能转 ~80°/s ⇒ 领先一路涨过 180°"完全对应。
         * 连续域模式下目标与航向都不取模，误差 = 还差多少度，方向永远正确。 */
        GyroControl_SetTargetAngleCont(s_base_yaw_cont + (float)b);
        s_mode = HOST_DRIVE_BASE;
        return 0;

    case HOST_DRIVE_TRACE2:
        Motion_SetSpeed(a);
        Motion_SetMode(MOTION_TRACE);        /* ★直驱差速循迹（老思路）：误差直接改左右轮，不经角度环 */
        s_mode = HOST_DRIVE_TRACE2;
        return 0;

    case HOST_DRIVE_TRACE:
        Motion_SetSpeed(a);
        Motion_SetMode(MOTION_TRACE_GYRO);   /* 三级串级：循迹 → 角度 → 速度 */
        s_mode = HOST_DRIVE_TRACE;
        return 0;

    default:
        return -1;
    }
}

/* -------------------------------------------------------------- 动作/舵机 */

int HostExt_ActionSet(int n, int a1, int a2)
{
    return Servo_SetActionPose(n, a1, a2);
}

int HostExt_ActionGet(int n, int *a1, int *a2)
{
    if (n < 1 || n > Servo_ActionCount()) {
        return 0;
    }
    Servo_ActionPose(n, a1, a2);
    return 1;
}

const char *HostExt_ActionName(int n)
{
    return Servo_ActionName(n);
}

int HostExt_ActionPlay(int n)
{
    if (n < 1 || n > Servo_ActionCount()) {
        return 0;
    }
    /* 只下发终点姿态：非阻塞（5/9 号的中间延时步骤只在车上自己跑比赛流程时用），
     * 否则会把主循环卡住近 1s，遥测/按键全停。 */
    Servo_ApplyAction(n);
    s_cur_action = (uint8_t)n;
    return 1;
}

uint8_t HostExt_CurrentAction(void)
{
    return s_cur_action;
}

int HostExt_ActionSave(void)
{
    return HostExt_SaveParams();
}

int HostExt_ServoSet(int ch, int deg)
{
    if (ch < 1 || ch > 2) {
        return 0;
    }
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    Servo_Control(ch, (uint8_t)deg);
    return 1;
}

int HostExt_ServoCenter(void)
{
    Servo_Control(1, 0U);
    Servo_Control(2, 0U);
    return 1;
}

int HostExt_ServoDeviation(int ch, int dev_us)
{
    if (ch < 1 || ch > 2) {
        return 0;
    }
    Servo_SetDeviation(ch, dev_us);        /* 内部夹到 ±100µs */
    /* 立刻按新偏差重发一次当前角度，否则要等下一次动角度才生效 */
    Servo_SetAngle(ch, Servo_GetAngle(ch));
    return 1;
}

float HostExt_ServoAngle(int ch)
{
    return Servo_GetAngle(ch);
}

/* ------------------------------------------------------------------ 固化 */

int HostExt_SaveParams(void)
{
    nvram_touch();
    return 1;
}

uint8_t HostExt_NvramState(void)
{
    return nvram_state();
}

uint8_t HostExt_MotorEnabled(void)
{
    return SpeedControl_IsEnabled();
}

uint8_t HostExt_TaskRunning(void)
{
    return (TaskRun_IsRunning() != 0U) ? 1U : 0U;
}

uint8_t HostExt_MotionMode(void)
{
    return (uint8_t)Motion_GetMode();
}

uint8_t HostExt_MotionFault(void)
{
    return Motion_GetFault();
}

/* ------------------------------------------------------------- 遥测只读量 */

void HostExt_GrayLevel(uint8_t out[8])
{
    uint8_t i;

    for (i = 0U; i < 8U; i++) {
        out[i] = trace_cal_level(i);
    }
}

float HostExt_TracePos(void)      { return PID_Tracing_TracePos(); }
float HostExt_TracePid(void)      { return PID_Tracing_PidOutput(); }
/* ★2026-10-18：环没使能时一律报 0。
 * 起因（子 Agent 审计）：`gyro_control.c` 的 target_angle 上电初值是 0.0f，而且**从来不会被
 * 初始化成当前航向**（只有 GyroControl_Start() 才设），而 HeadingError() 里没有
 * "未使能 ⇒ 0" 的门 ⇒ 上电后只要没进过角度环模式，遥测就是 err = 0 − 当前航向
 * （车头 137° 就显示 −137.5°）⇒ 用户以为是"一进 IMU 页就有误差"。
 * 同理 s_dbg_rate_ref / s_dbg_corr 只在使能时更新、Stop 也不清 ⇒ 未接管时冻结在上次的值。
 * 这里只影响遥测（host_link.c 是唯一消费点），**不碰任何控制律**。 */
float HostExt_AngleErr(void)
{
    return (GyroControl_IsEnabled() != 0U) ? GyroControl_HeadingError() : 0.0f;
}
float HostExt_AngleRateRef(void)
{
    return (GyroControl_IsEnabled() != 0U) ? GyroControl_DebugRateRef() : 0.0f;
}
float HostExt_AngleCorr(void)
{
    return (GyroControl_IsEnabled() != 0U) ? GyroControl_DebugCorr() : 0.0f;
}
int   HostExt_TraceCross(void)    { return (tracing_cross() != 0U) ? 1 : 0; }
int   HostExt_TraceLost(void)     { return (tracing_lost() != 0U) ? 1 : 0; }
