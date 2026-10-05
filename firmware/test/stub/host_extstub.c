/**
 * @file  host_extstub.c
 * @brief 上位机扩展层的测试替身：给 t_hostlink 用
 *
 * t_hostlink 测的是**协议层**（host_link.c）：它只要求"收到哪种帧 → 调用了哪层
 * 的哪个函数"。真正的业务逻辑在 host_ext.c 里，由 t_hostext 用另一套替身测
 * （test/stub/host_appstub.c）。两边互不链接，所以这里只需要一个可观测的假实现。
 */
#include <string.h>

#include "host_ext.h"
#include "motion.h"
#include "nvram.h"
#include "servo.h"
#include "trace_cal.h"

int      stubx_drive_set_calls;
int      stubx_trace_lost;          /* HostExt_TraceLost() 的返回值 */
int      stubx_cal_held;            /* trace_cal_held() 的返回值 */
uint8_t  stubx_last_mode;
int      stubx_last_a, stubx_last_b;
int      stubx_drive_set_rc;        /* 让测试模拟"任务在跑"→ -2 */
int      stubx_stop_calls;
int      stubx_keepalive_calls;
int      stubx_task_calls;
int      stubx_save_calls;
int      stubx_center_calls;
int      stubx_play_n;
uint8_t  stubx_cur_action;
uint8_t  stubx_nvram_state;
int      stubx_action_set_calls;
int      stubx_servo_set[3];
int      stubx_servo_dev[3];        /* TELEM2 里的两路舵机中位偏差（µs） */
int      stubx_drive_mode;
int      stubx_drive_enabled;
int      stubx_wd_trips;
int      stubx_clear_faults;
int      stubx_action_count = SERVO_ACTION_N;
static uint8_t s_pose[SERVO_ACTION_N][2] = {
    {  0,   0}, { 60, 120}, {135, 170}, { 90, 100}, {  0,   0},
    {135, 170}, { 80, 170}, {  0,   0}, { 70,  10}, {100,  80},
    { 45, 135}, {  0,   0}, { 90, 100}, { 60, 120},
};

void HostExt_Init(void) { }
void HostExt_Task(uint32_t now_ms) { (void)now_ms; stubx_task_calls++; }
void HostExt_DriveKeepAlive(uint32_t now_ms) { (void)now_ms; stubx_keepalive_calls++; }

int HostExt_DriveSet(uint8_t mode, int a, int b)
{
    stubx_drive_set_calls++;
    stubx_last_mode = mode;
    stubx_last_a = a;
    stubx_last_b = b;
    if (stubx_drive_set_rc != 0) return stubx_drive_set_rc;
    stubx_drive_mode = (int)mode;
    stubx_drive_enabled = (mode != HOST_DRIVE_OFF) ? 1 : 0;
    return 0;
}
void    HostExt_DriveStop(void) { stubx_stop_calls++; stubx_drive_mode = 0; stubx_drive_enabled = 0; }
uint8_t HostExt_DriveMode(void) { return (uint8_t)stubx_drive_mode; }
uint8_t HostExt_DriveEnabled(void) { return (uint8_t)stubx_drive_enabled; }
int     HostExt_DriveGet(int *a, int *b) { if (a) *a = stubx_last_a; if (b) *b = stubx_last_b; return stubx_drive_enabled; }
uint8_t HostExt_DriveWatchdogTrips(void) { return (uint8_t)stubx_wd_trips; }

int HostExt_ActionSet(int n, int a1, int a2)
{
    stubx_action_set_calls++;
    if (n < 1 || n > SERVO_ACTION_N) return 0;
    s_pose[n - 1][0] = (uint8_t)a1;
    s_pose[n - 1][1] = (uint8_t)a2;
    return 1;
}
int HostExt_ActionGet(int n, int *a1, int *a2)
{
    if (n < 1 || n > SERVO_ACTION_N) return 0;
    if (a1) *a1 = (int)s_pose[n - 1][0];
    if (a2) *a2 = (int)s_pose[n - 1][1];
    return 1;
}
const char *HostExt_ActionName(int n) { return (n >= 1 && n <= SERVO_ACTION_N) ? "动作" : ""; }
int HostExt_ActionPlay(int n)
{
    if (n < 1 || n > SERVO_ACTION_N) return 0;
    stubx_play_n = n;
    stubx_cur_action = (uint8_t)n;
    return 1;
}
int     HostExt_ActionSave(void) { stubx_save_calls++; return 1; }
uint8_t HostExt_CurrentAction(void) { return stubx_cur_action; }

int HostExt_ServoSet(int ch, int deg)
{
    if (ch < 1 || ch > 2) return 0;
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    stubx_servo_set[ch] = deg;
    return 1;
}
int   HostExt_ServoCenter(void) { stubx_center_calls++; return 1; }
int   HostExt_ServoDeviation(int ch, int dev_us) { (void)dev_us; return (ch == 1 || ch == 2) ? 1 : 0; }
float HostExt_ServoAngle(int ch) { return (ch == 1) ? 90.0f : 45.0f; }

int     HostExt_SaveParams(void) { stubx_save_calls++; return 1; }
uint8_t HostExt_NvramState(void) { return stubx_nvram_state; }
uint8_t HostExt_MotorEnabled(void) { return 0u; }
uint8_t HostExt_TaskRunning(void) { return 0u; }   /* TELEM2 51 现在直接问 TaskRun_IsRunning() */
uint8_t HostExt_MotionMode(void) { return 3u; }
uint8_t HostExt_MotionFault(void) { return 0u; }

/* host_link.c 还会直接调下面这几个（协议层自己的需求） */
uint8_t nvram_state(void) { return stubx_nvram_state; }
void    Motion_ClearFault(void) { stubx_clear_faults++; }
int     Servo_ActionCount(void) { return stubx_action_count; }
/* TELEM2 的 60/62 偏移要读两路中位偏差（host_link.c:472-473） */
int     Servo_GetDeviation(int x) { return (x >= 1 && x <= 2) ? stubx_servo_dev[x] : 0; }
void    trace_cal_get_data(trace_cal_data_t *d)
{
    int i;
    memset(d, 0, sizeof(*d));
    d->valid = 1u;
    for (i = 0; i < 8; i++) {
        d->pol[i] = (uint8_t)(i + 1u);
        d->blk[i] = (uint16_t)(100 + i);
        d->wht[i] = (uint16_t)(200 + i);
        d->thr[i] = (uint16_t)(300 + i);
        d->jit[i] = (uint16_t)i;
    }
}

void  HostExt_GrayLevel(uint8_t out[8])
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(i * 10u + 5u);
}
float HostExt_TracePos(void) { return 4.25f; }
float HostExt_TracePid(void) { return -123.0f; }
float HostExt_AngleErr(void) { return 1.25f; }
float HostExt_AngleRateRef(void) { return 2.5f; }
float HostExt_AngleCorr(void) { return -3.75f; }
int   HostExt_TraceCross(void) { return 1; }
int   HostExt_TraceLost(void)  { return stubx_trace_lost; }
uint8_t trace_cal_held(void)   { return stubx_cal_held; }

/* 遥测 v_l/v_r 现在上报真实轮速（host_link.c 的 put_telemetry_base），
   协议层测试不需要真的速度环，给一对可注入的固定值（默认 0 = 静止）。
   t_hostlink 里改这两个量就能断言 offset 10/12 真的带出去了。 */
int stubx_mmps_l = 0;
int stubx_mmps_r = 0;
int SpeedControl_SpeedMmps(int wheel) { return (wheel == 0) ? stubx_mmps_l : stubx_mmps_r; }

/* 母线低压保护：host_link.c 每真读到一次母线电压就报一次（用户 m29239）。
   真逻辑在 host_ext.c（由 t_hostext 测），协议层只需要一个能看见的假实现。 */
uint16_t stubx_vbus_mv;
void    HostExt_NoteVbusMv(uint16_t mv) { stubx_vbus_mv = mv; }
uint8_t HostExt_LowVoltageDisarms(void) { return 0u; }
