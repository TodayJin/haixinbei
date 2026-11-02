#ifndef _MOTOR_H
#define _MOTOR_H

#include "stm32f1xx_hal.h"

void Load(int moto1,int moto2);
void Limit(int *motoA,int *motoB);
void Set_Pwm(int mode,uint16_t left_speed,uint16_t right_speed);
void Motor_Init(void);
#endif
