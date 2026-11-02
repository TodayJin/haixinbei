/**
 * @file  stm32g4xx_hal_conf.h
 * @brief 全地形机器人自检固件 —— HAL 模块裁剪配置
 *        只开这块板用得到的模块；HSE 按板上实际晶振 25MHz。
 */
#ifndef STM32G4xx_HAL_CONF_H
#define STM32G4xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 模块开关 ---------- */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ---------- 振荡器 / 时钟 ---------- */
#if !defined  (HSE_VALUE)
  #define HSE_VALUE                (25000000UL)   /* 板上晶振 25MHz */
#endif
#if !defined  (HSE_STARTUP_TIMEOUT)
  #define HSE_STARTUP_TIMEOUT      (100UL)
#endif
#if !defined  (HSI_VALUE)
  #define HSI_VALUE                (16000000UL)
#endif
#if !defined  (HSI_STARTUP_TIMEOUT)
  #define HSI_STARTUP_TIMEOUT      (5000UL)
#endif
#if !defined  (HSI48_VALUE)
  #define HSI48_VALUE              (48000000UL)
#endif
#if !defined  (HSI48_STARTUP_TIMEOUT)
  #define HSI48_STARTUP_TIMEOUT    (2UL)
#endif
#if !defined  (LSI_VALUE)
  #define LSI_VALUE                (32000UL)
#endif
#if !defined  (LSE_VALUE)
  #define LSE_VALUE                (32768UL)       /* 本板未装 LSE */
#endif
#if !defined  (LSE_STARTUP_TIMEOUT)
  #define LSE_STARTUP_TIMEOUT      (5000UL)
#endif
#if !defined  (EXTERNAL_CLOCK_VALUE)
  #define EXTERNAL_CLOCK_VALUE     (12288000UL)
#endif

/* ---------- 系统 ---------- */
#define  VDD_VALUE                   (3300UL)
#define  TICK_INT_PRIORITY           (0x0FUL)
#define  USE_RTOS                    0U
#define  PREFETCH_ENABLE             0U            /* G4 用 I-Cache 替代 */
#define  INSTRUCTION_CACHE_ENABLE    1U
#define  DATA_CACHE_ENABLE           1U

/* ---------- 断言 ---------- */
/* #define USE_FULL_ASSERT    1U */
#ifdef  USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
  void assert_failed(uint8_t *file, uint32_t line);
#else
  #define assert_param(expr) ((void)0U)
#endif

/* ---------- 各模块头 ---------- */
#ifdef HAL_RCC_MODULE_ENABLED
  #include "stm32g4xx_hal_rcc.h"
  #include "stm32g4xx_hal_rcc_ex.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
  #include "stm32g4xx_hal_gpio.h"
  #include "stm32g4xx_hal_gpio_ex.h"
#endif
#ifdef HAL_EXTI_MODULE_ENABLED
  #include "stm32g4xx_hal_exti.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
  #include "stm32g4xx_hal_dma.h"
  #include "stm32g4xx_hal_dma_ex.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
  #include "stm32g4xx_hal_cortex.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
  #include "stm32g4xx_hal_flash.h"
  #include "stm32g4xx_hal_flash_ex.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
  #include "stm32g4xx_hal_pwr.h"
  #include "stm32g4xx_hal_pwr_ex.h"
#endif
#ifdef HAL_I2C_MODULE_ENABLED
  #include "stm32g4xx_hal_i2c.h"
  #include "stm32g4xx_hal_i2c_ex.h"
#endif
#ifdef HAL_UART_MODULE_ENABLED
  #include "stm32g4xx_hal_uart.h"
  #include "stm32g4xx_hal_uart_ex.h"
#endif
#ifdef HAL_TIM_MODULE_ENABLED
  #include "stm32g4xx_hal_tim.h"
  #include "stm32g4xx_hal_tim_ex.h"
#endif

#include "stm32g4xx_hal_def.h"

#ifdef __cplusplus
}
#endif

#endif /* STM32G4xx_HAL_CONF_H */
