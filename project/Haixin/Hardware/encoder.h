#ifndef __ENCODER_H__
#define __ENCODER_H__

#include "stm32f1xx_hal.h"
int Read_Speed(TIM_HandleTypeDef *htim);
void Encoder_Init(void);

#endif
