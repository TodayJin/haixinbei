#ifndef __PID_H__
#define __PID_H__

#include "stm32f1xx_hal.h"
#include "main.h"

float PID_Compute(float setpoint, float current_value);

#endif
