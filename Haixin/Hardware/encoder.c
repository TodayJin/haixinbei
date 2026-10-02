#include "encoder.h"

//extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim2;

//void Encoder_Init(void)
//{
//	HAL_TIM_Encoder_Start(&htim3,TIM_CHANNEL_ALL);
//	HAL_TIM_Encoder_Start(&htim2,TIM_CHANNEL_ALL);
//}

//int Read_Speed(TIM_HandleTypeDef *htim)
//{
//	int temp;
//	temp=(short)__HAL_TIM_GetCounter(htim);
//	__HAL_TIM_SetCounter(htim,0);
//	return temp;
//}
