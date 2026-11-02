/**
 * @file  ssd1315.c
 * @brief 裸屏 U16（SSD1315Z）I2C 驱动 + 自检图案
 *
 * I2C 格式：START | 0x3C<<1 | 控制字节 | 数据 | STOP
 *   控制字节 0x00 = 后面全是命令
 *   控制字节 0x40 = 后面全是显存数据
 * 显存 128x64，按 8 页 x 128 列，bit0 在最上面。
 */
#include "ssd1315.h"
#include "board.h"
#include "SEGGER_RTT.h"
#include <string.h>

#define OLED_ADDR   0x3C
#define FB_SIZE     (128 * 8)

static uint8_t s_fb[FB_SIZE];

uint8_t *oled_framebuffer(void) { return s_fb; }

/* ---------------- 底层 ---------------- */
static HAL_StatusTypeDef oled_cmd(const uint8_t *cmds, uint16_t n)
{
  uint8_t buf[33];
  if (n > sizeof(buf) - 1) return HAL_ERROR;
  buf[0] = 0x00;                       /* 控制字节：命令 */
  memcpy(&buf[1], cmds, n);
  return board_i2c_write(OLED_ADDR, buf, (uint16_t)(n + 1));
}

static HAL_StatusTypeDef oled_data(const uint8_t *d, uint16_t n)
{
  uint8_t buf[129];
  if (n > sizeof(buf) - 1) return HAL_ERROR;
  buf[0] = 0x40;                       /* 控制字节：数据 */
  memcpy(&buf[1], d, n);
  return board_i2c_write(OLED_ADDR, buf, (uint16_t)(n + 1));
}

/* ---------------- 初始化（屏厂 GME12864-41 参考代码） ----------------
 * 注意 8D 14 = 开内部电荷泵（本板只有 3.3V，必须开）
 *      AD 30 = 用内部 IREF（所以脚 26 悬空是对的）
 */
HAL_StatusTypeDef oled_init(void)
{
  static const uint8_t seq[] = {
    0xAE,              /* 关显示 */
    0x81, 0xCF,        /* 对比度 */
    0xA4,              /* 按显存显示 */
    0xA6,              /* 正常显示（非反白） */
    0x00, 0x10,        /* 列地址低/高 4 位归零 */
    0x20, 0x02,        /* 寻址模式 = 页寻址 */
    0xB0,              /* 起始页 0 */
    0x40,              /* 显示起始行 0 */
    0xA0,              /* 列地址映射：左->右 */
    0xA8, 0x3F,        /* multiplex = 63 (1/64 duty) */
    0xC0,              /* COM 扫描方向 */
    0xD3, 0x00,        /* COM 引脚配置 */
    0xDA, 0x12,        /* COM 引脚：交替 + 不左右反相 */
    0xD5, 0x80,        /* 时钟分频 */
    0xD9, 0x22,        /* 预充电周期 */
    0xDB, 0x40,        /* VCOMH 反压 */
    0x8D, 0x14,        /* ★ 启用内部电荷泵（VCC = 7.5V） */
    0xAD, 0x30,        /* ★ 内部 IREF，ISEG = 30uA */
    0xAF               /* 开显示 */
  };
  HAL_StatusTypeDef st;
  uint16_t i;

  oled_clear();

  /* 一条一条发，出错立刻停，便于定位是哪条挂了 */
  for (i = 0; i < sizeof(seq); i++) {
    st = oled_cmd(&seq[i], 1);
    if (st != HAL_OK) {
      SEGGER_RTT_printf(0, "  oled_init: 命令 0x%02X 失败 (st=%d)\r\n", seq[i], (int)st);
      return st;
    }
  }
  return oled_flush();
}

HAL_StatusTypeDef oled_flush(void)
{
  uint8_t page, c[3];

  for (page = 0; page < 8; page++) {
    c[0] = (uint8_t)(0xB0 | page);   /* 页地址 */
    c[1] = 0x00;                     /* 列低 4 位 */
    c[2] = 0x10;                     /* 列高 4 位 */
    if (oled_cmd(c, 3) != HAL_OK) return HAL_ERROR;
    if (oled_data(&s_fb[page * 128], 128) != HAL_OK) return HAL_ERROR;
  }
  return HAL_OK;
}

/* ---------------- 画点 ---------------- */
void oled_clear(void) { memset(s_fb, 0, FB_SIZE); }

void oled_px(int16_t x, int16_t y, uint8_t on)
{
  if (x < 0 || x > 127 || y < 0 || y > 63) return;
  if (on) s_fb[(y >> 3) * 128 + x] |=  (uint8_t)(1u << (y & 7));
  else    s_fb[(y >> 3) * 128 + x] &= (uint8_t)~(1u << (y & 7));
}

void oled_hline(int16_t x0, int16_t x1, int16_t y, uint8_t on)
{
  int16_t x;
  if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
  for (x = x0; x <= x1; x++) oled_px(x, y, on);
}

void oled_vline(int16_t x, int16_t y0, int16_t y1, uint8_t on)
{
  int16_t y;
  if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
  for (y = y0; y <= y1; y++) oled_px(x, y, on);
}

void oled_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on)
{
  oled_hline(x0, x1, y0, on);
  oled_hline(x0, x1, y1, on);
  oled_vline(x0, y0, y1, on);
  oled_vline(x1, y0, y1, on);
}

void oled_fill_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on)
{
  int16_t x, y;
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++) oled_px(x, y, on);
}

/* ---------------- 自检图案 ---------------- */

/* 图案1：左边 8 条 1px 竖线（验证每一列都能画）+ 右边棋盘（验证像素）
 * 中间留个"全亮/全灭"交替块，一眼看出屏是不是在刷新 */
void oled_pattern_bars(void)
{
  int i;

  oled_clear();

  /* 顶部：1px 间隔竖条，x=0..63，每隔 2 列一条 */
  for (i = 0; i < 64; i += 2) oled_vline(i, 0, 15, 1);

  /* 左中：8 条 1px 竖线，验证相邻列没串 */
  for (i = 0; i < 8; i++) oled_vline((int16_t)(i * 8), 20, 43, 1);

  /* 右中：4x4 棋盘 */
  for (i = 0; i < 8; i++) {
    int16_t x0 = (int16_t)(64 + (i % 4) * 16);
    int16_t y0 = (int16_t)(20 + (i / 4) * 12);
    oled_fill_rect(x0, y0, (int16_t)(x0 + 15), (int16_t)(y0 + 11), (uint8_t)(i & 1));
  }

  /* 底部：整行亮/交替，验证每一页 */
  for (i = 0; i < 128; i++) {
    oled_px(i, 48, (uint8_t)(i & 1));
    oled_px(i, 52, 1);
    oled_px(i, 56, (uint8_t)((i / 8) & 1));
    oled_px(i, 60, 1);
  }

  /* 四角标记：确认方向和边界 */
  oled_fill_rect(0, 0, 3, 3, 1);
  oled_fill_rect(124, 0, 127, 3, 1);
  oled_fill_rect(0, 60, 3, 63, 1);
  oled_fill_rect(124, 60, 127, 63, 1);
}

/* 图案2：边框 + 对角线 + 中间十字，方便肉眼看有没有整行/整列不亮 */
void oled_pattern_grid(void)
{
  int i;

  oled_clear();
  oled_rect(0, 0, 127, 63, 1);
  oled_rect(1, 1, 126, 62, 1);

  for (i = 0; i < 128; i++) {
    oled_px(i, (int16_t)(i >> 1), 1);
    oled_px(i, (int16_t)(63 - (i >> 1)), 1);
  }
  oled_hline(0, 127, 32, 1);
  oled_vline(64, 0, 63, 1);
  oled_fill_rect(58, 26, 70, 38, 0);
  oled_rect(58, 26, 70, 38, 1);
}
