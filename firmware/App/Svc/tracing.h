#ifndef _TRACING_H
#define _TRACING_H

#include "board.h"
#include "bsp_board.h"
#include "stm32g4xx_hal.h"
#include "main.h"

/* 8 路灰度（第五代智能数字灰度传感器，数字量输出）：
 *   默认电平：压黑线 = 高电平 1（说明书表五/官方例程 D==1 都是这个口径），
 *   所以这里把"压线"归一化成 1；如果你的模块被改成"深色=低电平"，
 *   或者用白线深底，把 trace_invert 置 1（界面上有"循迹反向"设置）。 */
extern int tracing_data[8];
extern uint8_t trace_invert;

int  trace_read_channel(uint8_t idx);   /* 0/1，已按 trace_invert 归一化：1 = 压线 */
int  trace_on_line_count(void);         /* 压线通道数（路口检测用） */

void read_tracing_data(void);
void read_tracing_data_mv(void);   /* 旧板的模拟量版本：本板走数字量 OUT1..OUT8，函数体是空的 */
void show_tracing_data(void);
void motor_tracing(int speed_zhi,int speed_cha);

#endif
