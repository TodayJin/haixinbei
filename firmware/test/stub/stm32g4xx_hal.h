/**
 * @file  stm32g4xx_hal.h   （主机侧假 HAL —— 只给 firmware/test 用，不进固件构建）
 *
 * 目的：让 App/Svc 里的纯逻辑（协议解析、标定数学、PID、寄存器打包）能在 PC 上
 * 用真源码编译、真跑起来，配合 UBSan trap 模式抓数组越界/有符号溢出/除零/空指针。
 * 用法：编译测试时把 -Ifirmware/test/stub 放在所有 -I 之前，真 HAL 头就不会被找到。
 *
 * 只实现被测模块真正用到的那一小块；缺什么补什么（编译报错会直接点名）。
 */
#ifndef __HOST_STM32G4XX_HAL_H
#define __HOST_STM32G4XX_HAL_H

#include <stdint.h>
#include <stddef.h>

/* ---- 通用 ---- */
typedef enum { HAL_OK = 0x00U, HAL_ERROR = 0x01U, HAL_BUSY = 0x02U, HAL_TIMEOUT = 0x03U } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef enum { DISABLE = 0, ENABLE = 1 } FunctionalState;

#ifndef __IO
#define __IO volatile
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif

#define SET_BIT(REG, BIT)     ((REG) |= (BIT))
#define CLEAR_BIT(REG, BIT)   ((REG) &= ~(BIT))
#define READ_BIT(REG, BIT)    ((REG) & (BIT))
#define MODIFY_REG(REG, CLEARMASK, SETMASK) ((REG) = ((REG) & ~(CLEARMASK)) | (SETMASK))

/* 位带/中断原语：主机上直接空操作（单线程测试） */
#define __disable_irq()  do { } while (0)
#define __enable_irq()   do { } while (0)

/* 主机上不能真复位：host_hal.c 里只记一次调用（host_system_reset_calls） */
void NVIC_SystemReset(void);
extern int host_system_reset_calls;
#define __enable_irq()   do { } while (0)
#define __NOP()          do { } while (0)

/* ---- GPIO（只给常量，测试里不用真引脚） ---- */
typedef struct { uint32_t dummy; } GPIO_TypeDef;
extern GPIO_TypeDef *const GPIOA;
extern GPIO_TypeDef *const GPIOB;
extern GPIO_TypeDef *const GPIOC;
extern GPIO_TypeDef *const GPIOD;
extern GPIO_TypeDef *const GPIOE;

#define GPIO_PIN_0   ((uint16_t)0x0001)
#define GPIO_PIN_1   ((uint16_t)0x0002)
#define GPIO_PIN_2   ((uint16_t)0x0004)
#define GPIO_PIN_3   ((uint16_t)0x0008)
#define GPIO_PIN_4   ((uint16_t)0x0010)
#define GPIO_PIN_5   ((uint16_t)0x0020)
#define GPIO_PIN_6   ((uint16_t)0x0040)
#define GPIO_PIN_7   ((uint16_t)0x0080)
#define GPIO_PIN_8   ((uint16_t)0x0100)
#define GPIO_PIN_9   ((uint16_t)0x0200)
#define GPIO_PIN_10  ((uint16_t)0x0400)
#define GPIO_PIN_11  ((uint16_t)0x0800)
#define GPIO_PIN_12  ((uint16_t)0x1000)
#define GPIO_PIN_13  ((uint16_t)0x2000)
#define GPIO_PIN_14  ((uint16_t)0x4000)
#define GPIO_PIN_15  ((uint16_t)0x8000)

/* ---- 外设句柄：主机上只要"有这个类型"即可（被测模块不碰内部字段） ---- */
typedef struct { uint32_t dummy; } UART_HandleTypeDef;
typedef struct { uint32_t dummy; } I2C_HandleTypeDef;
typedef struct { uint32_t dummy; } DMA_HandleTypeDef;
typedef struct { uint32_t dummy; } TIM_HandleTypeDef;
typedef struct { uint32_t dummy; } ADC_HandleTypeDef;

/* ---- 时间基准：host_hal.c 里用真实单调时钟实现 ---- */
uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);

/* ---- Flash：寄存器块在 host_hal.c 里定义，NVCRAM 数组当"闪存页" ---- */
typedef struct {
    __IO uint32_t ACR;
    __IO uint32_t PDKEYR;
    __IO uint32_t KEYR;
    __IO uint32_t OPTKEYR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t ECCR;
    __IO uint32_t OPTR;
    __IO uint32_t PCROP1SR;
    __IO uint32_t PCROP1ER;
    __IO uint32_t WRP1AR;
    __IO uint32_t WRP1BR;
} FLASH_TypeDef;
extern FLASH_TypeDef *const FLASH;

#define FLASH_CR_PG       (1UL << 0)
#define FLASH_CR_PER      (1UL << 1)
#define FLASH_CR_MER1     (1UL << 2)
#define FLASH_CR_MER2     (1UL << 15)
#define FLASH_CR_PNB_Pos  (3U)
#define FLASH_CR_PNB      (0x7FUL << FLASH_CR_PNB_Pos)
#define FLASH_CR_BKER     (1UL << 11)
#define FLASH_CR_STRT     (1UL << 16)
#define FLASH_CR_LOCK     (1UL << 31)
#define FLASH_SR_EOP      (1UL << 0)
#define FLASH_SR_BSY      (1UL << 16)

#define FLASH_BASE        0x08000000UL
#define FLASH_ACR_DCEN    (1UL << 10)
#define FLASH_OPTR_DBANK  (1UL << 22)

#define FLASH_FLAG_EOP     (1UL << 0)
#define FLASH_FLAG_OPERR   (1UL << 1)
#define FLASH_FLAG_PROGERR (1UL << 3)
#define FLASH_FLAG_WRPERR  (1UL << 4)
#define FLASH_FLAG_PGAERR  (1UL << 5)
#define FLASH_FLAG_PGSERR  (1UL << 7)
#define FLASH_FLAG_ALL_ERRORS (FLASH_FLAG_OPERR | FLASH_FLAG_PROGERR | FLASH_FLAG_WRPERR | \
                               FLASH_FLAG_PGAERR | FLASH_FLAG_PGSERR)

typedef enum { FLASH_TYPEPROGRAM_DOUBLEWORD = 0x00U } FLASH_ProgramTypeDef;
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data);
uint32_t          HAL_FLASH_GetError(void);

/* HAL 的"薄函数"在这个工程里是宏，测试里也给成同名的函数/宏 */
void     __HAL_FLASH_CLEAR_FLAG(uint32_t flags);
void     __HAL_FLASH_DATA_CACHE_DISABLE(void);
void     __HAL_FLASH_DATA_CACHE_ENABLE(void);
void     __HAL_FLASH_DATA_CACHE_RESET(void);
uint32_t FLASH_PageErase(uint32_t bank, uint32_t page);
uint32_t FLASH_Program(uint32_t address, uint64_t data);
void     FLASH_Unlock(void);
void     FLASH_Lock(void);
uint64_t FLASH_ReadDoubleWord(uint32_t address);   /* 测试自检用 */
/* 假 Flash 的 LOCK 状态（上电 = 1，与真板复位值一致）：测试用来断言
 * "上锁时擦写必须失败"，防止再次出现漏 HAL_FLASH_Unlock() 的真板 Bug。 */
int      host_flash_locked(void);
int      host_flash_try_erase_page(uint32_t page);
void     host_flash_unlock(void);
void     host_flash_lock(void);

/* ---- TIM：比较寄存器读写 ----
 * 真 HAL 的 __HAL_TIM_SET_COMPARE 是写 TIMx->CCRy；这里落到 host_hal.c 的
 * host_tim_ccr[]（真 servo.c 用得到，见 firmware/test/pipe_hostlink.c 的构建）。
 * 两个宏都按假 HAL 的习惯忽略句柄参数（假 TIM_HandleTypeDef 里没有 Instance）。 */
#define TIM_CHANNEL_1  0x00000000U
#define TIM_CHANNEL_2  0x00000004U
#define TIM_CHANNEL_3  0x00000008U
#define TIM_CHANNEL_4  0x0000000CU
#define __HAL_TIM_GET_COMPARE(__HANDLE__, __CHANNEL__)  host_tim_get_compare(__CHANNEL__)
#define __HAL_TIM_SET_COMPARE(__HANDLE__, __CHANNEL__, __COMPARE__)  host_tim_set_compare(__CHANNEL__, __COMPARE__)
uint32_t host_tim_get_compare(uint32_t channel);
void     host_tim_set_compare(uint32_t channel, uint32_t value);
/* 旧拼法（ST 的 stm32_hal_legacy.h 里就这么转的）：本工程 servo.c/motor.c 用的是这组 */
#define __HAL_TIM_SetCompare(__HANDLE__, __CHANNEL__, __COMPARE__) \
        __HAL_TIM_SET_COMPARE(__HANDLE__, __CHANNEL__, __COMPARE__)
#define __HAL_TIM_GetCompare(__HANDLE__, __CHANNEL__) \
        __HAL_TIM_GET_COMPARE(__HANDLE__, __CHANNEL__)
/* 注意：bsp_board.h 里 htim1/htim8… 是"对象"而不是指针，所以这里也只能是对象 */
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim8;

/* ---- DWT：trace_cal.c 用 CYCCNT 记账一帧耗时 ---- */
typedef struct { __IO uint32_t CTRL; __IO uint32_t CYCCNT; } DWT_TypeDef;
extern DWT_TypeDef *const DWT;
uint32_t host_cycles(void);          /* 主机上是"每次调用递增"的假周期计数 */

#endif /* __HOST_STM32G4XX_HAL_H */
