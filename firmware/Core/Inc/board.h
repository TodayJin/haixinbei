/**
 * @file  board.h
 * @brief 全地形机器人自检固件 —— 板级定义与测试接口
 *
 * 引脚全部按《固件接口规格.md》和网表 v6。
 */
#ifndef __BOARD_H
#define __BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* ---------------- 时钟自检结果 ---------------- */
typedef struct {
  uint8_t  hse_ok;        /* 1 = HSE(25MHz) 起振成功 */
  uint8_t  pll_used;      /* 1 = 已切到 PLL 170MHz */
  uint8_t  hse_ready;     /* RCC_CR.HSERDY */
  uint8_t  hsi_ready;     /* RCC_CR.HSIRDY */
  uint8_t  pll_locked;    /* RCC_CR.PLLRDY */
  uint32_t sysclk_hz;
  uint32_t hclk_hz;
  uint32_t pclk1_hz;
  uint32_t pclk2_hz;
  uint32_t flash_latency;
} clock_report_t;

/* ---------------- 引脚（网表 v6） ---------------- */
/* I2C1：OLED(0x3C) + INA226(0x40) + 循迹座 */
#define I2C1_SCL_PORT   GPIOA
#define I2C1_SCL_PIN    GPIO_PIN_15     /* PA15 */
#define I2C1_SDA_PORT   GPIOB
#define I2C1_SDA_PIN    GPIO_PIN_9      /* PB9  */

/* USART3：JY61P */
#define JY61_TX_PORT    GPIOB
#define JY61_TX_PIN     GPIO_PIN_10     /* PB10 */
#define JY61_RX_PORT    GPIOB
#define JY61_RX_PIN     GPIO_PIN_11     /* PB11 */

/* LPUART1：无线下载器虚拟串口（PC0=RX, PC1=TX） */
#define DL_TX_PORT      GPIOC
#define DL_TX_PIN       GPIO_PIN_1      /* PC1 */
#define DL_RX_PORT      GPIOC
#define DL_RX_PIN       GPIO_PIN_0      /* PC0 */

/* 五向键（低有效，内部上拉） */
#define KEY_UP_PORT     GPIOC
#define KEY_UP_PIN      GPIO_PIN_13
#define KEY_DOWN_PORT   GPIOC
#define KEY_DOWN_PIN    GPIO_PIN_14
#define KEY_LEFT_PORT   GPIOC
#define KEY_LEFT_PIN    GPIO_PIN_15
#define KEY_RIGHT_PORT  GPIOC
#define KEY_RIGHT_PIN   GPIO_PIN_2
#define KEY_CENTER_PORT GPIOC
#define KEY_CENTER_PIN  GPIO_PIN_3

/* 蜂鸣器（低边 S8050，高有效） */
#define BUZZER_PORT     GPIOD
#define BUZZER_PIN      GPIO_PIN_2

/* 电机 PWM（AT8236 输入，TIM8_CH1..4） */
#define MOTA_IN1_PORT   GPIOC
#define MOTA_IN1_PIN    GPIO_PIN_6
#define MOTA_IN2_PORT   GPIOC
#define MOTA_IN2_PIN    GPIO_PIN_7
#define MOTB_IN1_PORT   GPIOC
#define MOTB_IN1_PIN    GPIO_PIN_8
#define MOTB_IN2_PORT   GPIOC
#define MOTB_IN2_PIN    GPIO_PIN_9

/* 编码器（TIM3/TIM4） */
#define ENC1_A_PORT     GPIOB
#define ENC1_A_PIN      GPIO_PIN_4      /* PB4 */
#define ENC1_B_PORT     GPIOB
#define ENC1_B_PIN      GPIO_PIN_5      /* PB5 */
#define ENC2_A_PORT     GPIOB
#define ENC2_A_PIN      GPIO_PIN_6      /* PB6 */
#define ENC2_B_PORT     GPIOB
#define ENC2_B_PIN      GPIO_PIN_7      /* PB7 */

/* ---------------- I2C 从地址 ---------------- */
#define I2C_ADDR_OLED   0x3C
#define I2C_ADDR_INA226 0x40

/* ---------------- 板级接口 ---------------- */
void     board_clock_init(clock_report_t *rep);
void     board_gpio_keys_init(void);
void     board_buzzer_init(void);
HAL_StatusTypeDef board_i2c_init(void);          /* 100kHz（自检用，最保守） */
HAL_StatusTypeDef board_i2c_init_hz(uint32_t scl_hz);  /* 指定 SCL 频率 */
HAL_StatusTypeDef board_uart_jy61_init(void);
UART_HandleTypeDef *board_jy61_handle(void);
void board_jy61_start_rx(void);             /* 开 USART3 中断接收 + 环形缓冲 */
int  board_jy61_get_byte(uint8_t *b);       /* 取一个字节，无数据返回 0 */
UART_HandleTypeDef *board_dl_handle(void);
void     board_i2c_bus_recover(void);
/* I2C 总线健康看门狗（2026-10-02 加）：
 *   每一次 I2C 读/写都用 board_i2c_report(HAL_OK?) 报一次结果；
 *   连续失败 BOARD_I2C_FAIL_LIMIT(=5) 次就置"该恢复"标志，
 *   由 board_i2c_watchdog() 在**总线空闲**时真正执行恢复（bus_idle=0 就跳过，
 *   因为恢复会把 SCL/SDA 拉成 GPIO 手动打时钟，正在搬运的屏会被打断）。
 *   返回 1 = 这一次真的恢复了总线（会阻塞约 12ms：9 个时钟 + STOP，每个 1ms）。 */
void     board_i2c_report(uint8_t ok);
uint8_t  board_i2c_watchdog(uint8_t bus_idle);
uint32_t board_i2c_recover_count(void);   /* 上电以来恢复了几次（RTT 诊断用） */
void     board_pa15_release(void);          /* PA15 -> 输入上拉（复位默认 JTDI） */
void     board_pa15_od(uint8_t level);      /* PA15 -> 开漏输出 */
void     pa15_test_run(void);               /* PA15 判决实验 */

/* I2C 小工具（阻塞式，带超时） */
HAL_StatusTypeDef i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len);
HAL_StatusTypeDef i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint16_t len);
HAL_StatusTypeDef i2c_probe(uint8_t addr7);
I2C_HandleTypeDef *board_i2c(void);
HAL_StatusTypeDef  board_i2c_enable_dma(void);   /* I2C1 异步刷新接线（DMA1_Ch1 + 中断） */
DMA_HandleTypeDef *board_i2c_dma_tx(void);
HAL_StatusTypeDef board_i2c_write(uint8_t addr7, const uint8_t *buf, uint16_t len);

/* UART4（PC10=TX / PC11=RX，AF5）：灰度/循迹模块的串口，座子 H2 / U15。
 * 发：设置命令（0x4C... 把模块输出模式改成"全部"，这样 IIC 才能读 0xA2 模拟量）。
 * 收：模块只回"你问我才答"的读取帧（0x57 + 地址），所以阻塞读字节就够，
 *     不需要中断/环形缓冲；IIC 那条不通时 trace_cal.c 会拿它当备用数据通道。 */
HAL_StatusTypeDef board_uart4_init(void);
void              board_uart4_write(const uint8_t *data, uint16_t len);
int               board_uart4_read_byte(uint8_t *b, uint32_t timeout_ms);

/* LPUART1 = 无线下载器（上位机链路）：PC0=RX / PC1=TX，AF8
 * 8N1；收 = DMA1_Ch2 循环搬运（主循环按 CNDTR 取），发 = DMA1_Ch3 槽位队列
 * （非阻塞，满了丢帧并计数 —— 写上位机桥的波特率必须一致：--baud 见 board_dl_baud()）。
 * 波特率的唯一真值是 board.c 的 `#define DL_BAUD`（现 230400；原因见那里的注释），
 * 想打印/回显这个数字一律调 board_dl_baud()，别再手抄一份（手抄的启动横幅曾经
 * 在降档到 230400 之后还写着 921600）。 */
HAL_StatusTypeDef board_dl_init(void);
uint32_t          board_dl_baud(void);          /* 实际生效的链路波特率（= DL_BAUD） */
void              board_dl_start_rx(void);
int               board_dl_get_byte(uint8_t *b);
HAL_StatusTypeDef board_dl_send(const uint8_t *data, uint16_t len);
void              board_dl_dma_rx_irq(void);    /* DMA1_Channel2_IRQHandler 调：数绕圈溢出 */
DMA_HandleTypeDef *board_dl_dma_tx(void);       /* DMA1_Channel3_IRQHandler 要它 */
uint32_t          board_dl_tx_dropped(void);    /* 队列满/参数错丢掉的帧数（诊断） */
uint32_t          board_dl_rx_overrun(void);    /* 收 DMA 绕圈把没读完的数据盖掉的次数 */

/* INA226 */
HAL_StatusTypeDef ina226_read_id(uint16_t *mfg, uint16_t *die);
HAL_StatusTypeDef ina226_init(void);
HAL_StatusTypeDef ina226_read_bus_voltage_mv(int32_t *mv);
HAL_StatusTypeDef ina226_read_shunt_uv(int32_t *uv);
HAL_StatusTypeDef ina226_read_current_ma(int32_t *ma);
HAL_StatusTypeDef ina226_read_power_mw(int32_t *mw);

/* 五向键 */
uint8_t keys_raw(void);           /* bit0=上 bit1=下 bit2=左 bit3=右 bit4=中，1=按下 */

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H */
