#ifndef __PID_H__
#define __PID_H__

/* 纯算法模块：不依赖 HAL（原来 include 了 stm32g4xx_hal.h/main.h，
 * 既没必要，也让这份代码没法在 PC 上做单元测试）。 */

float PID_Compute(float setpoint, float current_value);
void  PID_Reset(void);   /* 清零积分与上次误差（丢线找回 / 重新起步时用） */
/* 切换"原地 pivot / 前进循迹"两组增益。两者是同一个 PID，但被控对象完全不同：
 * 前进时探头以车速扫过线，原地转时同一个重心偏差对应的车身转角大得多。
 * 只在模式真的变了的时候清积分（由 pid.c 负责判断）。 */
void  PID_SetPivot(int on);

#endif
