/**
 * @file  selftest.c
 * @brief 硬件自检项：I2C 总线扫描 / INA226（后续：OLED、按键、JY61P）
 *
 * 每项独立打 "RESULT PASS/FAIL"，RTT 里直接看结论。
 */
#include "board.h"
#include "ssd1315.h"
#include "SEGGER_RTT.h"
#include <string.h>

static int s_pass = 0;
static int s_fail = 0;

static void result(const char *name, int ok)
{
  if (ok) s_pass++; else s_fail++;
  SEGGER_RTT_printf(0, "RESULT %-4s %s\r\n", ok ? "PASS" : "FAIL", name);
}

/* =====================================================================
 * 1. I2C1 总线扫描
 * ===================================================================== */
int selftest_i2c_scan(void)
{
  uint8_t addr;
  int found = 0, have_oled = 0, have_ina = 0;
  HAL_StatusTypeDef st;

  SEGGER_RTT_WriteString(0, "\r\n--- TEST: I2C1 bus scan (PA15=SCL, PB9=SDA) ---\r\n");

  st = board_i2c_init();
  SEGGER_RTT_printf(0, "  board_i2c_init() = %d (0=OK)\r\n", (int)st);
  if (st != HAL_OK) {
    result("i2c_init", 0);
    return 0;
  }

  for (addr = 0x08; addr <= 0x77; addr++) {
    if (i2c_probe(addr) == HAL_OK) {
      SEGGER_RTT_printf(0, "  found device @ 0x%02X\r\n", addr);
      found++;
      if (addr == I2C_ADDR_OLED)   have_oled = 1;
      if (addr == I2C_ADDR_INA226) have_ina = 1;
    }
  }

  SEGGER_RTT_printf(0, "  devices found = %d\r\n", found);
  SEGGER_RTT_printf(0, "  OLED  (0x3C) = %s\r\n", have_oled ? "YES" : "no");
  SEGGER_RTT_printf(0, "  INA226(0x40) = %s\r\n", have_ina  ? "YES" : "no");

  if (found == 0) {
    SEGGER_RTT_WriteString(0, "  hint: 总线全无应答 -> 先量 PA15/PB9 空闲电平应为 3.3V；\r\n");
    SEGGER_RTT_WriteString(0, "        再看 R4/R5(4.7k 上拉) 和 U5/U16 的焊接\r\n");
    result("i2c_scan", 0);
  } else if (!have_ina || !have_oled) {
    result("i2c_scan", 0);
  } else {
    result("i2c_scan", 1);
  }
  return found;
}

/* =====================================================================
 * 2. INA226
 * ===================================================================== */
int selftest_ina226(void)
{
  uint16_t mfg = 0, die = 0;
  int32_t bus_mv = 0, shunt_uv = 0, cur_ma = 0;

  SEGGER_RTT_WriteString(0, "\r\n--- TEST: INA226 (addr 0x40, 5mOhm shunt) ---\r\n");

  if (ina226_read_id(&mfg, &die) != HAL_OK) {
    SEGGER_RTT_WriteString(0, "  读 0xFE/0xFF 失败\r\n");
    result("ina226_id", 0);
    return 0;
  }
  SEGGER_RTT_printf(0, "  MFG_ID = 0x%04X  (期望 0x5449 = \"TI\")\r\n", mfg);
  SEGGER_RTT_printf(0, "  DIE_ID = 0x%04X  (期望 0x2260)\r\n", die);

  if (mfg != 0x5449) {
    result("ina226_id", 0);
    return 0;
  }

  (void)ina226_init();
  HAL_Delay(20);
  (void)ina226_read_bus_voltage_mv(&bus_mv);
  (void)ina226_read_shunt_uv(&shunt_uv);
  (void)ina226_read_current_ma(&cur_ma);

  SEGGER_RTT_printf(0, "  VBUS   = %ld mV\r\n", (long)bus_mv);
  SEGGER_RTT_printf(0, "  VSHUNT = %ld uV\r\n", (long)shunt_uv);
  SEGGER_RTT_printf(0, "  I      = %ld mA\r\n", (long)cur_ma);
  SEGGER_RTT_WriteString(0, "  (12V 现在没上，VBUS~0 是正常的；ID 对就说明焊接+I2C 通)\r\n");

  result("ina226", 1);
  return 1;
}

/* =====================================================================
 * 3. OLED 裸屏 U16（SSD1315Z @ 0x3C）
 * ===================================================================== */
int selftest_oled(void)
{
  HAL_StatusTypeDef st;
  int i;

  SEGGER_RTT_WriteString(0, "\r\n--- TEST: OLED U16 (SSD1315 @0x3C) ---\r\n");

  st = oled_init();
  SEGGER_RTT_printf(0, "  oled_init() = %d (0=OK)\r\n", (int)st);
  if (st != HAL_OK) {
    result("oled_init", 0);
    return 0;
  }

  /* 交替显示两个图案各 1.5 s，共 6 s，让人眼确认 */
  for (i = 0; i < 2; i++) {
    oled_pattern_bars();
    if (oled_flush() != HAL_OK) { result("oled_flush", 0); return 0; }
    SEGGER_RTT_WriteString(0, "  图案1（竖条/棋盘）已显示 1.5s\r\n");
    HAL_Delay(1500);

    oled_pattern_grid();
    if (oled_flush() != HAL_OK) { result("oled_flush", 0); return 0; }
    SEGGER_RTT_WriteString(0, "  图案2（边框/对角/十字）已显示 1.5s\r\n");
    HAL_Delay(1500);
  }

  oled_pattern_bars();
  (void)oled_flush();
  SEGGER_RTT_WriteString(0, "  >>> 请肉眼确认屏幕：有图案 = 屏焊接正常\r\n");
  result("oled", 1);
  return 1;
}

/* =====================================================================
 * 汇总入口
 * ===================================================================== */
void selftest_run_all(void)
{
  s_pass = 0;
  s_fail = 0;

  SEGGER_RTT_WriteString(0, "\r\n########## SELFTEST START ##########\r\n");

  (void)selftest_i2c_scan();
  (void)selftest_ina226();
  (void)selftest_oled();

  SEGGER_RTT_printf(0, "\r\n########## SELFTEST DONE: %d PASS / %d FAIL ##########\r\n",
                    s_pass, s_fail);
}
