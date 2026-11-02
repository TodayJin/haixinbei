/**
 * @file  bsp_board.h
 * @brief 新板 BSP：把旧固件的硬件访问点映射到 STM32G474 主板
 *
 * 原则：**上层算法（tracing/PID/gyro/任务解释器）一行不改**，只在这里换硬件。
 * 数值刻度刻意与旧板保持一致（PWM 上限仍为 7200、舵机仍为 1us 刻度），
 * 这样上层里那些调好的 PID 参数不用重标定。
 */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "main.h"
#include <stdint.h>

/* ---------------- 定时器句柄（旧代码 extern htim2/htim4/htim8/htim1）---------------- */
extern TIM_HandleTypeDef htim1;   /* 舵机 50Hz，PA8–PA11 = TIM1_CH1–4 */
extern TIM_HandleTypeDef htim8;   /* 电机 PWM，PC6–PC9 = TIM8_CH1–4  */
extern TIM_HandleTypeDef htim6;   /* 循迹 PID 周期（替代旧 TIM2）    */
extern TIM_HandleTypeDef htim7;   /* 陀螺仪任务周期（替代旧 TIM4）   */

/* 旧代码里用 htim2/htim4 做周期中断，这里给个别名，避免改上层 */
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim4;

/* ---------------- 电机（2×AT8236，每电机两根输入，无独立方向脚）----------------
 * 旧板：IN1=PWM + 方向脚。新板：IN1/IN2 都是 PWM 输入。
 * 约定：正速度 -> IN1 出 PWM、IN2 恒低；负速度 -> IN1 恒低、IN2 出 PWM。 */
#define MOTOR_PWM_MAX   7200
#define MOTOR_PWM_MIN  (-7200)

/* ---------------- 舵机（旧板 CH4/CH3 -> 新板 CH1/CH2）---------------- */
#define SERVO1_CHANNEL  TIM_CHANNEL_1
#define SERVO2_CHANNEL  TIM_CHANNEL_2

/* ---------------- 编码器（新板）：TIM3 = 电机A，TIM4 = 电机B ----------------
 * 为什么单独叫 htim4_enc：旧上层 extern 的 htim4 在本工程里已经是 TIM7 的别名。 */
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4_enc;
void        bsp_encoder_init(void);

/* ---------------- 循迹 8 路：新板 PA0–PA7 = huidu1–8 ---------------- */
void        bsp_trace_init(void);
GPIO_PinState bsp_trace_bit(uint8_t idx);   /* 0..7 -> huidu1..8 */

/* ---------------- BSP 生命周期 ---------------- */
void bsp_motor_init(void);
void bsp_servo_init(void);
void bsp_periodic_start(void);   /* 启动 TIM6/TIM7 周期中断 */

#endif /* BSP_BOARD_H */
