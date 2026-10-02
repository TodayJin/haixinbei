/**
 * @file  jy61p.c
 * @brief JY61P 收帧解析（轮询 RDR，不占中断）
 */
#include "probe_jy61p.h"
#include "board.h"
#include <string.h>

static jy61_stat_t s_st;
static uint8_t  s_buf[11];
static uint8_t  s_idx;

void jy61p_init_parser(void)
{
  memset(&s_st, 0, sizeof(s_st));
  s_idx = 0;
}

static void feed(const jy61_stat_t *unused, uint8_t b)
{
  (void)unused;

  if (s_idx == 0 && b != 0x55) { s_st.resync++; return; }
  s_buf[s_idx++] = b;
  if (s_idx < 11) return;

  s_idx = 0;
  {
    uint8_t sum = 0, i;
    for (i = 0; i < 10; i++) sum = (uint8_t)(sum + s_buf[i]);
    if (sum != s_buf[10]) { s_st.bad_sum++; return; }
    s_st.frames++;

    if (s_buf[1] == 0x51) {
      s_st.ax = (float)(int16_t)((s_buf[3] << 8) | s_buf[2]) / 32768.0f * 16.0f;
      s_st.ay = (float)(int16_t)((s_buf[5] << 8) | s_buf[4]) / 32768.0f * 16.0f;
      s_st.az = (float)(int16_t)((s_buf[7] << 8) | s_buf[6]) / 32768.0f * 16.0f;
    } else if (s_buf[1] == 0x53) {
      s_st.roll  = (float)(int16_t)((s_buf[3] << 8) | s_buf[2]) / 32768.0f * 180.0f;
      s_st.pitch = (float)(int16_t)((s_buf[5] << 8) | s_buf[4]) / 32768.0f * 180.0f;
      s_st.yaw   = (float)(int16_t)((s_buf[7] << 8) | s_buf[6]) / 32768.0f * 180.0f;
    }
  }
}

void jy61p_poll(void)
{
  uint8_t b;

  /* 字节由 USART3 中断搬进环形缓冲，这里只负责取 */
  while (board_jy61_get_byte(&b)) {
    s_st.bytes++;
    feed(NULL, b);
  }
}

const jy61_stat_t *jy61p_stats(void) { return &s_st; }
