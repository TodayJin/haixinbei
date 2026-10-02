#ifndef __RGB_H__
#define __RGB_H__

#include "stm32g4xx_hal.h"
void RGB_on_r(GPIO_PinState x,GPIO_PinState y,GPIO_PinState z);
void RGB_on_l(GPIO_PinState x,GPIO_PinState y,GPIO_PinState z);
void RGB_off_r(void);
void RGB_off_l(void);
void RGB_blink(void);
void RGB_folg(void);
#endif
