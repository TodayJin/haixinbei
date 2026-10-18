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
 * 或者任务解释器用 Toggle_Gyro_Control() 直接开的那些转弯），只要 500ms
 * 没解析出新帧（JY61P 默认 100Hz，正常 10ms 一帧）就判定失联并停车。
 *
 * 为什么必须停：角度环拿的是 Yaw 的**冻结值** —— 传感器一死，环会认为"已经
 * 朝着目标、误差 0"，于是不再修正；循迹串级里外层改的目标航向也就跟不动了，
 * 车会笔直冲出去（比停下来危险得多）。故障方向只能是"停"。
 *
 * 判据用 GyroControl_IsEnabled() 而不是运动模式：任务解释器不走
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

/* ★2026-10-15：分段耗时峰值 —— 找 TIM6 中断里的 4ms 尖峰用。
 * 逐帧实测 loop_us：中位 8us、最大 4308us、>500us 占 24% ⇒ **卡顿在中断内部**。
 * 只知总时长定位不到函数，所以把 Motion_Task() 切成 3 段各记峰值：
 *   [0] 陀螺活性看门狗   [1] 模式段（角度环 / 循迹环 / IDLE 后台）   [2] 速度环
 * 单位 DWT 周期（170MHz ⇒ 1us = 170）。用 SWD 读，不占遥测字节。 */
volatile uint32_t g_seg_cyc_max[3] = {0U, 0U, 0U};

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
    if (m == s_mode) {
        /* 幂等调用：切换动作不重复做，但**必须保证速度环的状态是对的**。
         *
         * 方向一（用户 m28949）：任务块的 SET_MODE 早先是在调用前先 SpeedControl_Stop()，
         * 而这里对同模式直接 return ⇒ 速度环被永久关掉（它的 Task() 因 s_enabled=0 立刻
         * 返回，PWM 一直 0）—— "跑循迹根本就不动"就是这个组合。
         * 现在 task_run.c 侧只在真换模式时才 Stop，这里是第二道保险。
         *
         * 方向二（用户 m29239）：WHEELS 直给（host_ext.c）和电机测试页都是"常驻 IDLE +
         * 自己使能速度环"，于是同模式的 MOTION_IDLE 早退会把"停车"变成空操作 ——
         * stop 之后车还在跑、只有拔电池才停。同模式 IDLE 必须真的停。 */
        if (m == MOTION_IDLE) {
            GyroControl_Stop();
            SpeedControl_Stop();      /* 里面会 Load(0,0) */
        } else if (!SpeedControl_IsEnabled()) {
            SpeedControl_SetTarget(0, 0);
            SpeedControl_Enable(1U);
        }
        return;
    }

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
    uint32_t c1 = 0U, c2 = 0U;      /* ★分段打点（见 g_seg_cyc_max 的说明）*/

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

    /* ★段 [0] = 陀螺活性看门狗 */
    c1 = DWT->CYCCNT;
    if ((c1 - t0) > g_seg_cyc_max[0]) g_seg_cyc_max[0] = c1 - t0;

    switch (s_mode) {
    case MOTION_GYRO:
        /* ★2026-10-18：直行差速补偿已移到 GyroControl_SetBaseSpeed() 内部（所有模式共享），
     * 这里不要再加一次，否则会双份补偿。见 gyro_control.c 的说明。 */
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
        /* 任务解释器不走运动模式：它直接调 Toggle_Gyro_Control() 和
         * PID_Tracing_Timer_Start()。旧板这两种“后台环”分别跑在 TIM4(10ms) 和 TIM2(10Hz)
         * 的中断里；新板没有多余的周期定时器，统一在这里补上：
         *   · 角度环（5ms 一拍，gyro_control_enabled 关着时 GyroControl_Task() 自己返回）
         *   · 后台循迹环（内部 /20 分频 = 100ms，保持旧板 TIM2 的采样率）
         * 运动模式（MOTION_GYRO/TRACE/TRACE_GYRO）与任务互斥：进任务前会 Motion_Stop()。 */
        if (GyroControl_IsEnabled()) {
            GyroControl_Task();
        }
        (void)PID_Tracing_BackgroundTask();
        break;
    }

    /* ★段 [1] = 模式段（角度环 / 循迹环 / IDLE 的后台循迹）*/
    c2 = DWT->CYCCNT;
    if ((c2 - c1) > g_seg_cyc_max[1]) g_seg_cyc_max[1] = c2 - c1;

    SpeedControl_Task();    /* 最内层：永远测速；只有使能时才写 PWM */

    /* ★段 [2] = 速度环 */
    {
        uint32_t c3 = DWT->CYCCNT;
        if ((c3 - c2) > g_seg_cyc_max[2]) g_seg_cyc_max[2] = c3 - c2;
    }

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
