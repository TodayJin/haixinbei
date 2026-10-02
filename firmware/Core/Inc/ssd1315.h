/**
 * @file  ssd1315.h
 * @brief 裸屏 U16（X096-2864KSWPG01-H30 / SSD1315Z）I2C 驱动
 *
 * 接法见 固件接口规格.md 3.7 节：BS1=3V3、BS2=GND → I2C；A0=GND → 地址 0x3C；
 * D0=SCL、D1+D2=SDA；电荷泵必须用内部（命令 8D 14），VCC(脚28) 不接电源。
 */
#ifndef __SSD1315_H
#define __SSD1315_H

#include "main.h"
#include <stdint.h>

HAL_StatusTypeDef oled_init(void);
HAL_StatusTypeDef oled_flush(void);
void oled_clear(void);
void oled_px(int16_t x, int16_t y, uint8_t on);
void oled_hline(int16_t x0, int16_t x1, int16_t y, uint8_t on);
void oled_vline(int16_t x, int16_t y0, int16_t y1, uint8_t on);
void oled_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on);
void oled_fill_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on);
void oled_pattern_bars(void);      /* 图案1：竖条 + 棋盘，验证每一列 */
void oled_pattern_grid(void);      /* 图案2：网格 + 边框 + 对角线 */
uint8_t *oled_framebuffer(void);

#endif /* __SSD1315_H */
