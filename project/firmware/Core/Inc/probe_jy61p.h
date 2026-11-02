/**
 * @file  jy61p.h
 * @brief JY61P 陀螺仪（USART3, PB10/PB11, 115200）非阻塞收帧解析
 *
 * 帧格式：0x55 + 类型 + 8 数据 + 校验和
 *   0x51 加速度  0x52 角速度  0x53 角度  0x54 磁场
 * 校验 = 前 10 字节之和 & 0xFF
 */
#ifndef __JY61P_H
#define __JY61P_H

#include "main.h"
#include <stdint.h>

typedef struct {
  uint32_t bytes;         /* 收到的字节总数 */
  uint32_t frames;        /* 校验通过的帧数 */
  uint32_t bad_sum;       /* 校验失败的帧数 */
  uint32_t resync;        /* 丢弃的字节数（找同步头时丢的） */
  float    roll, pitch, yaw;
  float    ax, ay, az;
} jy61_stat_t;

void jy61p_init_parser(void);
void jy61p_poll(void);                    /* 主循环里反复调用 */
const jy61_stat_t *jy61p_stats(void);

#endif
