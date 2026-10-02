#ifndef __JY61P_H
#define __JY61P_H

#include "main.h" 

// PID控制器结构体
typedef struct {
    float kp;           // 比例系数
    float ki;           // 积分系数
    float kd;           // 微分系数
    float target;       // 目标值
    float actual;       // 实际值
    float error;        // 当前误差
    float last_error;   // 上次误差
    float integral;     // 积分项
    float output;       // 输出值
} PID_Controller;

void jy61p_ReceiveData(uint8_t RxData);

extern float Roll,Pitch,Yaw;
extern float Gyro_Z;  // Z轴角速度变量

// PID相关函数
void PID_Init(PID_Controller* pid, float kp, float ki, float kd);
float PID_Compute_z(PID_Controller* pid, float target, float actual, float dt);

#endif