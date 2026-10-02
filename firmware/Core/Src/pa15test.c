/**
 * @file  pa15test.c
 * @brief PA15 判决实验：分清"PA15 配置/活动导致 J-Link 连不上"还是"PA15 与 SWCLK 连锡"
 *
 * -DPA15_TEST=2 : 只把 PA15 配成 I2C1_SCL（静态、不做任何总线传输）20 s，然后释放
 * -DPA15_TEST=3 : PA15 配成开漏 GPIO 输出，50 Hz 翻转 20 s，然后释放（能安全拖低 SWCLK）
 *
 * 两段都是"20 s 活动 + 之后释放回输入"，所以就算把调试口搞挂了，
 * 20 s 后也会自己恢复，不需要按复位键。
 */
#include "board.h"
#include "SEGGER_RTT.h"

#ifdef PA15_TEST

void pa15_test_run(void)
{
  int i;

  SEGGER_RTT_WriteString(0, "\r\n########## PA15 判决实验 ##########\r\n");

#if PA15_TEST == 2
  SEGGER_RTT_WriteString(0, "MODE 2: PA15 = I2C1_SCL，静态（不发任何数据）\r\n");
  SEGGER_RTT_WriteString(0, "        若这 20 s 内 J-Link 连不上 -> 静态配置就足以搞挂调试口\r\n");
  SEGGER_RTT_WriteString(0, "        若一直能连 -> 静态没问题，是「总线活动」搞挂的\r\n");
  (void)board_i2c_init();
  for (i = 0; i < 20; i++) {
    HAL_Delay(1000);
    SEGGER_RTT_printf(0, "  [A] static-hold %d/20 s  (PA15=SCL idle)\r\n", i + 1);
  }
  board_pa15_release();
  SEGGER_RTT_WriteString(0, "  [A] PA15 已释放为输入 -> 现在应该能连\r\n");
#elif PA15_TEST == 3
  SEGGER_RTT_WriteString(0, "MODE 3: PA15 = 开漏 GPIO，50 Hz 翻转\r\n");
  SEGGER_RTT_WriteString(0, "        若这 20 s 内连不上 -> PA15 与 SWCLK(PA14) 之间存在物理短路\r\n");
  SEGGER_RTT_WriteString(0, "        若一直能连 -> 没有短路，是 I2C 外设本身的问题\r\n");
  for (i = 0; i < 20; i++) {
    board_pa15_od(0);
    HAL_Delay(10);
    board_pa15_od(1);
    HAL_Delay(10);
    if ((i % 5) == 0) {
      SEGGER_RTT_printf(0, "  [B] toggle %d/20 s\r\n", i);
    }
  }
  board_pa15_release();
  SEGGER_RTT_WriteString(0, "  [B] PA15 已释放为输入 -> 现在应该能连\r\n");
#elif PA15_TEST == 4
  {
    extern void selftest_run_all(void);
    SEGGER_RTT_WriteString(0, "MODE 4: 复现原故障 —— 跑完整 I2C 扫描，扫描完立刻释放 PA15\r\n");
    SEGGER_RTT_WriteString(0, "        扫描期间连不上 = 复现成功；释放后又能连 = 就是 PA15 相关\r\n");
    selftest_run_all();
    board_pa15_release();
    SEGGER_RTT_WriteString(0, "  [C] PA15 已释放为输入 -> 现在应该能连\r\n");
  }
#else
  SEGGER_RTT_WriteString(0, "MODE ?: 未选择\r\n");
#endif

  SEGGER_RTT_WriteString(0, "########## PA15 判决实验结束 ##########\r\n");
}

#endif /* PA15_TEST */
