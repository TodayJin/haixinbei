/**
 * @file  main.c
 * @brief 全地形机器人自检固件 —— 里程碑 2：时钟 + I2C 总线 + INA226
 */
#include "main.h"
#include "board.h"
#include "ssd1315.h"
#include "probe_jy61p.h"
#include "SEGGER_RTT.h"

void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

void selftest_run_all(void);

static void report_reset_cause(uint32_t csr)
{
  SEGGER_RTT_printf(0, "RST: %s%s%s%s%s%s%s (CSR=0x%08lX)\r\n",
    (csr & RCC_CSR_LPWRRSTF) ? "LPWR " : "",
    (csr & RCC_CSR_WWDGRSTF) ? "WWDG " : "",
    (csr & RCC_CSR_IWDGRSTF) ? "IWDG " : "",
    (csr & RCC_CSR_SFTRSTF)  ? "SOFT " : "",
    (csr & RCC_CSR_BORRSTF)  ? "BOR "  : "",
    (csr & RCC_CSR_PINRSTF)  ? "PIN "  : "",
    (csr & RCC_CSR_OBLRSTF)  ? "OBL "  : "",
    (unsigned long)csr);
}

static void report_clock(const clock_report_t *c)
{
  SEGGER_RTT_printf(0, "CLK: HSE=%s PLL=%s HSERDY=%u PLLRDY=%u\r\n",
    c->hse_ok ? "OK(25MHz)" : "FAIL",
    c->pll_used ? "OK(170MHz)" : "NOT USED",
    c->hse_ready, c->pll_locked);
  SEGGER_RTT_printf(0, "CLK: SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu LAT=%lu\r\n",
    (unsigned long)c->sysclk_hz, (unsigned long)c->hclk_hz,
    (unsigned long)c->pclk1_hz, (unsigned long)c->pclk2_hz,
    (unsigned long)c->flash_latency);
  SEGGER_RTT_WriteString(0,
    (c->hse_ok && c->pll_used && c->sysclk_hz == 170000000UL)
      ? "RESULT PASS clk(170MHz + HSE25)\r\n"
      : "RESULT FAIL clk\r\n");
}

int main(void)
{
  uint32_t csr;
  clock_report_t clk;
  uint32_t n = 0;

  csr = RCC->CSR;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  HAL_Init();

  SEGGER_RTT_Init();
  SEGGER_RTT_WriteString(0, "\r\n\r\n");
  SEGGER_RTT_WriteString(0, "========================================\r\n");
  SEGGER_RTT_WriteString(0, " STM32G474RBT6 全地形机器人 自检固件\r\n");
  SEGGER_RTT_WriteString(0, "========================================\r\n");
  SEGGER_RTT_printf(0, "BUILD: %s %s\r\n", __DATE__, __TIME__);
  SEGGER_RTT_printf(0, "DEVID: 0x%08lX\r\n", (unsigned long)DBGMCU->IDCODE);

  report_reset_cause(csr);

  board_clock_init(&clk);
  report_clock(&clk);

#ifdef RESCUE_MODE
  SEGGER_RTT_WriteString(0, "RESCUE MODE: 不初始化任何外设（I2C 等），只跑时钟 + RTT\r\n");
#elif defined(PA15_TEST)
  pa15_test_run();
#else
  selftest_run_all();
#endif

  /* ---- 实时观测：按键 + JY61P ---- */
  board_gpio_keys_init();
  (void)board_uart_jy61_init();
  board_jy61_start_rx();
  jy61p_init_parser();

  SEGGER_RTT_WriteString(0, "\r\n--- live ---\r\n");
  SEGGER_RTT_WriteString(0, "按键：按下即打印，并且会【锁存】到 seen；随时按都算，不用赶时间\r\n");
#ifdef UART_LOOPBACK
  SEGGER_RTT_WriteString(0, "UART 自环：每 2s 在 USART3 发一帧假角度(yaw=90.0)，\r\n");
  SEGGER_RTT_WriteString(0, "           把 U1 的 2 脚(PB10=TX) 与 3 脚(PB11=RX) 短接即可收回\r\n");
#endif
  {
    uint8_t  last_keys;              /* 循环前先读一次，否则会把"空闲"当成"刚按下" */
    uint8_t  seen      = 0;          /* 见过的按键位（锁存） */
    uint16_t cnt[5]    = {0,0,0,0,0};
#ifdef UART_LOOPBACK
    uint32_t lb        = 0;
#endif
    last_keys = keys_raw();
    while (1) {
      uint8_t k, chg;
      const jy61_stat_t *j;
      uint8_t i;

      jy61p_poll();

      k = keys_raw();
      if (k != last_keys) {
        chg   = (uint8_t)((~k) & last_keys & 0x1F);   /* 这次新按下的键（低有效） */
        for (i = 0; i < 5; i++) if (chg & (1u << i)) cnt[i]++;
        seen |= (uint8_t)((~k) & 0x1F);
        SEGGER_RTT_printf(0, "KEYS = 0x%02X  %s%s%s%s%s | seen=0x%02X\r\n", k,
          (k & 0x01) ? "UP " : "", (k & 0x02) ? "DOWN " : "",
          (k & 0x04) ? "LEFT " : "", (k & 0x08) ? "RIGHT " : "",
          (k & 0x10) ? "CENTER " : "", seen);
        last_keys = k;
      }

#ifdef UART_LOOPBACK
      /* 每 2 秒发一帧合成的 JY61P 角度帧（yaw=90.0°），自环时会被自己收到 */
      if (++lb >= 20) {
        /* JY61P 角度帧布局：0x55 0x53 | RollL RollH | PitchL PitchH | YawL YawH | VL VH | Sum
         * 故意发已知值：roll=10.0°, pitch=-20.0°, yaw=90.0°（单位 180/32768 deg） */
        uint8_t f[11];
        int16_t roll  =  1820;   /* +10.0 deg */
        int16_t pitch = -3641;   /* -20.0 deg */
        int16_t yaw   = 16384;   /* +90.0 deg */
        uint8_t s = 0;

        lb = 0;
        f[0] = 0x55; f[1] = 0x53;
        f[2] = (uint8_t)(roll  & 0xFF); f[3] = (uint8_t)((roll  >> 8) & 0xFF);
        f[4] = (uint8_t)(pitch & 0xFF); f[5] = (uint8_t)((pitch >> 8) & 0xFF);
        f[6] = (uint8_t)(yaw   & 0xFF); f[7] = (uint8_t)((yaw   >> 8) & 0xFF);
        f[8] = 0; f[9] = 0;
        for (i = 0; i < 10; i++) s = (uint8_t)(s + f[i]);
        f[10] = s;
        (void)HAL_UART_Transmit(board_jy61_handle(), f, 11, 50);
      }
#endif

      if (++n >= 10) {
        j = jy61p_stats();
        SEGGER_RTT_printf(0,
          "[%6lu ms] KEYS seen=0x%02X cnt=%u/%u/%u/%u/%u | JY61P bytes=%lu frames=%lu bad=%lu resync=%lu | roll=%ld pitch=%ld yaw=%ld (0.1deg)\r\n",
          (unsigned long)HAL_GetTick(), seen,
          cnt[0], cnt[1], cnt[2], cnt[3], cnt[4],
          (unsigned long)j->bytes, (unsigned long)j->frames,
          (unsigned long)j->bad_sum, (unsigned long)j->resync,
          (long)(j->roll * 10.0f), (long)(j->pitch * 10.0f), (long)(j->yaw * 10.0f));
        n = 0;
      }
      HAL_Delay(100);
    }
  }
}
