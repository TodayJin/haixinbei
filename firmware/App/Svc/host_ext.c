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
 *   3) 所有角度/脉宽最终都由 servo.c / speed_control.c 自己夹限幅。
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
static uint32_t s_keepalive_ms;     /* 最近一次收到合法帧的时刻 */
static uint8_t  s_trips;            /* 看门狗自动停车的次数（诊断） */
static uint8_t  s_cur_action;       /* 最近一次下发的动作号（0 = 没下发过） */

void HostExt_Init(void)
{
    s_mode         = HOST_DRIVE_OFF;
    s_a            = 0;
    s_b            = 0;
    s_base_yaw     = 0.0f;
    s_keepalive_ms = HAL_GetTick();
    s_trips        = 0U;
    s_cur_action   = 0U;
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

void HostExt_DriveStop(void)
{
    /* MOTION_IDLE 里就是 GyroControl_Stop() + SpeedControl_Stop()（后者会 Load(0,0)）*/
    Motion_SetMode(MOTION_IDLE);
    s_mode = HOST_DRIVE_OFF;
    s_a    = 0;
    s_b    = 0;
}

void HostExt_DriveKeepAlive(uint32_t now_ms)
{
    s_keepalive_ms = now_ms;
}

void HostExt_Task(uint32_t now_ms)
{
    if (s_mode == HOST_DRIVE_OFF) {
        return;
    }
    if ((uint32_t)(now_ms - s_keepalive_ms) < HOST_DRIVE_TIMEOUT_MS) {
        return;
    }
    /* 链路断了（线掉了 / 上位机崩了 / 串口拔了）：绝不能把最后一条指令留着。
     * 用户 m13466 的场景是"人在旁边调车"，车自己冲出去是最大的风险。 */
    s_trips++;
    HostExt_DriveStop();
}

int HostExt_DriveSet(uint8_t mode, int a, int b)
{
    if (TaskRun_IsRunning() != 0U) {
        return -2;                     /* 任务解释器在跑：不抢电机 */
    }

    s_a = a;
    s_b = b;
    HostExt_DriveKeepAlive(HAL_GetTick());

    switch (mode) {
    case HOST_DRIVE_OFF:
        HostExt_DriveStop();
        return 0;

    case HOST_DRIVE_WHEELS:
        /* 速度环单环：不角度环、不循迹（只有 IDLE 分支会只跑速度环）。
         * 先切 IDLE（会把速度环停掉、目标清零），再自己设目标使能。 */
        Motion_SetMode(MOTION_IDLE);
        SpeedControl_SetRamp(HOST_DRIVE_RAMP);
        SpeedControl_SetTarget(a, b);
        SpeedControl_Enable(1U);
        s_mode = HOST_DRIVE_WHEELS;
        return 0;

    case HOST_DRIVE_BASE:
        if (s_mode != HOST_DRIVE_BASE) {
            s_base_yaw = Yaw;          /* 进模式那一刻的航向当基准 */
        }
        Motion_SetSpeed(a);
        Motion_SetMode(MOTION_GYRO);   /* 角度环 + 速度环（两种形态里的"障碍流程"） */
        GyroControl_SetTargetAngle(NormalizeAngle(s_base_yaw + (float)b));
        s_mode = HOST_DRIVE_BASE;
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
float HostExt_AngleErr(void)      { return GyroControl_HeadingError(); }
float HostExt_AngleRateRef(void)  { return GyroControl_DebugRateRef(); }
float HostExt_AngleCorr(void)     { return GyroControl_DebugCorr(); }
int   HostExt_TraceCross(void)    { return (tracing_cross() != 0U) ? 1 : 0; }
