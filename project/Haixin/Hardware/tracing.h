#ifndef __TRACING_H__
#define __TRACING_H__

#include "stm32f1xx_hal.h"
#include "main.h"

extern int tracing_data[8];

void read_tracing_data(void);
void show_tracing_data(void);
void motor_tracing(int speed_zhi,int speed_cha);
//void motor_tracing_mv(int speed_zhi,int speed_cha);
//void read_tracing_data_mv(void);
#endif
