#ifndef __PID_TRACING_H
#define __PID_TRACING_H
#include "main.h"

void read_tracing_data_pid(void);
void show_tracing_data_pid(void);
void motor_tracing_pid(float target);
void motor_tracing_pid_0(float target);
void motor_tracing_pid_0_task(void);

// 定时器2控制函数
void PID_Tracing_Timer_Start(void);
void PID_Tracing_Timer_Stop(void);

extern TIM_HandleTypeDef htim2;

#endif