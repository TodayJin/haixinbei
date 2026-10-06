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

#include "app_ui.h"        /* app_settings_t g_cfg + AppUi_Beep（task_run.c 的「过点蜂鸣」）*/
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

/* ---- 设置 / 界面（app_ui.c 的替身）的观测点 ----
 * task_run.c 切状态时会 `if (g_cfg.beep_marker) AppUi_Beep(20);`（★2026-10-07
 * 把「过点蜂鸣」这个孤儿开关接回），这几个观测点让主机测试能断言"响了没有/响了几次"、
 * 以及 CAL_SET 块到底切到了哪一套标定。 */
int      stub_beep_n;               /* AppUi_Beep 被调了几次 */
int      stub_beep_ms;              /* 最近一次的蜂鸣时长（ms） */
int      stub_cal_set;              /* 最近一次 trace_cal_select_set() 的套号（0/1） */

/* ---- 任务解释器（t_task.c）要的可变观测点 ----
 * 默认值与原来自成一体的常量完全一致（4.25f / 1.25f / 0 / idx*10+5），
 * 所以既有测试（t_hostext 等）的行为一个字节都没变；t_task.c 直接给它们赋值来造场景。 */
float    stub_trace_pos = 4.25f;
uint8_t  stub_cross     = 1u;
uint8_t  stub_lost      = 0u;
uint8_t  stub_gray[8]   = { 5u, 15u, 25u, 35u, 45u, 55u, 65u, 75u };
float    stub_gyro_err  = 1.25f;
uint8_t  stub_motion_fault;
int      stub_cross_clears;
int      stub_speed_mmps[2];

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
    /* 真 motion.c 对"同模式"是早退，且会顺手把速度环的状态摆正（motion.c:80-99）。
     * 桩必须照做，否则测不出"同模式 IDLE 把停车变成空操作"这类 bug（用户 m29239：
     * WHEELS 直给时 Motion 常驻 IDLE，HostExt_DriveStop() 就成了空调用）。 */
    if (stub_motion_mode == (int)m) {
        return;
    }
    stub_motion_mode = (int)m;
    if (m == MOTION_IDLE) {
        SpeedControl_Stop();
    }
}
void Motion_SetSpeed(int s) { stub_motion_speed = s; }
int  Motion_GetSpeed(void) { return stub_motion_speed; }
Motion_Mode Motion_GetMode(void) { return (Motion_Mode)stub_motion_mode; }
uint8_t Motion_GetFault(void) { return stub_motion_fault; }
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
float   GyroControl_HeadingError(void) { return stub_gyro_err; }
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

/* ---- app_ui（设置表 g_cfg + 蜂鸣接口）----
 * task_run.c 现在要读 `g_cfg.beep_marker`（「过点蜂鸣」）并在切状态时调 AppUi_Beep()，
 * 但 app_ui.c（KK_UI 那一整套界面）从来不在主机测试的源码列表里 ⇒ 这两样得在这里补。
 *
 * 带 `-DHOST_PIPE_REAL_NVRAM` 时（firmware/test/pipe_hostlink.c 的构建）**g_cfg 由
 * host_pipestub.c 定义** —— 那边链的是真 nvram.c，g_cfg 是它 snapshot/apply 的落点，
 * 必须只有一份 ⇒ 这里让位（与下面 trace_cal_level / trace_cal_channel 同一个套路，
 * 否则 `multiple definition of 'g_cfg'`）。AppUi_Beep 没有这个顾虑：全工程只有 app_ui.c
 * 定义它，而 app_ui.c 不在任何主机测试里。 */
#ifndef HOST_PIPE_REAL_NVRAM

/* 逐项照抄 App/Src/app_ui.c 的 g_cfg 初值（2026-10-07 对齐）。
 * 为什么要一致而不是留 0：主机测试要的是"与真机同样的默认开/关" —— 比如
 * `beep_marker` 真机默认 **true**，桩里若留 false，「过点蜂鸣」那条分支在测试里
 * 永远走不到，接回开关的回归就没网了。改 app_ui.c 的默认值要连这里一起改。
 * （主机侧目前只有 task_run.c 读这个结构，它用的是 beep_marker 一项。） */
app_settings_t g_cfg = {
    .trace_speed  = 6480,
    .speed_kp_x100 = 2000,
    .speed_ki_x100 = 200,
    .speed_kd_x100 = 0,
    .speed_ilim    = 3000,
    .speed_ff_slope_x1000 = 662,
    .speed_ff_fric        = 2327,
    .line_kp_x100 = 270000,
    .line_ki_x100 = 1080,
    .line_kd_x100 = 3600,
    .line_ilim    = 50,
    .gyro_kp_x100 = 15000,
    .gyro_ki_x100 = 0,
    .gyro_kd_x100 = 1500,
    .gyro_ilim    = 1000,
    .use_gyro     = true,
    .beep_key     = false,
    .beep_marker  = true,          /* ★task_run.c 唯一读的那一项（「过点蜂鸣」）*/
    .trace_invert = false,
    .trace_adapt  = true,
    .trace_corner = 0,
    .gray_mode    = TRACE_MODE_AUTO,
    .trace_cal_set = 0
};

#endif /* !HOST_PIPE_REAL_NVRAM */

/* 空实现 + 计数器：主机上没有蜂鸣器，能验的只有"该响的时候调了、时长是多少"。
 * 不动生产代码（app_ui.c 里那份才是真驱动 GPIO 的实现）。 */
void AppUi_Beep(uint16_t ms)
{
    stub_beep_n++;
    stub_beep_ms = (int)ms;
}

/* ---- nvram ---- */
/* 带 `-DHOST_PIPE_REAL_NVRAM` 时改用真 App/Svc/nvram.c（管道对拍要验"固化→掉电→重启"），
 * 这两个假桩必须让位，否则符号冲突。`stub_nvram_touch/stub_nvram_state` 观测变量保持无条件
 * （t_hostext.c 读它们）。 */
#ifndef HOST_PIPE_REAL_NVRAM
void    nvram_touch(void) { stub_nvram_touch++; }
uint8_t nvram_state(void) { return stub_nvram_state; }
#endif /* !HOST_PIPE_REAL_NVRAM */

/* ---- trace_cal / PID_tracing ---- */
/* trace_cal_level() / trace_cal_channel() / trace_cal_select_set() 真 trace_cal.c 里也有
 *（`-DHOST_PIPE_REAL_NVRAM` 会链它）⇒ 同样要让位 */
#ifndef HOST_PIPE_REAL_NVRAM
uint8_t trace_cal_level(uint8_t idx) { return (idx < 8u) ? stub_gray[idx] : 0u; }

/* 「这一路压黑线吗」：给 task_run.c 的 TASK_C_CH_BLACK / TASK_C_CH_WHITE 逐路取位
 *（`ch_black()` 优先用它，回 -1 才回落 trace_cal_level）。
 *
 * 口径与 t_cascade.c 里那份同类桩一致（同一份柱高的**半高判定**）：拿 stub_gray[]
 * 当柱高、`>= 128` 算压黑。★t_cascade.c **另有自己的一份**：那个测试不链本文件，
 * 它的注入点是模拟帧 g_w[]（`trace_cal_frame()` 填），判据同样是"柱高比半高大"，
 * 所以两处不会互相打架，也不存在重复定义（`run_tests.ps1` 的 t_cascade 只链
 * PID_tracing.c + t_cascade.c 自己）。本文件的注入点就是 stub_gray[] ——
 * 默认 {5,15,25,35,45,55,65,75} 全在半高以下 ⇒ **恒 0（没压线）**，是确定值。
 * 越界路返回 0（照 t_cascade 的口径；任务块的掩码只有 8 位，走不到这里）。 */
int trace_cal_channel(uint8_t idx)
{
    if (idx >= 8u) return 0;
    return (stub_gray[idx] >= 128u) ? 1 : 0;
}

/* 切标定套（TASK_OP_CAL_SET：0 = 普通 / 1 = 亚克力）：只**记录最后一次的值**供断言，
 * 不搬任何标定数据（真 trace_cal.c 那一套两套数据在主机测试里没有对应物）。
 * 夹取口径照真实现（trace_cal.c:997 `(set != 0) ? 1 : 0`）——越界值不许记成非法套号。 */
void trace_cal_select_set(uint8_t set) { stub_cal_set = (set != 0u) ? 1 : 0; }
#endif
float   PID_Tracing_TracePos(void) { return stub_trace_pos; }
float   PID_Tracing_PidOutput(void) { return -123.0f; }
uint8_t tracing_cross(void) { return stub_cross; }
void    tracing_cross_clear(void) { stub_cross_clears++; }
uint8_t tracing_lost(void) { return stub_lost; }

/* ---- 任务解释器还要读轮速（里程积分用） ---- */
int SpeedControl_SpeedMmps(int wheel)
{
    return (wheel >= 0 && wheel <= 1) ? stub_speed_mmps[wheel] : 0;
}
