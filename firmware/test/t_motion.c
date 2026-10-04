/**
 * @file  t_motion.c   主机侧单元测试：运动调度 + 安全看门狗（App/Svc/motion.c）
 *
 * motion.c 是 5ms 中断里的总调度，负责"哪个模式跑哪一层环"。这个测试盯三件事：
 *
 *   1) 模式 -> 环路/斜坡的接线没被后人改坏
 *      转角度（MOTION_GYRO / TRACE_GYRO）必须放开斜坡到 100000 PWM/s（否则角度环
 *      反向刹车晚 200ms，实测超调从 1° 变 12~15°），循迹/任务必须留 20000 软启动。
 *
 *   2) 陀螺仪失联看门狗
 *      线一断 Yaw 就是冻结值，角度环会以为"已到位"不再修正 -> 车笔直冲出去。
 *      现在 >500ms 没有**校验通过的新帧**就自动停车并置 fault。用例把 500ms
 *      边界、宽限期、"有帧就不误判"三件事都钉住（帧计数来自真 jy61p.c）。
 *
 *   3) 中断记账不会自己溢出/越界
 *      g_isr_n 每拍 +1、delta 为 0 时 g_isr_over 不动。
 *
 * 依赖：motion.c + 真 jy61p.c（喂真帧），其余服务用本文件的桩。
 */
#include <stdint.h>
#include <string.h>
#include "motion.h"
#include "jy61p.h"
#include "gyro_control.h"
#include "speed_control.h"
#include "PID_tracing.h"
#include "host_link.h"
#include "motor.h"
#include "t_common.h"

extern int host_load_left(void);
extern int host_load_right(void);
void host_tick_add(uint32_t ms);

/* ---- 参数表桩 ---- */
static float g_p[65536];
static uint8_t g_set[65536];

float host_param_get(uint16_t id, float fallback)
{
    return g_set[id] ? g_p[id] : fallback;
}
void host_param_set_local(uint16_t id, float v)
{
    g_p[id] = v;
    g_set[id] = 1u;
}

/* ---- 各层桩：只记调用次数/参数，验证接线 ---- */
static int     gc_start, gc_stop, gc_task, gc_base_calls;
static uint8_t gc_enabled;
static int     sc_init, sc_stop, sc_task, sc_enable_last;
static float   sc_ramp;
static int     sc_tgt_l, sc_tgt_r;
static int     cascade_reset, trace_pid_calls, cascade_calls, bg_calls;

void    GyroControl_Start(void)  { gc_start++; gc_enabled = 1u; }
void    GyroControl_Stop(void)   { gc_stop++;  gc_enabled = 0u; }
void    GyroControl_Task(void)   { gc_task++; }
uint8_t GyroControl_IsEnabled(void) { return gc_enabled; }
void    GyroControl_SetBaseSpeed(int l, int r) { (void)l; (void)r; gc_base_calls++; }

void    SpeedControl_Init(void)  { sc_init++; }
void    SpeedControl_Enable(uint8_t on) { sc_enable_last = (int)on; }
uint8_t SpeedControl_IsEnabled(void) { return 0u; }
void    SpeedControl_SetTarget(int l, int r) { sc_tgt_l = l; sc_tgt_r = r; }
void    SpeedControl_SetRamp(float r) { sc_ramp = r; }
void    SpeedControl_Stop(void)  { sc_stop++; Load(0, 0); }   /* 真实现里 Stop() 会 Load(0,0) */
void    SpeedControl_Task(void)  { sc_task++; }

void    motor_tracing_reset_cascade(void) { cascade_reset++; }
void    motor_tracing_pid_speed(int base_speed) { (void)base_speed; trace_pid_calls++; }
void    motor_tracing_cascade(int base_speed) { (void)base_speed; cascade_calls++; }
uint8_t PID_Tracing_BackgroundTask(void) { bg_calls++; return 0u; }

/* ---- 造一帧真的 JY61P 角度帧（0x55 0x53 ... 校验和）喂进解析器 ---- */
static uint32_t feed_yaw_frame(float yaw_deg)
{
    uint8_t f[11];
    int16_t raw = (int16_t)(yaw_deg / 180.0f * 32768.0f);
    uint8_t i, sum = 0u;
    uint32_t before = jy61p_frame_count();

    f[0] = 0x55u;
    f[1] = 0x53u;
    f[2] = (uint8_t)((uint16_t)raw & 0xFFu);          /* roll */
    f[3] = (uint8_t)(((uint16_t)raw >> 8) & 0xFFu);
    f[4] = 0u; f[5] = 0u;                             /* pitch */
    f[6] = (uint8_t)((uint16_t)raw & 0xFFu);          /* yaw 在 6,7 */
    f[7] = (uint8_t)(((uint16_t)raw >> 8) & 0xFFu);
    f[8] = 0u; f[9] = 0u;
    for (i = 0u; i < 10u; i++) sum = (uint8_t)(sum + f[i]);
    f[10] = sum;

    for (i = 0u; i < 11u; i++) jy61p_ReceiveData(f[i]);
    CHECK(jy61p_frame_count() == before + 1u);        /* 帧必须真的被解析出来 */
    return jy61p_frame_count();
}

static void back_to_idle(void)
{
    Motion_SetMode(MOTION_IDLE);
}

int main(void)
{
    Motion_Init();
    CHECK_EQ_I(sc_init, 1);
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_IDLE);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);

    /* ---------------------------------------------------------------
     * 1) 模式 -> 环路 + 斜坡接线
     * --------------------------------------------------------------- */
    gc_start = gc_stop = cascade_reset = 0;
    Motion_SetMode(MOTION_GYRO);
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_GYRO);
    CHECK_EQ_I(gc_start, 1);                 /* 记当前航向为目标 */
    CHECK(sc_ramp > 99000.0f && sc_ramp < 101000.0f);   /* 转角度：放开反向 */
    CHECK_EQ_I(sc_enable_last, 1);
    CHECK_EQ_I(sc_tgt_l, 0);
    CHECK_EQ_I(sc_tgt_r, 0);

    back_to_idle();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_IDLE);
    CHECK(gc_stop >= 1);
    CHECK(sc_stop >= 1);
    CHECK_EQ_I(host_load_left(), 0);         /* 停车必须真的把 PWM 写 0 */
    CHECK_EQ_I(host_load_right(), 0);

    sc_ramp = 0.0f;
    cascade_reset = 0;
    Motion_SetMode(MOTION_TRACE);
    CHECK(sc_ramp > 19000.0f && sc_ramp < 21000.0f);    /* 循迹：留 20000 软启动 */
    CHECK_EQ_I(cascade_reset, 1);                       /* 进任何非 IDLE 模式都重记基准航向 */

    back_to_idle();
    sc_ramp = 0.0f;
    gc_start = 0;
    cascade_reset = 0;
    Motion_SetMode(MOTION_TRACE_GYRO);
    CHECK(sc_ramp > 99000.0f && sc_ramp < 101000.0f);
    CHECK_EQ_I(gc_start, 1);
    CHECK_EQ_I(cascade_reset, 1);

    /* 一拍里的分工：哪个模式调哪个环 */
    gc_task = trace_pid_calls = cascade_calls = bg_calls = 0;
    Motion_Task();
    CHECK_EQ_I(cascade_calls, 1);
    CHECK_EQ_I(trace_pid_calls, 0);
    CHECK_EQ_I(gc_task, 0);                  /* 串级里角度环由 PID_tracing 侧驱动 */

    back_to_idle();
    trace_pid_calls = cascade_calls = gc_task = bg_calls = 0;
    Motion_SetMode(MOTION_TRACE);
    Motion_Task();
    CHECK_EQ_I(trace_pid_calls, 1);
    CHECK_EQ_I(cascade_calls, 0);
    CHECK_EQ_I(bg_calls, 0);                 /* 运动模式不跑后台任务环 */

    back_to_idle();
    gc_task = bg_calls = 0;
    Motion_SetMode(MOTION_IDLE);             /* 同模式 IDLE：现在是"确保停"，会把角度环关掉 */
    gc_enabled = 1u;                         /* 任务流程/旧板后台角度环：进 IDLE 后由它自己打开 */
    Motion_Task();
    CHECK_EQ_I(gc_task, 1);
    CHECK_EQ_I(bg_calls, 1);
    gc_enabled = 0u;

    /* ---------------------------------------------------------------
     * 2) 陀螺仪失联看门狗（500ms 无新帧）
     * --------------------------------------------------------------- */
    host_tick_add(100000u);                  /* 把假时间推到足够大，避免和为 0 混淆 */
    back_to_idle();
    Motion_SetMode(MOTION_GYRO);
    Motion_Task();                           /* 第一拍：消化角度环打开的上升沿（宽限基准在这里重置） */

    /* 2a) 刚进模式的宽限期：400ms 没帧也不能判失联 */
    host_tick_add(400u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_GYRO);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);

    /* 2b) 边界 = 500ms：恰好 500 不触发，501 触发 */
    host_tick_add(100u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_GYRO);
    host_tick_add(1u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_IDLE);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_GYRO_STALE);
    CHECK_EQ_I(host_load_left(), 0);         /* 失联必须停车 */
    CHECK_EQ_I(host_load_right(), 0);

    /* 2c) 帧在动就不该误判：每 400ms 一帧，跑 3 秒仍然在 GYRO */
    back_to_idle();
    Motion_SetMode(MOTION_GYRO);
    {
        int k;
        for (k = 0; k < 8; k++) {
            host_tick_add(400u);
            feed_yaw_frame((float)k * 5.0f);
            Motion_Task();
            CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_GYRO);
            CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
        }
    }

    /* 2d) 帧停住之后必须重新触发（故障恢复后还能再报） */
    host_tick_add(600u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_IDLE);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_GYRO_STALE);

    /* 2e) 重新进模式要把故障清掉（否则界面一直挂着旧故障） */
    back_to_idle();
    Motion_SetMode(MOTION_GYRO);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    back_to_idle();

    /* 2f) 不用陀螺仪的模式（循迹/后台任务）绝不因为陀螺仪死了就停 */
    feed_yaw_frame(0.0f);
    Motion_SetMode(MOTION_TRACE);
    host_tick_add(5000u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_TRACE);
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    back_to_idle();

    /* 2g) 任务路径：不走 Motion_SetMode，直接 GyroControl_Start()（任务解释器的
     *     Toggle_Gyro_Control(1,..) 就是这样）—— 失联同样必须停车 + 置故障。 */
    feed_yaw_frame(0.0f);
    Motion_ClearFault();
    gc_enabled = 1u;                         /* 相当于 Toggle_Gyro_Control 打开了角度环 */
    host_tick_add(3000u);
    Motion_Task();                           /* 上升沿：重置宽限，不能立刻判死 */
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    CHECK_EQ_I(host_load_left(), 0);         /* 宽限期内不许停车 */
    host_tick_add(400u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    host_tick_add(200u);                     /* 累计 600ms > 500ms 且一帧都没来 */
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_GYRO_STALE);
    CHECK_EQ_I((int)gc_enabled, 0);          /* 角度环被强制关掉 */
    CHECK_EQ_I(host_load_left(), 0);         /* 并且真的把 PWM 写 0（任务流程不在运动模式里） */
    CHECK_EQ_I(host_load_right(), 0);

    /* 2h) 任务流程里重新打开角度环、帧也回来了 -> 宽限重算，不许误判 */
    Motion_ClearFault();
    feed_yaw_frame(10.0f);
    gc_enabled = 1u;
    Motion_Task();                           /* 上升沿 */
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    host_tick_add(400u);
    feed_yaw_frame(11.0f);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    CHECK_EQ_I((int)gc_enabled, 1);          /* 有帧就一直是"活着" */
    gc_enabled = 0u;

    /* 2i) ClearFault() 必须真的把旧故障清掉（开机/开始任务时用） */
    Motion_SetMode(MOTION_GYRO);
    host_tick_add(5000u);
    Motion_Task();
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_GYRO_STALE);
    Motion_ClearFault();
    CHECK_EQ_I((int)Motion_GetFault(), (int)MOTION_FAULT_NONE);
    back_to_idle();

    /* ---------------------------------------------------------------
     * 3) 中断记账：每拍 +1，delta 为 0 时不计超预算
     * --------------------------------------------------------------- */
    {
        uint32_t n0 = g_isr_n;
        uint32_t o0 = g_isr_over;

        Motion_Task();
        Motion_Task();
        CHECK_EQ_I((long long)(g_isr_n - n0), 2);
        CHECK_EQ_I((long long)(g_isr_over - o0), 0);
        CHECK_EQ_I((long long)g_isr_cyc_last, 0);   /* 主机假 CYCCNT 不前进 */
    }

    /* ---------------------------------------------------------------
     * 4) 同模式的 Motion_SetMode 必须保证速度环开着（用户 m28949）
     *    任务块的 SET_MODE 早先是"先 SpeedControl_Stop() 再 SetMode"：
     *    同模式时 SetMode 本来是空操作 ⇒ 速度环永远关着（它的 Task() 因
     *    s_enabled=0 直接返回，PWM 恒 0）—— 车"根本就不动"。
     * --------------------------------------------------------------- */
    back_to_idle();
    SpeedControl_Stop();                      /* 相当于任务块 SET_MODE 先停环 */
    sc_enable_last = 0;
    Motion_SetMode(MOTION_TRACE_GYRO);        /* 真换模式：照旧 Enable(1) */
    CHECK_EQ_I(sc_enable_last, 1);
    CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_TRACE_GYRO);

    {
        int stops_before = sc_stop;

        sc_enable_last = 0;                   /* 清掉上一次的记录 */
        Motion_SetMode(MOTION_TRACE_GYRO);    /* 同模式：不能再空返回 */
        CHECK_EQ_I(sc_enable_last, 1);        /* 必须重新把速度环打开 */
        CHECK_EQ_I(sc_stop, stops_before);    /* 同模式也不许再 Stop（Stop 里会 Load(0,0)） */
        CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_TRACE_GYRO);
    }

    /* ---------------------------------------------------------------
     * 4b) 同模式的 IDLE 必须真的停（用户 m29239）
     *     WHEELS 直给（host_ext.c）和电机测试页都是"常驻 IDLE + 自己使能速度环"，
     *     同模式早退会把"停车"变成空操作：stop 之后车还在跑，只有拔电池才停。
     * --------------------------------------------------------------- */
    back_to_idle();                           /* 真转换回 IDLE */
    {
        int stops_before = sc_stop;

        Motion_SetMode(MOTION_IDLE);          /* 同模式 IDLE：必须再 Stop 一次 */
        CHECK_EQ_I(sc_stop, stops_before + 1);
        CHECK_EQ_I(host_load_left(), 0);      /* 停车必须真的把 PWM 写 0 */
        CHECK_EQ_I(host_load_right(), 0);
        CHECK_EQ_I((int)Motion_GetMode(), (int)MOTION_IDLE);
    }

    back_to_idle();

    return T_SUMMARY();
}
