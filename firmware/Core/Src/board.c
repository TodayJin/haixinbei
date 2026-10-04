/**
 * @file  board.c
 * @brief 全地形机器人自检固件 —— 板级初始化与外设小驱动
 */
#include "board.h"
#include "bus_health.h"
#include "SEGGER_RTT.h"
#include <string.h>

static I2C_HandleTypeDef  hi2c1;
static UART_HandleTypeDef hjy61;     /* USART3  -> JY61P */
static UART_HandleTypeDef hdl;       /* LPUART1 -> 无线下载器 */

/* =====================================================================
 *  时钟
 * ===================================================================== */
void board_clock_init(clock_report_t *rep)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  memset(rep, 0, sizeof(*rep));

  __HAL_RCC_PWR_CLK_ENABLE();
  (void)HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /* --- 先试 HSE 25MHz + PLL /5 ×68 /2 = 170MHz --- */
  osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState            = RCC_HSE_ON;
  osc.PLL.PLLState        = RCC_PLL_ON;
  osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM            = RCC_PLLM_DIV5;
  osc.PLL.PLLN            = 68;
  osc.PLL.PLLP            = RCC_PLLP_DIV2;
  osc.PLL.PLLQ            = RCC_PLLQ_DIV2;
  osc.PLL.PLLR            = RCC_PLLR_DIV2;

  if (HAL_RCC_OscConfig(&osc) == HAL_OK) {
    rep->hse_ok = 1;
    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) == HAL_OK) {
      rep->pll_used = 1;
    }
  }

  /* --- HSE 挂了就退回 HSI16，保证还能往主机报错 --- */
  if (!rep->pll_used) {
    RCC_OscInitTypeDef fb = {0};
    fb.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    fb.HSIState       = RCC_HSI_ON;
    fb.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    fb.PLL.PLLState   = RCC_PLL_NONE;
    (void)HAL_RCC_OscConfig(&fb);
    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0);
  }

  HAL_RCC_GetClockConfig(&clk, &rep->flash_latency);
  rep->sysclk_hz  = HAL_RCC_GetSysClockFreq();
  rep->hclk_hz    = HAL_RCC_GetHCLKFreq();
  rep->pclk1_hz   = HAL_RCC_GetPCLK1Freq();
  rep->pclk2_hz   = HAL_RCC_GetPCLK2Freq();
  rep->hse_ready  = (RCC->CR & RCC_CR_HSERDY) ? 1u : 0u;
  rep->hsi_ready  = (RCC->CR & RCC_CR_HSIRDY) ? 1u : 0u;
  rep->pll_locked = (RCC->CR & RCC_CR_PLLRDY) ? 1u : 0u;
}

/* =====================================================================
 *  GPIO
 * ===================================================================== */
void board_gpio_keys_init(void)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  io.Mode  = GPIO_MODE_INPUT;
  io.Pull  = GPIO_PULLUP;
  io.Speed = GPIO_SPEED_FREQ_LOW;

  io.Pin = KEY_UP_PIN | KEY_DOWN_PIN | KEY_LEFT_PIN;
  HAL_GPIO_Init(GPIOC, &io);

  io.Pin = KEY_RIGHT_PIN | KEY_CENTER_PIN;
  HAL_GPIO_Init(GPIOC, &io);
}

void board_buzzer_init(void)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();
  HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
  io.Pin   = BUZZER_PIN;
  io.Mode  = GPIO_MODE_OUTPUT_PP;
  io.Pull  = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BUZZER_PORT, &io);
}

uint8_t keys_raw(void)
{
  uint8_t b = 0;
  /* 低有效：按下 = 0 */
  if (HAL_GPIO_ReadPin(KEY_UP_PORT,     KEY_UP_PIN)     == GPIO_PIN_RESET) b |= 1u << 0;
  if (HAL_GPIO_ReadPin(KEY_DOWN_PORT,   KEY_DOWN_PIN)   == GPIO_PIN_RESET) b |= 1u << 1;
  if (HAL_GPIO_ReadPin(KEY_LEFT_PORT,   KEY_LEFT_PIN)   == GPIO_PIN_RESET) b |= 1u << 2;
  if (HAL_GPIO_ReadPin(KEY_RIGHT_PORT,  KEY_RIGHT_PIN)  == GPIO_PIN_RESET) b |= 1u << 3;
  if (HAL_GPIO_ReadPin(KEY_CENTER_PORT, KEY_CENTER_PIN) == GPIO_PIN_RESET) b |= 1u << 4;
  return b;
}

/* 把 PA15 还原成复位默认状态（输入 + 上拉，即 JTDI），用于"释放"它 */
void board_pa15_release(void)
{
  GPIO_InitTypeDef io = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  io.Pin   = GPIO_PIN_15;
  io.Mode  = GPIO_MODE_INPUT;
  io.Pull  = GPIO_PULLUP;
  io.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &io);
}

/* 把 PA15 配成开漏 GPIO 输出并给定电平（用于验证是否与 SWCLK 短路） */
void board_pa15_od(uint8_t level)
{
  GPIO_InitTypeDef io = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  io.Pin   = GPIO_PIN_15;
  io.Mode  = GPIO_MODE_OUTPUT_OD;
  io.Pull  = GPIO_NOPULL;
  io.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &io);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* =====================================================================
 *  I2C1 —— PA15(SCL) / PB9(SDA)，开漏 + 板上 4.7k 上拉
 * ===================================================================== */
/**
 * @brief 按目标 SCL 频率算 I2C_TIMINGR。
 *        周期分配按 t_LOW = 0.52/f、t_HIGH = 0.48/f：
 *          100kHz -> 5.2us / 4.8us（都高于标准模式 4.7us / 4.0us 下限）
 *          400kHz -> 1.3us / 1.2us（都高于快速模式 1.3us / 0.6us 下限）
 */
static uint32_t i2c_timing(uint32_t pclk, uint32_t scl_hz)
{
  uint32_t presc, tick_ns;
  uint32_t t_low_ns, t_high_ns;
  uint32_t scll, sclh, scldel, sdadel;

  if (scl_hz == 0u) scl_hz = 100000u;
  t_low_ns  = (uint32_t)(520000000ULL / scl_hz);
  t_high_ns = (uint32_t)(480000000ULL / scl_hz);

  for (presc = 0; presc < 16; presc++) {
    tick_ns = (uint32_t)(((uint64_t)(presc + 1) * 1000000000ULL) / pclk);
    if (tick_ns >= 80u) break;
  }
  if (presc > 15) { presc = 15; tick_ns = (uint32_t)(16ULL * 1000000000ULL / pclk); }

  /* 全部向上取整，保证不违反 I2C 最小时间 */
  scll   = (t_low_ns  + tick_ns - 1) / tick_ns; if (scll)   scll--;   if (scll   > 255) scll = 255;
  sclh   = (t_high_ns + tick_ns - 1) / tick_ns; if (sclh)   sclh--;   if (sclh   > 255) sclh = 255;
  scldel = ( 250u + tick_ns - 1) / tick_ns;     if (scldel) scldel--; if (scldel >  15) scldel = 15;
  sdadel = ( 300u + tick_ns - 1) / tick_ns;     if (sdadel) sdadel--; if (sdadel >  15) sdadel = 15;

  return (presc << 28) | (scldel << 24) | (sdadel << 20) | (sclh << 8) | scll;
}

/* 当前配置的 I2C1 速率（Hz）。board_i2c_init_hz() 每次成功都记下来，
 * 唯一用途是让 board_i2c_bus_recover() 恢复后回到**同一个速率**（见那里的注释）。
 * 默认值就是主程序要的 400kHz。 */
static uint32_t s_i2c_scl_hz = 400000U;

HAL_StatusTypeDef board_i2c_init_hz(uint32_t scl_hz)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_I2C1_CLK_ENABLE();

  io.Pin   = I2C1_SCL_PIN;
  io.Mode  = GPIO_MODE_AF_OD;
  io.Pull  = GPIO_PULLUP;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(I2C1_SCL_PORT, &io);

  io.Pin   = I2C1_SDA_PIN;
  io.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(I2C1_SDA_PORT, &io);

  hi2c1.Instance             = I2C1;
  hi2c1.Init.Timing          = i2c_timing(HAL_RCC_GetPCLK1Freq(), scl_hz);
  hi2c1.Init.OwnAddress1     = 0;
  hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2     = 0;
  hi2c1.Init.OwnAddress2Masks= I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK) return HAL_ERROR;

  s_i2c_scl_hz = scl_hz;     /* 记住这次配的速率，供总线恢复用 */

  return HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
}

/* 只给自检固件（selftest.c / pa15test.c）用：100kHz 最保守。
 * ★ 总线恢复不许调这个 —— 它会把主程序的 400kHz 顶掉，见 board_i2c_bus_recover()。 */
HAL_StatusTypeDef board_i2c_init(void)
{
  return board_i2c_init_hz(100000U);
}

uint32_t board_i2c_scl_hz(void)
{
  return s_i2c_scl_hz;
}

HAL_StatusTypeDef i2c_probe(uint8_t addr7)
{
  return HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr7 << 1), 2, 20);
}

I2C_HandleTypeDef *board_i2c(void)
{
  return &hi2c1;
}

/* ---- I2C1 异步（IT + DMA）刷新所需的接线 ----
 * 官方 NUCLEO-G474RE 的 I2C_TwoBoards_ComDMA 例程里：
 *   I2C1_TX -> DMA1_Channel1，请求 DMA_REQUEST_I2C1_TX
 * 页命令（3 字节）走中断，页数据（最多 128 字节）走 DMA。 */
static DMA_HandleTypeDef hdma_i2c1_tx;

HAL_StatusTypeDef board_i2c_enable_dma(void)
{
  /* ★ G4 上 DMAMUX1 的时钟必须显式打开（HAL 不会自动开，官方例程 main.c 里有这一句）。
   *   漏了它：DMA 请求没有路由，异步传输永远不会发生 —— 现象是屏幕一直黑。 */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_i2c1_tx.Instance                 = DMA1_Channel1;
  hdma_i2c1_tx.Init.Request             = DMA_REQUEST_I2C1_TX;
  hdma_i2c1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_i2c1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_i2c1_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_i2c1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_i2c1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_i2c1_tx.Init.Mode                = DMA_NORMAL;
  hdma_i2c1_tx.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_i2c1_tx) != HAL_OK) return HAL_ERROR;
  __HAL_LINKDMA(&hi2c1, hdmatx, hdma_i2c1_tx);

  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  HAL_NVIC_SetPriority(I2C1_EV_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  HAL_NVIC_SetPriority(I2C1_ER_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
  return HAL_OK;
}

DMA_HandleTypeDef *board_i2c_dma_tx(void)
{
  return &hdma_i2c1_tx;
}

HAL_StatusTypeDef board_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
  return HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(addr7 << 1),
                                 (uint8_t *)buf, len, 200);
}

HAL_StatusTypeDef i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
  return HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(addr7 << 1), reg,
                          I2C_MEMADD_SIZE_8BIT, buf, len, 50);
}

HAL_StatusTypeDef i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint16_t len)
{
  return HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(addr7 << 1), reg,
                           I2C_MEMADD_SIZE_8BIT, (uint8_t *)buf, len, 50);
}

/* 总线被拉死时的恢复：手动发 9 个时钟 + STOP */
void board_i2c_bus_recover(void)
{
  GPIO_InitTypeDef io = {0};
  int i;

  HAL_I2C_DeInit(&hi2c1);
  io.Mode  = GPIO_MODE_OUTPUT_OD;
  io.Pull  = GPIO_PULLUP;
  io.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Pin   = I2C1_SCL_PIN; HAL_GPIO_Init(I2C1_SCL_PORT, &io);
  io.Pin   = I2C1_SDA_PIN; HAL_GPIO_Init(I2C1_SDA_PORT, &io);

  HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_SET);
  for (i = 0; i < 9; i++) {
    HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_SET);
    HAL_Delay(1);
  }
  /* STOP */
  HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_RESET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(I2C1_SCL_PORT, I2C1_SCL_PIN, GPIO_PIN_SET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(I2C1_SDA_PORT, I2C1_SDA_PIN, GPIO_PIN_SET);
  HAL_Delay(1);

  /* ★ 按**当前配置的速率**重配，绝不能调 board_i2c_init()：那个函数是自检固件的
   * 100kHz。主程序开机是 400kHz（main_app.c:80），恢复时降到 100kHz 的话屏的异步
   * 搬运会慢 4 倍（满屏帧 ~23ms -> ~94ms），现象就是用户 m26333 报的"屏幕有时候
   * 特别卡、帧率特别低"，而且断电重启才回得去 —— 而这台车恰恰会因为灰度模块掉线/
   * ESD 之类的事触发恢复，谁也想不到屏变慢是"恢复"干的。 */
  (void)board_i2c_init_hz(s_i2c_scl_hz);
}

/* ---------------- I2C 总线健康看门狗 ----------------
 * 决策逻辑在 App/Svc/bus_health.c 里（纯逻辑，主机侧能单测），这里只负责
 * "报结果"和"真恢复"这两件碰硬件的事。为什么需要：总线被拉死（从机在事务中间
 * 掉电/复位、ESD 打一下）之后，每一次 HAL_I2C_* 都只会超时失败、永不自愈 ——
 * 屏黑着、电流和灰度全读不到，只能断电重启。
 *   报告点：所有 i2c_read_reg/i2c_write_reg 的调用者（灰度、INA226、屏）。
 *   执行点：主循环里总线空闲的地方（bus_idle 由调用者给，别插进屏的 DMA 搬运）。 */
void board_i2c_report(uint8_t ok)
{
  bus_health_report(ok);
}

uint8_t board_i2c_watchdog(uint8_t bus_idle)
{
  if (!bus_health_take(HAL_GetTick(), bus_idle)) return 0U;
  board_i2c_bus_recover();          /* 阻塞约 12ms：9 个时钟 + STOP，每个 1ms */
  return 1U;
}

uint32_t board_i2c_recover_count(void)
{
  return bus_health_count();
}

/* =====================================================================
 *  INA226
 * ===================================================================== */
HAL_StatusTypeDef ina226_read_id(uint16_t *mfg, uint16_t *die)
{
  uint8_t b[2];
  if (i2c_read_reg(I2C_ADDR_INA226, 0xFE, b, 2) != HAL_OK) return HAL_ERROR;
  *mfg = (uint16_t)((b[0] << 8) | b[1]);
  if (i2c_read_reg(I2C_ADDR_INA226, 0xFF, b, 2) != HAL_OK) return HAL_ERROR;
  *die = (uint16_t)((b[0] << 8) | b[1]);
  return HAL_OK;
}

HAL_StatusTypeDef ina226_init(void)
{
  /* 配置寄存器 0x00 = 0x4127（位序已在本板上实测确认，别照抄"16 次平均"那种位序）：
   *   bit15    RST     自清零，读回恒 0
   *   bit14:12 保留    只读，写任何值都读回 100（实测写过 0x0007/0x0000/0x6007）
   *   bit11:9  AVG     000 = 1 次平均
   *   bit8:6   VBUSCT  100 = 1.1ms
   *   bit5:3   VSHCT   100 = 1.1ms
   *   bit2:0   MODE    111 = 分流+母线 连续
   * 实测：写 0x0000 -> 掉电、转换停止（MODE 确认在 bit2:0）；写 0x0007 -> 每 0.28ms
   * 一次转换（140us 档）；写 0x4127 -> 每 2.26ms 更新一次（= 1 次平均）。 */
  uint8_t cfg[2] = {0x41, 0x27};
  uint8_t cal[2] = {0x04, 0x00};   /* CAL = 1024 -> 1mA/bit, 5mOhm */
  if (i2c_write_reg(I2C_ADDR_INA226, 0x00, cfg, 2) != HAL_OK) return HAL_ERROR;
  if (i2c_write_reg(I2C_ADDR_INA226, 0x05, cal, 2) != HAL_OK) return HAL_ERROR;
  return HAL_OK;
}

static HAL_StatusTypeDef ina226_read_u16(uint8_t reg, int32_t *raw, uint16_t *u16)
{
  uint8_t b[2];
  uint16_t v;
  if (i2c_read_reg(I2C_ADDR_INA226, reg, b, 2) != HAL_OK) return HAL_ERROR;
  v = (uint16_t)((b[0] << 8) | b[1]);
  if (u16) *u16 = v;
  *raw = (int32_t)(int16_t)v;
  return HAL_OK;
}

HAL_StatusTypeDef ina226_read_bus_voltage_mv(int32_t *mv)
{
  int32_t raw; uint16_t u;
  if (ina226_read_u16(0x02, &raw, &u) != HAL_OK) return HAL_ERROR;
  *mv = (int32_t)((int64_t)u * 1250 / 1000);   /* 1.25mV/bit */
  return HAL_OK;
}

HAL_StatusTypeDef ina226_read_shunt_uv(int32_t *uv)
{
  int32_t raw;
  if (ina226_read_u16(0x01, &raw, NULL) != HAL_OK) return HAL_ERROR;
  *uv = raw * 25 / 10;                          /* 2.5uV/bit */
  return HAL_OK;
}

HAL_StatusTypeDef ina226_read_current_ma(int32_t *ma)
{
  int32_t raw;
  if (ina226_read_u16(0x04, &raw, NULL) != HAL_OK) return HAL_ERROR;
  *ma = raw;                                    /* 1mA/bit（CAL=1024, 5mOhm） */
  return HAL_OK;
}

/* 功率寄存器（0x03）由 INA226 内部按 CAL 算好：Power_LSB = 25 x Current_LSB
 * = 25 x 1mA = 25mW/bit（INA226 数据手册 7.5 节）。它是无符号量，
 * 最大 32767 x 25mW = 819W。 */
HAL_StatusTypeDef ina226_read_power_mw(int32_t *mw)
{
  int32_t raw; uint16_t u;
  if (ina226_read_u16(0x03, &raw, &u) != HAL_OK) return HAL_ERROR;
  *mw = (int32_t)((int64_t)u * 25);
  return HAL_OK;
}

/* =====================================================================
 *  串口
 * ===================================================================== */
HAL_StatusTypeDef board_uart_jy61_init(void)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_USART3_CLK_ENABLE();

  io.Pin       = JY61_TX_PIN;
  io.Mode      = GPIO_MODE_AF_PP;
  io.Pull      = GPIO_PULLUP;
  io.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF7_USART3;
  HAL_GPIO_Init(JY61_TX_PORT, &io);

  io.Pin       = JY61_RX_PIN;
  HAL_GPIO_Init(JY61_RX_PORT, &io);

  hjy61.Instance          = USART3;
  hjy61.Init.BaudRate     = 115200;
  hjy61.Init.WordLength   = UART_WORDLENGTH_8B;
  hjy61.Init.StopBits     = UART_STOPBITS_1;
  hjy61.Init.Parity       = UART_PARITY_NONE;
  hjy61.Init.Mode         = UART_MODE_TX_RX;
  hjy61.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  hjy61.Init.OverSampling = UART_OVERSAMPLING_16;
  hjy61.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hjy61.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  return HAL_UART_Init(&hjy61);
}

UART_HandleTypeDef *board_jy61_handle(void) { return &hjy61; }

/* ---- USART3 中断接收 + 256 字节环形缓冲 ----
 * JY61P 是 100Hz 连续输出（约 1.1KB/s），主循环 100ms 轮询一次必然丢字节，
 * 必须靠中断把每个字节搬进环形缓冲，主循环只负责"取"。 */
#define JY_RX_RING_SZ   256
static uint8_t  s_rx_ring[JY_RX_RING_SZ];
static volatile uint16_t s_rx_head, s_rx_tail;
static uint8_t  s_rx_byte;

/* ---- LPUART1（上位机链路）的 DMA 状态：定义必须早于用到它的中断回调 ----
 * 2026-10-03 提速改造（用户 m20705"刷新快点、下发快点"）：115200 → 921600，
 * 收改成"循环 DMA 搬运"（不再有逐字节中断：921600 下那是 92kHz 中断），
 * 发改成"DMA 槽位队列"（board_dl_send 只拷贝+排队就返回，不阻塞主循环）。
 *   DL_RX_DMA_SZ 必须是 2 的幂：主循环按 DMA 的 CNDTR 反推写指针，用掩码绕圈取模。 */
#define DL_RX_DMA_SZ     2048u     /* 921600 下约 22ms 的缓冲余量 */
#define DL_TX_SLOTS      8u        /* 一帧最大 7 + MAX_PAYLOAD(124) = 131 B */
#define DL_TX_SLOT_SZ    144u      /* 槽位字节数（131 向上取整，留点余量） */

static DMA_HandleTypeDef hdma_dl_rx;
static DMA_HandleTypeDef hdma_dl_tx;
static uint8_t  s_dl_rx_dma[DL_RX_DMA_SZ];
static volatile uint16_t s_dl_rx_read;     /* 主循环读到哪：写指针 = 缓冲末尾 - CNDTR */
static volatile uint32_t s_dl_rx_overrun;  /* 落后整整一缓冲、有老数据被覆盖的次数（诊断） */
static uint8_t  s_dl_tx_slot[DL_TX_SLOTS][DL_TX_SLOT_SZ];
static uint16_t s_dl_tx_len[DL_TX_SLOTS];
static volatile uint8_t  s_dl_tx_head, s_dl_tx_tail, s_dl_tx_active;
static volatile uint8_t  s_dl_tx_count;    /* 队列里排着几帧：head==tail 分不出"空"和"满" */
static volatile uint32_t s_dl_tx_drop;     /* 队列满/超长而被丢掉的帧数（诊断） */

void board_jy61_start_rx(void)
{
  s_rx_head = 0;
  s_rx_tail = 0;
  HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART3_IRQn);
  (void)HAL_UART_Receive_IT(&hjy61, &s_rx_byte, 1);
}

int board_jy61_get_byte(uint8_t *b)
{
  if (s_rx_tail == s_rx_head) return 0;
  *b = s_rx_ring[s_rx_tail];
  s_rx_tail = (uint16_t)((s_rx_tail + 1u) & (JY_RX_RING_SZ - 1u));
  return 1;
}

UART_HandleTypeDef *board_dl_handle(void)   { return &hdl; }

/* ---- 收发回调：必须放在 DMA 状态**定义之后**（C 要求先声明）----
 * LPUART1 已经不在 HAL_UART_RxCpltCallback 里了：它整条 RX 走 DMA1_Ch2 循环搬运，
 * 没有逐字节中断。TX 完成则要在这里放槽位（见 HAL 的 UART_DMATransmitCplt：
 * 普通 DMA 模式是"DMA 走完 → 置 TCIE → LPUART1 中断里回调"，所以两个中断都要开）。 */
static void dl_tx_kick(void);

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3) {
    uint16_t next = (uint16_t)((s_rx_head + 1u) & (JY_RX_RING_SZ - 1u));
    if (next != s_rx_tail) {            /* 满了就丢，不覆盖未读数据 */
      s_rx_ring[s_rx_head] = s_rx_byte;
      s_rx_head = next;
    }
    (void)HAL_UART_Receive_IT(&hjy61, &s_rx_byte, 1);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != LPUART1) return;
  s_dl_tx_active = 0u;                                      /* 这一帧真的出去了 */
  s_dl_tx_tail = (uint8_t)((s_dl_tx_tail + 1u) % DL_TX_SLOTS);
  if (s_dl_tx_count) s_dl_tx_count--;
  dl_tx_kick();                                             /* 接着发下一帧 */
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3) {
    __HAL_UART_CLEAR_OREFLAG(huart);
    (void)HAL_UART_Receive_IT(&hjy61, &s_rx_byte, 1);
  } else if (huart->Instance == LPUART1) {
    /* 收是 DMA 的：不能像 115200 时代那样重挂 HAL_UART_Receive_IT（会和 DMA 抢）。
     * 万一 HAL 走了"阻塞错误"分支（它会清掉 CR3.DMAR），这里把收 DMA 重新拉起来。 */
    __HAL_UART_CLEAR_OREFLAG(huart);
    if ((LPUART1->CR3 & USART_CR3_DMAR) == 0u) {
      s_dl_rx_read = 0u;
      (void)HAL_DMA_Start(&hdma_dl_rx, (uint32_t)&LPUART1->RDR,
                          (uint32_t)s_dl_rx_dma, DL_RX_DMA_SZ);
      SET_BIT(LPUART1->CR3, USART_CR3_DMAR);
    }
    /* 发的槽位也要放掉，否则队列会卡在"active"上永远不发下一帧 */
    (void)HAL_UART_AbortTransmit(&hdl);
    if (s_dl_tx_active) {
      s_dl_tx_active = 0u;
      s_dl_tx_tail = (uint8_t)((s_dl_tx_tail + 1u) % DL_TX_SLOTS);
      if (s_dl_tx_count) s_dl_tx_count--;
    }
    dl_tx_kick();
  }
}
/* ---- LPUART1：无线下载器（PC0=RX / PC1=TX，AF8）
 * 上位机链路走这条：8N1，收 = DMA1_Ch2 循环搬运、发 = DMA1_Ch3 槽位队列。
 * 波特率必须和上位机桥一致（`python -m bridge.main --serial COM26 --baud 230400`），
 * 上位机那边的串口参数是 COM26 这条 CDC 串口自己的事，跟 SWD 无关。
 * 复用编号 AF8 是查数据手册 Table 13（DS12288 Rev6，第 75 页）确认的，不是猜的。
 * 2026-10-03 23:50 定档 **230400**：手上这颗 J-Link 的 CDC VCOM 上限实测就是 230400
 * （115200/230400 能开能读；460800 在 SET_LINE_CODING 上超时 = ERROR_SEM_TIMEOUT 121，
 * 921600 直接 ERROR_GEN_FAILURE 31，而且试 ≥460800 会把探针打进 bootloader 态、只能物理
 * 拔插 USB 才能恢复）。所以真机实测先跑 230400（原 115200 的 2×）；换普通 USB-TTL 接 U12
 * 脚6/5/4 保 921600 时，改这一行 + 桥跟上 `--baud` 就行。 */
#define DL_BAUD  230400u

HAL_StatusTypeDef board_dl_init(void)
{
  GPIO_InitTypeDef io = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_LPUART1_CLK_ENABLE();
  /* ★ G4 上 DMAMUX1 的时钟必须显式打开（HAL 不会自动开，官方例程 main.c 里有这一句）；
   *   漏了它：DMA 请求没有路由，收发都永远不会发生 —— 现象是链路一条数据都没有。 */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  io.Pin       = DL_TX_PIN;
  io.Mode      = GPIO_MODE_AF_PP;
  io.Pull      = GPIO_PULLUP;
  io.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF8_LPUART1;
  HAL_GPIO_Init(DL_TX_PORT, &io);

  io.Pin       = DL_RX_PIN;
  HAL_GPIO_Init(DL_RX_PORT, &io);

  hdl.Instance            = LPUART1;
  hdl.Init.BaudRate       = DL_BAUD;
  hdl.Init.WordLength     = UART_WORDLENGTH_8B;
  hdl.Init.StopBits       = UART_STOPBITS_1;
  hdl.Init.Parity         = UART_PARITY_NONE;
  hdl.Init.Mode           = UART_MODE_TX_RX;
  hdl.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
  hdl.Init.OverSampling   = UART_OVERSAMPLING_16;
  hdl.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hdl.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hdl) != HAL_OK) return HAL_ERROR;

  /* 收：DMA1_Ch2 循环把 RDR 搬进 s_dl_rx_dma。只开"绕圈 TC"中断用来数溢出，
   *     不挂 HAL 的收 DMA（那样它会自己管 CR3.DMAR 和回调），主循环按 CNDTR 反推写指针。 */
  hdma_dl_rx.Instance                 = DMA1_Channel2;
  hdma_dl_rx.Init.Request             = DMA_REQUEST_LPUART1_RX;
  hdma_dl_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_dl_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_dl_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_dl_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_dl_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_dl_rx.Init.Mode                = DMA_CIRCULAR;
  hdma_dl_rx.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_dl_rx) != HAL_OK) return HAL_ERROR;

  /* 发：DMA1_Ch3 普通模式 + 槽位队列；HAL_UART_Transmit_DMA() 自己去挂 huart->hdmatx */
  hdma_dl_tx.Instance                 = DMA1_Channel3;
  hdma_dl_tx.Init.Request             = DMA_REQUEST_LPUART1_TX;
  hdma_dl_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_dl_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_dl_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_dl_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_dl_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_dl_tx.Init.Mode                = DMA_NORMAL;
  hdma_dl_tx.Init.Priority            = DMA_PRIORITY_MEDIUM;
  if (HAL_DMA_Init(&hdma_dl_tx) != HAL_OK) return HAL_ERROR;
  __HAL_LINKDMA(&hdl, hdmatx, hdma_dl_tx);

  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 5, 0);   /* 收 DMA 绕圈（诊断用） */
  HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 5, 0);   /* 发 DMA 完成 */
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  return HAL_OK;
}



/* ---- UART4：灰度/循迹模块的串口（PC10=TX / PC11=RX，AF5）
 * 官方《关于IIC通信注意事项》写明：模块 IIC 默认输出数字量，想读模拟量必须先用
 * 串口把输出模式改成"模拟量/全部" -> 上电时发 0x4C 00 01 03（见 trace_cal.c）。
 * 另外模块也支持"你问我才答"的读取帧（0x57 + 地址），IIC 不通时可以改走这条线，
 * 那样 OLED 就独享 I2C 总线了（所以下面多一个带超时的读字节）。
 * AF5 是查数据手册 Table 13 得到的（PC10 = UART4_TX、PC11 = UART4_RX）。 */
static UART_HandleTypeDef huart4;

HAL_StatusTypeDef board_uart4_init(void)
{
  GPIO_InitTypeDef io = {0};

  if (huart4.Instance == UART4) return HAL_OK;   /* 只初始化一次 */

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_UART4_CLK_ENABLE();

  io.Pin       = GPIO_PIN_10 | GPIO_PIN_11;
  io.Mode      = GPIO_MODE_AF_PP;
  io.Pull      = GPIO_PULLUP;
  io.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  io.Alternate = GPIO_AF5_UART4;
  HAL_GPIO_Init(GPIOC, &io);

  huart4.Instance            = UART4;
  huart4.Init.BaudRate       = 115200;
  huart4.Init.WordLength     = UART_WORDLENGTH_8B;
  huart4.Init.StopBits       = UART_STOPBITS_1;
  huart4.Init.Parity         = UART_PARITY_NONE;
  huart4.Init.Mode           = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling   = UART_OVERSAMPLING_16;
  huart4.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  return HAL_UART_Init(&huart4);
}

void board_uart4_write(const uint8_t *data, uint16_t len)
{
  if (huart4.Instance != UART4) {
    if (board_uart4_init() != HAL_OK) return;
  }
  (void)HAL_UART_Transmit(&huart4, (uint8_t *)data, len, 50U);
}

/* 读一个字节：拿到返回 1，超时返回 0。
 * 官方例程把这条线当"设置口"只发不收，但模块也支持 0x57 + 地址 的读取帧
 * （System/USART/usart.c 的 Read_Data3 / Read_Data4），IIC 不通时 trace_cal.c 走它。
 * 超时给 0 就是"有就取走、没有立刻返回"，用来冲刷可能残留的字节。 */
int board_uart4_read_byte(uint8_t *b, uint32_t timeout_ms)
{
  if (huart4.Instance != UART4) {
    if (board_uart4_init() != HAL_OK) return 0;
  }
  return (HAL_UART_Receive(&huart4, b, 1U, timeout_ms) == HAL_OK) ? 1 : 0;
}

void board_dl_start_rx(void)
{
  s_dl_rx_read = 0;
  (void)HAL_DMA_Start(&hdma_dl_rx, (uint32_t)&LPUART1->RDR,
                      (uint32_t)s_dl_rx_dma, DL_RX_DMA_SZ);
  SET_BIT(LPUART1->CR3, USART_CR3_DMAR);         /* 打开收 DMA 请求 */
  __HAL_DMA_CLEAR_FLAG(&hdma_dl_rx, DMA_FLAG_TC2);
  __HAL_DMA_ENABLE_IT(&hdma_dl_rx, DMA_IT_TC);   /* 绕一圈报一次：数"主循环没跟上" */
  HAL_NVIC_SetPriority(LPUART1_IRQn, 5, 0);      /* 发完的 TC 在这里面 */
  HAL_NVIC_EnableIRQ(LPUART1_IRQn);
}

/* 收 DMA 绕了一圈：如果主循环还没读完（读指针 != 0），中间那段就被新数据盖掉了。
 * 不致命（协议层靠 SOF/CRC 重新同步），只数一下，报给 1Hz 的 RTT 行。 */
void board_dl_dma_rx_irq(void)
{
  if (__HAL_DMA_GET_FLAG(&hdma_dl_rx, DMA_FLAG_TC2) == RESET) return;
  __HAL_DMA_CLEAR_FLAG(&hdma_dl_rx, DMA_FLAG_TC2);
  if (s_dl_rx_read != 0u) {
    s_dl_rx_overrun++;
    s_dl_rx_read = 0u;                 /* 丢掉被覆盖的那一段 */
  }
}

/* 取一个字节：写指针 = 缓冲末尾 - DMA 还剩几个（CNDTR），和读指针比就有没有新数据。
 * 主循环 5ms 一拍，921600 下最多攒 ~576 字节，2048 的缓冲够用。 */
int board_dl_get_byte(uint8_t *b)
{
  uint16_t w = (uint16_t)((DL_RX_DMA_SZ - (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_dl_rx))
                          & (DL_RX_DMA_SZ - 1u));
  if (s_dl_rx_read == w) return 0;
  *b = s_dl_rx_dma[s_dl_rx_read];
  s_dl_rx_read = (uint16_t)((s_dl_rx_read + 1u) & (DL_RX_DMA_SZ - 1u));
  return 1;
}

/* 队列里有活就交给 DMA 发一帧出去（任务上下文和中断里都会被调到）。
 * "检查 active + 置位"必须原子：主循环和 TxCplt 中断同时进来会把同一帧发两遍，
 * 第二遍失败后还会把刚起来的传输 abort 掉。 */
static void dl_tx_kick(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s_dl_tx_active || s_dl_tx_count == 0u) { __set_PRIMASK(primask); return; }
  s_dl_tx_active = 1u;
  __set_PRIMASK(primask);

  if (HAL_UART_Transmit_DMA(&hdl, s_dl_tx_slot[s_dl_tx_tail], s_dl_tx_len[s_dl_tx_tail]) != HAL_OK) {
    /* 起不来：多半是上一次的 gState 卡在 BUSY_TX（TC 没等到）。清一次再试，仍不行就下次再说 */
    (void)HAL_UART_AbortTransmit(&hdl);
    if (HAL_UART_Transmit_DMA(&hdl, s_dl_tx_slot[s_dl_tx_tail], s_dl_tx_len[s_dl_tx_tail]) != HAL_OK) {
      s_dl_tx_active = 0u;
    }
  }
}

/* 发一帧：拷进槽位 → 发布 head → 交给 DMA —— 立刻返回，**绝不阻塞主循环**。
 * 队列满（8 帧 ≈ 1KB，约 11ms @921600）时丢帧并计数：宁可丢遥测，也不让控制环等串口。 */
HAL_StatusTypeDef board_dl_send(const uint8_t *data, uint16_t len)
{
  uint8_t  next;
  uint32_t primask;

  if (len == 0u || len > DL_TX_SLOT_SZ) { s_dl_tx_drop++; return HAL_ERROR; }
  if (s_dl_tx_count >= DL_TX_SLOTS) { s_dl_tx_drop++; return HAL_BUSY; }
  next = (uint8_t)((s_dl_tx_head + 1u) % DL_TX_SLOTS);
  memcpy(s_dl_tx_slot[s_dl_tx_head], data, len);
  s_dl_tx_len[s_dl_tx_head] = len;
  primask = __get_PRIMASK();      /* 与"发完中断"互斥：先填完内容，再发布 head 和计数 */
  __disable_irq();
  s_dl_tx_head = next;
  s_dl_tx_count++;
  __set_PRIMASK(primask);
  dl_tx_kick();
  return HAL_OK;
}

DMA_HandleTypeDef *board_dl_dma_tx(void)  { return &hdma_dl_tx; }
uint32_t board_dl_baud(void)              { return DL_BAUD; }
uint32_t board_dl_tx_dropped(void)        { return s_dl_tx_drop; }
uint32_t board_dl_rx_overrun(void)        { return s_dl_rx_overrun; }
