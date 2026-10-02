#include "buzzer.h"
void Buzzer_On(void)
{
	HAL_GPIO_WritePin(BUZZER_PORT,BUZZER_PIN,GPIO_PIN_SET);
}

void Buzzer_Off(void)
{
  HAL_GPIO_WritePin(BUZZER_PORT,BUZZER_PIN,GPIO_PIN_RESET);
}
void Buzzer_blink(void)
{
	Buzzer_On();
	HAL_Delay(200);
	Buzzer_Off();
}
