#include "rgb.h"
#include "buzzer.h"

//#define GPIO_PIN_RESET 1
//#define GPIO_PIN_SET   0

void RGB_on_r(GPIO_PinState x,GPIO_PinState y,GPIO_PinState z)
{
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_0,x);//bule
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_1,y);//red
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_3,z);//green
}
void RGB_on_l(GPIO_PinState x,GPIO_PinState y,GPIO_PinState z)
{
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_9,x);//bule
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_8,y);//red
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_11,z);//green
}
void RGB_off_r(void)
{
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_0,GPIO_PIN_SET);//bule
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_1,GPIO_PIN_SET);//red
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_3,GPIO_PIN_SET);//green
}
void RGB_off_l(void)
{
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_8,GPIO_PIN_SET);//red
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_9,GPIO_PIN_SET);//bule
	HAL_GPIO_WritePin(GPIOD,GPIO_PIN_11,GPIO_PIN_SET);//green
}

void RGB_folg(void)
{
	RGB_blink();
	Buzzer_blink();
}

void RGB_blink(void)
{
	RGB_on_r(1,0,0);
	RGB_on_l(1,0,0);
	Buzzer_blink();
	HAL_Delay(200);
	RGB_on_r(0,1,0);
	RGB_on_l(0,1,0);
	Buzzer_blink();
	HAL_Delay(200);
	RGB_on_r(0,0,1);
	RGB_on_l(0,0,1);
	Buzzer_blink();
	HAL_Delay(200);
}