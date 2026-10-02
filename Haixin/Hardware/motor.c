#include "motor.h"

#define PWM_MAX 7200
#define PWM_MIN -7200

extern TIM_HandleTypeDef htim8;
extern uint8_t stop;

int abs(int p)
{
	if(p>0)
		return p;
	else
		return -p;
}

void Motor_Init(void)
{
	HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_1); 
	HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_2); 
}

void Load(int moto1,int moto2)			
{
	if(moto1<0)
	{
		HAL_GPIO_WritePin(GPIOD,GPIO_PIN_6,GPIO_PIN_SET);
		HAL_GPIO_WritePin(GPIOD,GPIO_PIN_7,GPIO_PIN_RESET);
	}
	else
	{
		HAL_GPIO_WritePin(GPIOD,GPIO_PIN_6,GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOD,GPIO_PIN_7,GPIO_PIN_SET);
	}
	__HAL_TIM_SetCompare(&htim8,TIM_CHANNEL_1,abs(moto1));
	if(moto2>0)
	{
		HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_SET);
		HAL_GPIO_WritePin(GPIOB,GPIO_PIN_4,GPIO_PIN_RESET);
	}
	else
	{
		HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOB,GPIO_PIN_4,GPIO_PIN_SET);
	}
	__HAL_TIM_SetCompare(&htim8,TIM_CHANNEL_2,abs(moto2));
}



void Limit(int *motoA,int *motoB)
{
	if(*motoA>PWM_MAX)*motoA=PWM_MAX;
	if(*motoA<PWM_MIN)*motoA=PWM_MIN;
	if(*motoB>PWM_MAX)*motoB=PWM_MAX;
	if(*motoB<PWM_MIN)*motoB=PWM_MIN;
}

void Set_Pwm(int mode,uint16_t left_speed,uint16_t right_speed)
{
	switch(mode)
	{
		case 1://前进
			Load(left_speed,right_speed);
			
			break;
		case 2://后退
			Load(-left_speed,-right_speed);
			
			break;
		case 3://左转
			Load(-left_speed,right_speed);
			
			break;
		case 4://右转
			Load(left_speed,-right_speed);
			
			break;
		case 5://停止
			Load(0,0);
			
			break;
	}
}
