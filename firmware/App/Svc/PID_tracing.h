#ifndef _PID_TRACING_H
#define _PID_TRACING_H

#include "board.h"
#include "bsp_board.h"
#include "main.h"

void read_tracing_data_pid(void);
void show_tracing_data_pid(void);

/* 循迹环（差速直出）：灰度位置误差 PID -> 左右轮速度差 */
void motor_tracing_pid(float target);
void motor_tracing_pid_0(float target);
void motor_tracing_pid_0_task(void);
void motor_tracing_pid_speed(int base_speed);   /* 用参数表里的循迹 PID 参数，base_speed 为基准速度 */

/* 三层串级的外两层：灰度误差 -> 目标航向 -> 角度环 -> 左右轮目标速度 */
void motor_tracing_cascade(int base_speed);
void motor_tracing_reset_cascade(void);

/* 三级串级的外环标定量（重心偏 1 格 -> 目标航向偏多少度 / 目标航向偏移上限）
 * 出厂 10°/格、35°；台架实测几何是 1 格 ≈ 5~7° 车身角 ⇒ 需要重标，故做成可写变量，
 * 用 SWD 或上位机在线调，不用重编烧录。写进来的值会夹到安全范围（0.5~60 / 1~90）。*/
void PID_Tracing_SetCascadeGains(float deg_per_unit, float aim_max_deg);
void PID_Tracing_GetCascadeGains(float *deg_per_unit, float *aim_max_deg);

/* 路口检测（说明书第七章：N 路灯同时压线 + 消抖） */
uint8_t tracing_cross(void);
void    tracing_cross_clear(void);

/* 边缘外推 + 位置中值滤波的可见状态（只给台架/真车看，不参与控制）
 *   trace_bar_w        跟踪到的黑带宽度，单位"格"（一个传感器间距）= 线宽
 *   trace_pos_jit_raw  Σ|本帧重心 - 上帧重心|（滤波前）；均值 = 它 / trace_pos_frames
 *   trace_pos_jit_filt 同上（滤波后）：这两个的比值就是中值滤波压掉了多少抖动
 *   trace_edge_cnt     累计触发边缘外推的帧数（真车跑完看它还是 0 = 线从没跑出阵，这条没用上）*/
extern float    trace_bar_w;
extern float    trace_pos_jit_raw;
extern float    trace_pos_jit_filt;
extern uint32_t trace_pos_frames;
extern uint32_t trace_edge_cnt;
extern uint8_t  trace_edge_extrap;

/* 循迹周期定时器（旧板 TIM2 的 10Hz 中断；新板改成软件开关，由 Motion_Task 按 5ms/20 分频执行） */
void PID_Tracing_Timer_Start(void);
void PID_Tracing_Timer_Stop(void);

/* 后台循迹环：任务流程里写的是 PID_Tracing_Timer_Start(); HAL_Delay(N); Stop();
 * 旧板靠 TIM2 中断在后台跑 motor_tracing_pid_0_task()（10Hz）。新板由 Motion_Task()（TIM6 5ms）
 * 每 PID_TRACE_BG_DIV 拍调一次，保持旧板的采样率。返回 1 表示这一拍真的跑了循迹。 */
#define PID_TRACE_BG_DIV 20U               /* 5ms × 20 = 100ms = 10Hz（旧板 TIM2：PSC=7199/ARR=999 @72MHz） */
uint8_t PID_Tracing_BackgroundOn(void);
uint8_t PID_Tracing_BackgroundTask(void);

extern TIM_HandleTypeDef htim2;

#endif
