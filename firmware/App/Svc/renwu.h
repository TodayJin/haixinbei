#ifndef __RENWU_H__
#define __RENWU_H__

#include "stm32g4xx_hal.h"

extern uint8_t renwu_flag;
extern uint8_t black_num;
void renwu(void);
void renwu_stop(void);   /* 任务被外部停掉：关角度环 + 断电机 */
void A(void);
void B(void);
void C(void);

#endif
