/**
 * @file  motion.c
 * @brief 三层串级调度：最外层（循迹/角度）先算目标速度，最内层（速度环）再算 PWM。
 */
#include "motion.h"
#include "speed_control.h"
#include "gyro_control.h"
#include "PID_tracing.h"
#include "motor.h"
#include "host_link.h"
#include "jy61p.h"

/* 陀螺仪活性看门狗：**只要角度环在跑**（UI 的 MOTION_GYRO / MOTION_TRACE_GYRO，
 * 或者任务流程 renwu.c 用 Toggle_Gyro_Control() 直接开的那些转弯），只要 500ms
 * 没解析出新帧（JY61P 默认 100Hz，正常 10ms 一帧）就判定失联并停车。
 *
 * 为什么必须停：角度环拿的是 Yaw 的**冻结值** —— 传感器一死，环会认为"已经
 * 朝着目标、误差 0"，于是不再修正；循迹串级里外层改的目标航向也就跟不动了，
 * 车会笔直冲出去（比停下来危险得多）。故障方向只能是"停"。
 *
 * 判据用 GyroControl_IsEnabled() 而不是运动模式：任务流程（renwu.c）不走
 * Motion_SetMode，它只是在 IDLE 模式下借用同一个角度环，光看模式会漏掉它。
 * 上升沿（角度环刚被打开）给 500ms 宽限：上电/刚插上模块时它可能还没开始发帧。 */
#define GYRO_STALE_MS 500U

static uint32_t        s_gyro_frames    = 0U;   /* 上次看到的成功帧数 */
static uint32_t        s_gyro_frame_ms  = 0U;   /* 最近一次"帧数变了"的时刻 */
static uint8_t         s_gyro_watch     = 0U;   /* 上一拍角度环是否在跑（用来认上升沿） */
static volatile uint8_t s_fault = MOTION_FAULT_NONE;

static Motion_Mode s_mode = MOTION_IDLE;
static int         s_base = 6480;

/* 5ms 控制中断耗时统计（声明与用法见 motion.h；主循环每 1s 打印并清零 sum/max/n） */
volatile uint32_t g_isr_cyc_last = 0U;
volatile uint32_t g_isr_cyc_max  = 0U;
volatile uint32_t g_isr_cyc_sum  = 0U;
volatile uint32_t g_isr_n        = 0U;
volatile uint32_t g_isr_over     = 0U;

void Motion_Init(void)
{
    s_mode = MOTION_IDLE;
    s_gyro_watch = 0U;
    s_fault = MOTION_FAULT_NONE;
    /* 兜底默认值 = app_ui.c 的 g_cfg.trace_speed 默认 6480（旧板刻度 900，旧板满量程 1000）。
     * 旧板这里写死 1000 = 满油门；照抄到本板（满量程 7200）只有 13.9% 占空比，低于静摩擦
     * 死区（~2500~2900），车推不动（m02112 同步）。 */
    s_base = (int)host_param_get(HP_BASE_SPEED, 6480.0f);
    SpeedControl_Init();
}

void Motion_SetSpeed(int base_speed)
{
    s_base = base_speed;
}

int Motion_GetSpeed(void)
{
    return s_base;
}

Motion_Mode Motion_GetMode(void)
{
    return s_mode;
}

uint8_t Motion_GetFault(void)
{
    return s_fault;
}

void Motion_ClearFault(void)
{
    s_fault = MOTION_FAULT_NONE;
}

void Motion_SetMode(Motion_Mode m)
{
    if (m == s_mode) return;

    if (m == MOTION_IDLE) {
        GyroControl_Stop();
        SpeedControl_Stop();          /* 里面会 Load(0,0) */
        s_mode = MOTION_IDLE;
        return;
    }

    motor_tracing_reset_cascade();     /* 重新记基准航向（循迹环和串级都要） */

    if (m == MOTION_GYRO || m == MOTION_TRACE_GYRO) {
        GyroControl_Start();           /* 记录当前航向为目标 */
        /* 刚进模式时给 500ms 宽限：上电/刚插上陀螺仪时它可能还没开始发帧，
         * 不能一进模式就判失联。 */
        s_gyro_frames   = jy61p_frame_count();
        s_gyro_frame_ms = HAL_GetTick();
        s_fault         = MOTION_FAULT_NONE;
        /* 角度环的修正是直接顶到左右轮目标上的，它必须能立刻反向：
         * 默认 20000 PWM/s 的斜坡下，+4000 → −4000 要拖 400ms，刹车指令晚到
         * 200ms 以上 ⇒ 实测 20° 台阶超调从 1° 涨到 12~15°（可能失控）。
         * 循迹/任务路径保持 20000 软启动不动，只有转角度时放开。 */
        SpeedControl_SetRamp(100000.0f);
    } else {
        GyroControl_Stop();
        SpeedControl_SetRamp(20000.0f);    /* 软启动 */
    }
    SpeedControl_SetTarget(0, 0);
    SpeedControl_Enable(1U);
    s_mode = m;
}

void Motion_Stop(void)
{
    Motion_SetMode(MOTION_IDLE);
}

void Motion_Task(void)
{
    uint32_t t0 = DWT->CYCCNT;

    /* 陀螺仪活性检查（放在最前，5ms 一拍）。只有"帧数变了"才算活着：
     * 线断了以后 Yaw/Gyro_Z 是冻结的旧值，光看数值是看不出来的。 */
    {
        uint8_t on = GyroControl_IsEnabled() ? 1U : 0U;

        if (on != s_gyro_watch) {              /* 角度环刚被打开/关掉 */
            s_gyro_watch = on;
            if (on) {                          /* 上升沿：记基准帧数和时刻，给宽限 */
                s_gyro_frames   = jy61p_frame_count();
                s_gyro_frame_ms = HAL_GetTick();
                s_fault         = MOTION_FAULT_NONE;
            }
        } else if (on) {
            uint32_t n   = jy61p_frame_count();
            uint32_t now = HAL_GetTick();

            if (n != s_gyro_frames) {
                s_gyro_frames   = n;
                s_gyro_frame_ms = now;
            } else if ((uint32_t)(now - s_gyro_frame_ms) > GYRO_STALE_MS) {
                s_fault = MOTION_FAULT_GYRO_STALE;
                GyroControl_Stop();             /* 关角度环（任务流程不走下面的 Motion_SetMode） */
                SpeedControl_Stop();            /* 里面 Load(0,0)：不管在哪个模式都必须停 */
                Motion_SetMode(MOTION_IDLE);    /* UI 的陀螺模式在这切回 IDLE；本来就是 IDLE 就直接返回 */
            }
        }
    }

    switch (s_mode) {
    case MOTION_GYRO:
        GyroControl_SetBaseSpeed(s_base, s_base);
        GyroControl_Task();
        break;

    case MOTION_TRACE:
        motor_tracing_pid_speed(s_base);      /* 循迹环 -> 速度环目标 */
        break;

    case MOTION_TRACE_GYRO:
        motor_tracing_cascade(s_base);        /* 循迹 -> 角度 -> 速度环目标 */
        break;

    case MOTION_IDLE:
    default:
        /* 任务流程（renwu.c）不走运动模式：它直接调 Toggle_Gyro_Control() 和
         * PID_Tracing_Timer_Start()。旧板这两种“后台环”分别跑在 TIM4(10ms) 和 TIM2(10Hz)
         * 的中断里；新板没有多余的周期定时器，统一在这里补上：
         *   · 角度环（5ms 一拍，gyro_control_enabled 关着时 GyroControl_Task() 自己返回）
         *   · 后台循迹环（内部 /20 分频 = 100ms，保持旧板 TIM2 的采样率）
         * 运动模式（MOTION_GYRO/TRACE/TRACE_GYRO）与任务流程互斥：进任务前会 Motion_Stop()。 */
        if (GyroControl_IsEnabled()) {
            GyroControl_Task();
        }
        (void)PID_Tracing_BackgroundTask();
        break;
    }

    SpeedControl_Task();    /* 最内层：永远测速；只有使能时才写 PWM */

    /* 本拍耗时（DWT 是 32 位、170MHz 下 25s 回绕，无符号相减天然处理回绕） */
    {
        uint32_t d = DWT->CYCCNT - t0;

        g_isr_cyc_last = d;
        if (d > g_isr_cyc_max) g_isr_cyc_max = d;
        g_isr_cyc_sum += d;
        g_isr_n++;
        if (d > MOTION_ISR_BUDGET_CYC) g_isr_over++;
    }
}
