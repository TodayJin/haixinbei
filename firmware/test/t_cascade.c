/**
 * @file  t_cascade.c   主机侧单元测试：循迹串级（App/Svc/PID_tracing.c）
 *
 * 为什么值得测（用户 m30082："模拟一些传感器的值，跑一些循迹的 PID，把所有功能都验证一下"）：
 *   PID_tracing.c 是**唯一**把 8 路灰度变成轮速指令的地方，但它此前没有任何主机测试
 *   （run_tests.ps1 的 t_tracing 只链了 tracing.c，t_motion 里 motor_tracing_cascade()
 *   整个被替身顶掉）。三轮真车事故（不循迹 / 直着走 / 7200 不动）都落在这个文件里：
 *     · 2026-10-05 幻影中线：探头底下没线时，自适应帧把 8 路全归一到"很黑"，
 *       重心算出来正好 4.5 ⇒ 固件以为线在正中，满速直走还不报丢线（用户 m29852）。
 *     · 同一事故的第二条路：模拟量断了会回落到 PA0~PA7 裸脚，那 8 脚上拉恒为高
 *       ⇒ 掩码 0xFF ⇒ num=8/sum=36 ⇒ 又是 4.5/不丢线。
 *   这里用**模拟的传感器值**把两条路都钉死，并验证"丢线久了必须停车"（用户 m29239）。
 */
#include <math.h>
#include <stdint.h>

#include "PID_tracing.h"
#include "motor.h"
#include "pid.h"
#include "jy61p.h"
#include "trace_cal.h"
#include "tracing.h"
#include "speed_control.h"
#include "gyro_control.h"
#include "host_link.h"     /* HP_TRACE_ANGLE_PER_UNIT / HP_BASE_SPEED_xxx 参数 id */
#include "t_common.h"

/* test/stub/host_hal.c 与 host_board.c 的注入口/观测口 */
extern void host_tick_set(uint32_t ms);
extern void host_tick_add(uint32_t ms);
extern int  host_load_left(void);
extern int  host_load_right(void);

/* ---- 可控桩：灰度帧（模拟传感器） ---- */
static trace_frame_t g_fr = TRACE_FRAME_OK;
static float         g_w[TRACE_CAL_CH];
uint8_t              trace_cal_mode = TRACE_MODE_AUTO;

trace_frame_t trace_cal_frame(float w[TRACE_CAL_CH])
{
    for (int i = 0; i < (int)TRACE_CAL_CH; i++) w[i] = g_w[i];
    return g_fr;
}

/* 数字口（只有 TRACE_MODE_DIGITAL 才该被当成真值） */
static int g_dig[8];
int trace_read_channel(uint8_t idx) { return (idx < 8u) ? g_dig[idx] : 0; }

/* ---- 可控桩：速度环 / 角度环 / PID / 参数表 ---- */
static int g_sc_enabled, g_sc_l, g_sc_r, g_sc_stops;
uint8_t SpeedControl_IsEnabled(void)  { return (uint8_t)g_sc_enabled; }
void    SpeedControl_SetTarget(int l, int r) { g_sc_l = l; g_sc_r = r; }
void    SpeedControl_Stop(void)       { g_sc_stops++; g_sc_enabled = 0; g_sc_l = g_sc_r = 0; }

static float g_gyro_target;
static int   g_gyro_l, g_gyro_r, g_gyro_tasks, g_gyro_stops;
void GyroControl_SetTargetAngle(float deg) { g_gyro_target = deg; }
void GyroControl_SetBaseSpeed(int l, int r) { g_gyro_l = l; g_gyro_r = r; }
void GyroControl_Task(void)  { g_gyro_tasks++; }
void GyroControl_Stop(void)  { g_gyro_stops++; }

float Yaw = 0.0f;
float NormalizeAngle(float a)
{
    while (a > 180.0f)  a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

static int   g_pid_resets, g_pid_pivot, g_pid_calls;
static float g_pid_sp, g_pid_fb;
void  PID_Reset(void)   { g_pid_resets++; }
void  PID_SetPivot(int on) { g_pid_pivot = on; }
float PID_Compute(float sp, float fb) { g_pid_calls++; g_pid_sp = sp; g_pid_fb = fb; return 111.0f; }

static float g_pu = 10.0f, g_amax = 35.0f;
void host_param_set_local(uint16_t id, float v)      /* 上位机点改参数：这里只记进本场景的"参数表" */
{
    if (id == HP_TRACE_ANGLE_PER_UNIT) g_pu = v;
    if (id == HP_TRACE_ANGLE_MAX)      g_amax = v;
}
float host_param_get(uint16_t id, float fallback)
{
    if (id == HP_TRACE_ANGLE_PER_UNIT) return g_pu;
    if (id == HP_TRACE_ANGLE_MAX)      return g_amax;
    if (id == HP_BASE_SPEED)           return 6480.0f;
    if (id == HP_BASE_SPEED_SLOW)      return 2500.0f;
    return fallback;
}

/* ---- 场景脚手架 ---- */
static void scene(void)
{
    motor_tracing_reset_cascade();
    host_tick_set(1000u);
    g_fr = TRACE_FRAME_OK;
    trace_cal_mode = TRACE_MODE_AUTO;
    for (int i = 0; i < (int)TRACE_CAL_CH; i++) { g_w[i] = 0.0f; if (i < 8) g_dig[i] = 0; }
    g_sc_enabled = 0; g_sc_l = g_sc_r = 0; g_sc_stops = 0;
    g_gyro_l = g_gyro_r = 0; g_gyro_tasks = 0; g_gyro_stops = 0; g_gyro_target = 999.0f;
    g_pid_resets = 0; g_pid_calls = 0; g_pid_pivot = -1;
    Yaw = 0.0f; g_pu = 10.0f; g_amax = 35.0f;
}

static void w_line(int ch, float amp)          /* 线压在 ch(1..8) 下 */
{
    for (int i = 0; i < (int)TRACE_CAL_CH; i++) g_w[i] = 0.0f;
    if (ch >= 1 && ch <= 8) g_w[ch - 1] = amp;
}

int main(void)
{
    CHECK_EQ_I(TRACE_CAL_CH, 8);

    /* §1 正中间：重心 4.5，航向偏移 0，基准速度原样交给角度环 */
    scene();
    g_w[3] = 200.0f; g_w[4] = 200.0f;
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 0);
    CHECK(fabsf(PID_Tracing_TracePos() - 4.5f) < 0.01f);
    CHECK(fabsf(g_gyro_target - 0.0f) < 0.01f);
    CHECK_EQ_I(g_gyro_l, 1200);
    CHECK_EQ_I(g_gyro_r, 1200);
    CHECK_EQ_I(g_gyro_tasks, 1);

    /* §2 线在右边 ⇒ 目标航向往右（负）：aim = -per_unit*err，随重心连续变化 */
    scene();
    g_w[5] = 200.0f; g_w[4] = 100.0f;
    motor_tracing_cascade(1200);
    {
        float err = PID_Tracing_TracePos() - 4.5f;
        CHECK(err > 0.5f);                                    /* 确认真在右边 */
        CHECK(fabsf(g_gyro_target - (0.0f + (-g_pu * err))) < 0.05f);
    }

    /* §3 线在左边 ⇒ 目标航向往左（正），且与 §2 反号 */
    scene();
    g_w[2] = 200.0f; g_w[3] = 100.0f;
    motor_tracing_cascade(1200);
    {
        float err = PID_Tracing_TracePos() - 4.5f;
        CHECK(err < -0.5f);
        CHECK(fabsf(g_gyro_target - (0.0f + (-g_pu * err))) < 0.05f);
        CHECK(g_gyro_target > 0.0f);
    }

    /* §4 航向偏移夹到 trace_angle_max（参数 518） */
    scene();
    g_pu = 10.0f; g_amax = 12.0f;
    w_line(2, 200.0f);                                       /* 重心 2 ⇒ err -2.5 ⇒ raw aim +25 */
    for (int i = 0; i < 3; i++) motor_tracing_cascade(1200); /* 3 帧过掉重心中值滤波 */
    CHECK(PID_Tracing_TracePos() - 4.5f < -2.0f);
    CHECK(fabsf(g_gyro_target - 12.0f) < 0.01f);              /* 不夹的话会是 +25 */

    /* §5 ★幻影中线（用户 m29852）：探头底下没线，自适应帧 8 路全白 ⇒ 必须判丢线 */
    scene();
    for (int i = 0; i < 8; i++) g_w[i] = 200.0f;              /* 均匀一片 = 无线 */
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 1);
    CHECK_EQ_I(g_gyro_l, 1200);                               /* 刚丢线：先照常走 */
    host_tick_add(350u);                                      /* 超 TRACE_LOST_MS(300) */
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 1);
    CHECK_EQ_I(g_gyro_l, 600);                                /* 慢速找线 */
    host_tick_add(1000u);                                     /* 超 TRACE_LOST_STOP_MS(1000) */
    motor_tracing_cascade(1200);
    CHECK_EQ_I(g_gyro_stops, 1);                              /* 角度环松手 */
    CHECK_EQ_I(g_sc_stops, 1);                                /* 速度环停 */
    CHECK_EQ_I(host_load_left(), 0);                          /* 直接写 PWM 也是 0 */
    CHECK_EQ_I(host_load_right(), 0);
    motor_tracing_cascade(1200);                              /* 持续丢线：不许反复下"停"指令 */
    CHECK_EQ_I(g_gyro_stops, 1);
    CHECK_EQ_I(g_sc_stops, 1);

    /* §6 帧判 OK 但 8 路权重全 0（模块回帧但全白）⇒ 也是丢线，且不许除零变 NaN */
    scene();
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 1);
    CHECK(PID_Tracing_TracePos() == PID_Tracing_TracePos());  /* NaN 自比较为假 ⇒ UBSan 下也不许出现 */

    /* §7 帧判 WEAK（模块说这帧不可信）⇒ 丢线 */
    scene();
    g_fr = TRACE_FRAME_WEAK;
    g_w[3] = 200.0f;
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 1);

    /* §8 ★断链回落（用户 m29852 事故第二条路）：模拟量断了、又不是"数字量"模式时，
     *     以前拿 PA0~PA7 裸脚当掩码（上拉恒 1 ⇒ 0xFF ⇒ 重心 4.5、不丢线），
     *     现在必须判丢线 —— 这是"直着走"的根，回归保护。 */
    scene();
    g_fr = TRACE_FRAME_FALLBACK;
    trace_cal_mode = TRACE_MODE_AUTO;
    for (int i = 0; i < 8; i++) g_dig[i] = 1;                /* 裸脚全 1 = 上拉 */
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 1);
    CHECK_EQ_I(g_gyro_l, 1200);
    host_tick_add(1100u);
    motor_tracing_cascade(1200);
    CHECK_EQ_I(host_load_left(), 0);
    CHECK_EQ_I(host_load_right(), 0);

    /* §9 用户显式选了"数字量"模式时，裸脚才是真值（有 2 拍消抖）*/
    scene();
    g_fr = TRACE_FRAME_FALLBACK;
    trace_cal_mode = TRACE_MODE_DIGITAL;
    g_dig[2] = 1;                                            /* 只有第 3 路压线 */
    motor_tracing_cascade(1200);
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 0);
    CHECK(fabsf(PID_Tracing_TracePos() - 3.0f) < 0.01f);

    /* §10 找回线要复跑：清积分 + 重新起步，且"停"状态被解除 */
    scene();
    for (int i = 0; i < 8; i++) g_w[i] = 200.0f;
    motor_tracing_cascade(1200);
    host_tick_add(1500u);
    motor_tracing_cascade(1200);
    CHECK_EQ_I(g_sc_stops, 1);
    g_w[3] = 200.0f; g_w[4] = 200.0f;                        /* 重新压到线 */
    for (int i = 0; i < 8; i++) if (i != 3 && i != 4) g_w[i] = 0.0f;
    motor_tracing_cascade(1200);
    CHECK_EQ_I(tracing_lost(), 0);
    CHECK(g_pid_resets >= 1);                                /* 内环积分清零 */
    CHECK_EQ_I(g_gyro_l, 1200);                              /* 速度回来 */
    CHECK_EQ_I(g_gyro_r, 1200);

    /* §11 上位机可以点改串级增益（参数 517/518），越界要被夹住 */
    scene();
    PID_Tracing_SetCascadeGains(6.0f, 12.0f);
    {
        float pu = 0.0f, am = 0.0f;
        PID_Tracing_GetCascadeGains(&pu, &am);
        CHECK(fabsf(pu - 6.0f) < 0.001f);
        CHECK(fabsf(am - 12.0f) < 0.001f);
    }
    PID_Tracing_SetCascadeGains(0.0f, 0.0f);                 /* 0 = 不合法，兜底夹到下限/上限 */
    {
        float pu = 0.0f, am = 0.0f;
        PID_Tracing_GetCascadeGains(&pu, &am);
        CHECK(pu >= 0.5f);
        CHECK(am >= 1.0f);
    }
    PID_Tracing_SetCascadeGains(10.0f, 35.0f);

    /* §12 老差速直出路径（MOTION_TRACE，用户没在用但代码还在）：PID 输出当差速；
     *     基准 0 时切 pivot 增益；丢线久了要收到 0 并减速 */
    scene();
    g_w[3] = 200.0f; g_w[4] = 200.0f;
    motor_tracing_pid_speed(1200);
    CHECK_EQ_I(g_pid_calls, 1);
    CHECK(fabsf(g_pid_sp - 4.5f) < 0.01f);
    CHECK_EQ_I(g_pid_pivot, 0);                              /* 前进用 line 增益 */
    CHECK_EQ_I(host_load_left(), 1200 - 111);
    CHECK_EQ_I(host_load_right(), 1200 + 111);
    scene();
    g_w[3] = 200.0f;
    motor_tracing_pid_speed(0);                              /* 原地转 ⇒ 该用 pivot 增益 */
    CHECK_EQ_I(g_pid_pivot, 1);
    scene();
    for (int i = 0; i < 8; i++) g_w[i] = 200.0f;             /* 丢线 */
    motor_tracing_pid_speed(1200);
    host_tick_add(1000u);
    motor_tracing_pid_speed(1200);
    CHECK(host_load_left() < 1200);                          /* 输出在衰减 + 基准减半 */
    CHECK(host_load_right() < 1200);

    return T_SUMMARY();
}
