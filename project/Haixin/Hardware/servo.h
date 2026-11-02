#ifndef __SERVO_H__
#define __SERVO_H__

#include "stm32f1xx_hal.h"
#include "main.h"
extern TIM_HandleTypeDef htim1;

void Servo_Init(void);
void Servo_Control(int x, uint8_t angle);
void Servo_Move(int x);

#endif