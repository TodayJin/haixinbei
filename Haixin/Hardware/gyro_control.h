// gyro_control.h
#ifndef __GYRO_CONTROL_H
#define __GYRO_CONTROL_H

#include "main.h"
#include "jy61p.h"

// PID控制器实现
extern PID_Controller gyro_pid;

// 陀螺仪保持控制函数
void GyroControl_Init(void);
void GyroControl_Start(void);
void GyroControl_Stop(void);
void GyroControl_SetBaseSpeed(int left, int right);
void GyroControl_Task(void);


void GyroControl_TurnRightFixed(void);
void GyroControl_TurnLeftFixed(void);

void GyroControl_TurnRight(float angle);
void GyroControl_TurnLeft(float angle);
// 定时器控制函数
void Gyro_Timer_Start(void);
void Gyro_Timer_Stop(void);

#endif