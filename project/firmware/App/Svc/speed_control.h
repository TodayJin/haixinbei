/**
 * @file  speed_control.h
 * @brief 速度环（三层串级的最内层）：编码器测速 -> 双轮 PI+前馈 -> PWM
 *
 * 目标速度的刻度沿用旧板的 PWM 0..7200（带符号），
 * 这样菜单/上位机里那些已经调好的数不用重标定。
 *
 * 测速：反馈量是「最近 4 拍（20ms）的滑动平均」，用来抑制低速时的计数量化误差，
 * 但**控制节拍仍然是 5ms** —— TIM6 那一拍里同时跑循迹环/角度环/速度环，
 * 不要为了测速精度去改 bsp_board.c 的 BSP_TRACE_PERIOD_MS。
 */
#ifndef __SPEED_CONTROL_H__
#define __SPEED_CONTROL_H__

#include <stdint.h>

void    SpeedControl_Init(void);
void    SpeedControl_Enable(uint8_t on);      /* 使能后 Task 才会写 PWM；未使能只测速不碰电机 */
uint8_t SpeedControl_IsEnabled(void);
void    SpeedControl_SetTarget(int left, int right);                 /* 目标转速（PWM 刻度） */
void    SpeedControl_SetRamp(float ramp_pwm_per_s);                 /* 目标斜坡，0=不限（用 SetTargetRamp 单次设） */
void    SpeedControl_SetTargetRamp(int left, int right, float ramp_pwm_per_s);

/* 前馈参数在线读写：out = slope*target ± fric + PI。
 * 存在 host_link 参数表里（HP_SPEED_FF_SLOPE / HP_SPEED_FF_FRIC），菜单
 * 「PID -> 速度环 -> 前馈斜率/前馈摩擦」改的就是同一份值，所以这三个入口
 * （菜单 / 上位机 / 本接口）行为一致，也都能被 Flash 存档保住。
 * fric 就是摩擦等效压降，是高速精度的关键 —— 台架扫表用这两个接口，不用重烧。 */
void    SpeedControl_SetFeedforward(float slope, float fric);
void    SpeedControl_GetFeedforward(float *slope, float *fric);

void    SpeedControl_Stop(void);              /* 停：清目标/积分，并 Load(0,0) */
void    SpeedControl_Task(void);              /* 每 5ms 调一次（TIM6 中断里的 Motion_Task） */

int     SpeedControl_Counts(int wheel);       /* 0=左 1=右：本拍**原始**增量（计数/5ms，未平滑） */
int     SpeedControl_WindowCounts(int wheel); /* 0=左 1=右：最近 20ms 测速窗的计数增量之和，
                                               * 满速约 100、倒转为负。界面显示"实时速度"用这个
                                               * （界面自己调 Encoder_ReadDelta 会偷走速度环一拍）。 */
int     SpeedControl_SpeedMmps(int wheel);    /* 轮缘线速度 mm/s（带符号） */
int     SpeedControl_Output(int wheel);       /* 本周期实际写下去的 PWM */
int     SpeedControl_Target(int wheel);       /* 本周期实际使用的目标（含斜坡） */

#endif /* __SPEED_CONTROL_H__ */
