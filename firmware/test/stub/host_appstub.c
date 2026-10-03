/**
 * @file  host_appstub.c
 * @brief 控制模块的测试替身：给 t_hostext 用（host_ext.c 的全部依赖）
 *
 * host_ext.c 是"上位机请求 → 控制模块调用"的胶水层，它自己不含控制律，
 * 所以真正的逻辑（模式切换顺序、任务在跑就拒绝、链路断了要停车）可以用一个
 * **纯记录式的替身**把调用序列钉死；实机行为由 make + 上板验证负责。
 *
 * 观测变量（extern 在 t_hostext.c 里）命名一律 stub_ 开头。
 */
#include <math.h>
#include <string.h>

#include "gyro_control.h"
#include "motion.h"
#include "nvram.h"
#include "servo.h"
#include "speed_control.h"
#include "trace_cal.h"
#include "PID_tracing.h"

/* ---- 观测变量 ---- */
int      stub_motion_mode;          /* 最近一次 Motion_SetMode 的模式 */
int      stub_motion_speed;         /* 最近一次 Motion_SetSpeed */
int      stub_clear_faults;
int      stub_sc_enabled;
int      stub_sc_l, stub_sc_r;
float    stub_sc_ramp;
int      stub_sc_stops;
int      stub_gyro_started, stub_gyro_stopped;
float    stub_gyro_target;
int      stub_servo_apply_n;        /* 最近一次 Servo_ApplyAction 的动作号 */
int      stub_servo_ctl[3];         /* Servo_Control 收到的角度 */
int      stub_servo_dev[3];
float    stub_servo_angle[3];
int      stub_nvram_touch;
uint8_t  stub_nvram_state;

float Yaw, Pitch, Roll;
/* `-DHOST_PIPE_REAL_SERVO`（firmware/test/pipe_hostlink.c 的构建）会把 servo 这一段
 * 换成真的 firmware/App/Bsp/servo.c —— 那时这些假表和假函数都不能有，否则符号重复。
 * 顺带说明：下面 k_def[] 是**旧台架值**，与 servo.c 的 k_action_default[] 不同 ——
 * 所以 t_hostext 用这套桩时测的是桩自己的表，不是出厂表。 */
#ifndef HOST_PIPE_REAL_SERVO
static uint8_t s_pose[SERVO_ACTION_N][2];
static int     s_pose_ready;

static const uint8_t k_def[SERVO_ACTION_N][2] = {
    {  0,   0}, { 60, 120}, {135, 170}, { 90, 100}, {  0,   0},
    {135, 170}, { 80, 170}, {  0,   0}, { 70,  10}, {100,  80},
    { 45, 135}, {  0,   0}, { 90, 100}, { 60, 120},
};

static void pose_ready(void)
{
    if (!s_pose_ready) {
        memcpy(s_pose, k_def, sizeof(s_pose));
        s_pose_ready = 1;
    }
}
#endif /* !HOST_PIPE_REAL_SERVO */

/* ---- motion ---- */
void Motion_SetMode(Motion_Mode m)
{
    stub_motion_mode = (int)m;
    if (m == MOTION_IDLE) {
        SpeedControl_Stop();
    }
}
void Motion_SetSpeed(int s) { stub_motion_speed = s; }
int  Motion_GetSpeed(void) { return stub_motion_speed; }
Motion_Mode Motion_GetMode(void) { return (Motion_Mode)stub_motion_mode; }
uint8_t Motion_GetFault(void) { return 0u; }
void Motion_ClearFault(void) { stub_clear_faults++; }

/* ---- speed_control ---- */
void    SpeedControl_Enable(uint8_t en) { stub_sc_enabled = (int)en; }
uint8_t SpeedControl_IsEnabled(void) { return (uint8_t)stub_sc_enabled; }
void    SpeedControl_SetTarget(int l, int r) { stub_sc_l = l; stub_sc_r = r; }
void    SpeedControl_SetRamp(float r) { stub_sc_ramp = r; }
void    SpeedControl_Stop(void) { stub_sc_stops++; stub_sc_enabled = 0; }

/* ---- gyro_control ---- */
void    GyroControl_Start(void) { stub_gyro_started++; }
void    GyroControl_Stop(void) { stub_gyro_stopped++; }
uint8_t GyroControl_IsEnabled(void) { return 0u; }
void    GyroControl_SetTargetAngle(float deg) { stub_gyro_target = NormalizeAngle(deg); }
float   GyroControl_GetTargetAngle(void) { return stub_gyro_target; }
float   GyroControl_HeadingError(void) { return 1.25f; }
float   GyroControl_DebugRateRef(void) { return 2.5f; }
float   GyroControl_DebugCorr(void) { return -3.75f; }
float   NormalizeAngle(float a)
{
    while (a > 180.0f)  a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/* ---- servo ----
 * 带 `-DHOST_PIPE_REAL_SERVO` 编译时（firmware/test/pipe_hostlink.c）这一整段换成真的
 * firmware/App/Bsp/servo.c：动作表 const→RAM、中位偏差、角度→脉宽、越界夹取都走真代码。 */
#ifndef HOST_PIPE_REAL_SERVO
int Servo_ActionCount(void) { return SERVO_ACTION_N; }
int Servo_SetActionPose(int n, int a1, int a2)
{
    pose_ready();
    if (n < 1 || n > SERVO_ACTION_N) return 0;
    if (a1 < 0) a1 = 0;
    if (a1 > 180) a1 = 180;
    if (a2 < 0) a2 = 0;
    if (a2 > 180) a2 = 180;
    s_pose[n - 1][0] = (uint8_t)a1;
    s_pose[n - 1][1] = (uint8_t)a2;
    return 1;
}
void Servo_ActionPose(int n, int *a1, int *a2)
{
    pose_ready();
    if (n < 1 || n > SERVO_ACTION_N) {
        if (a1) *a1 = 0;
        if (a2) *a2 = 0;
        return;
    }
    if (a1) *a1 = (int)s_pose[n - 1][0];
    if (a2) *a2 = (int)s_pose[n - 1][1];
}
void Servo_ApplyAction(int n) { stub_servo_apply_n = n; }
const char *Servo_ActionName(int n)
{
    static const char *names[SERVO_ACTION_N] = {
        "收回", "打下准备", "打下1", "打下2", "台阶2收回", "大坡1", "三级12",
        "三级2收回", "三级3打", "高台", "高台下", "收回2", "打下1(另一路)", "打下准备(另一路)"
    };
    if (n < 1 || n > SERVO_ACTION_N) return "";
    return names[n - 1];
}
void  Servo_Control(int x, uint8_t angle)
{
    if (x < 1 || x > 2) return;
    stub_servo_ctl[x] = (int)angle;
    stub_servo_angle[x] = (float)angle;
}
void  Servo_SetDeviation(int x, int d)
{
    if (x < 1 || x > 2) return;
    stub_servo_dev[x] = d;
}
void  Servo_SetAngle(int x, float a)
{
    if (x < 1 || x > 2) return;
    stub_servo_angle[x] = a;
}
float Servo_GetAngle(int x) { return (x >= 1 && x <= 2) ? stub_servo_angle[x] : 0.0f; }
#endif /* !HOST_PIPE_REAL_SERVO */

/* ---- nvram ---- */
void    nvram_touch(void) { stub_nvram_touch++; }
uint8_t nvram_state(void) { return stub_nvram_state; }

/* ---- trace_cal / PID_tracing ---- */
uint8_t trace_cal_level(uint8_t idx) { return (uint8_t)(idx * 10u + 5u); }
float   PID_Tracing_TracePos(void) { return 4.25f; }
float   PID_Tracing_PidOutput(void) { return -123.0f; }
uint8_t tracing_cross(void) { return 1u; }
