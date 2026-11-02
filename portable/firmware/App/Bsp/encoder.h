#ifndef __ENCODER_H__
#define __ENCODER_H__

#include "stm32g4xx_hal.h"

/* 编码器（新板）：
 *   电机 A = TIM3 (PB4=CH1/A相, PB5=CH2/B相)
 *   电机 B = TIM4 (PB6=CH1/A相, PB7=CH2/B相)
 * TI1+TI2 双边沿 = 4 倍频。★2026-10-15：现役电机是 **MG513X（轮趣科技）**，
 *   13 线霍尔 + **1:28** 减速 ⇒ 13 * 4 * 28 = **1456** 计数 / 输出轴圈
 *   （= 参数表 HP_COUNTS_PER_REV）。
 *   旧注释写的是上一代 JGB37-3865-520：11 * 4 * 45 = 1980。
 *
 * 用法：固定周期（速度环 5ms）调 Encoder_ReadDelta()，把两次之间的计数差当转速。
 * 计数在 16 位 CNT 里，只要周期内不越过 32767 就不会溢出（5ms 内最多几十个计数）。 */

void Encoder_Init(void);                        /* 启动 TIM3/TIM4 编码器模式（GPIO/时基由 BSP 配好） */
void Encoder_Reset(void);                       /* 计数器清零 */
int  Read_Speed(TIM_HandleTypeDef *htim);       /* 兼容旧接口：读计数并清零（带符号） */
void Encoder_ReadDelta(int *left, int *right);  /* 本周期左右轮计数增量（带符号，正=车前进方向） */

#endif /* __ENCODER_H__ */
