/**
 * @file  motion.h
 * @brief 运动调度：三层串级的总入口（TIM6 每 5ms 跑一次 Motion_Task）
 *
 *   循迹环  PID_tracing.c   8 路灰度位置误差 -> 目标航向
 *   角度环  gyro_control.c  目标航向        -> 左右轮目标速度
 *   速度环  speed_control.c 目标速度(编码器) -> PWM
 */
#ifndef __MOTION_H__
#define __MOTION_H__

#include <stdint.h>

typedef enum {
    MOTION_IDLE = 0,     /* 不接管电机：PWM 由调用者直接 Load()（模块测试页/手工标定用） */
    MOTION_GYRO,         /* 角度环单独跑：保持/转到目标航向 */
    MOTION_TRACE,        /* 循迹环单独跑：灰度 -> 差速 -> 速度环（不开陀螺仪也能跑） */
    MOTION_TRACE_GYRO    /* 三层串级：灰度 -> 目标航向 -> 角度环 -> 速度环 */
} Motion_Mode;

void        Motion_Init(void);
void        Motion_SetMode(Motion_Mode m);
Motion_Mode Motion_GetMode(void);
void        Motion_SetSpeed(int base_speed);   /* 基准速度（PWM 刻度 0..7200） */
int         Motion_GetSpeed(void);
void        Motion_Stop(void);                 /* = SetMode(MOTION_IDLE) */
void        Motion_Task(void);                 /* TIM6，5ms 一拍 */

/* 运动安全故障：0 = 没有，1 = 陀螺仪失联（>500ms 没有新帧，已自动停车）
 * 只要角度环在跑就会检查（UI 的陀螺模式 + 任务解释器的转弯都算）。
 * 故障位会一直挂着（界面/RTT 看得见），直到重新打开角度环或显式 ClearFault()。 */
#define MOTION_FAULT_NONE        0U
#define MOTION_FAULT_GYRO_STALE  1U
uint8_t     Motion_GetFault(void);
void        Motion_ClearFault(void);      /* 清故障位（开机 / 开始跑任务时用） */

/* ------------------------------------------------------------------
 * 5ms 控制中断的耗时统计（DWT->CYCCNT，170 MHz 时 /170 = 微秒）
 *
 * 为什么要有：控制环跑在 TIM6 中断里，它的耗时决定了"这一拍还剩多少余量"。
 * 主循环那边已经在量 KK_UI 刷新耗时（FRAME 行），控制环一直没有数 ——
 * 出问题时无法区分"环算太慢"还是"环被别的长操作堵住了"（后者真发生过：
 * 擦 Flash 时同 bank 取指停摆，能把 5ms 中断拖几十毫秒）。现在 1Hz 的 RTT
 * MOTION 行会打印 avg/max/over，over = 超过 5ms 预算的拍数，正常情况下必须是 0。
 * ------------------------------------------------------------------ */
#define MOTION_ISR_BUDGET_CYC  (850000UL)      /* 5ms @170MHz */
extern volatile uint32_t g_isr_cyc_last;       /* 上一拍耗时 */
extern volatile uint32_t g_isr_cyc_max;        /* 本报告周期内最大耗时 */
extern volatile uint32_t g_isr_cyc_sum;        /* 累加（报告后清零） */
extern volatile uint32_t g_isr_n;              /* 拍数（报告后清零） */
extern volatile uint32_t g_isr_over;           /* 超预算拍数（累计，不清零） */

#endif /* __MOTION_H__ */
