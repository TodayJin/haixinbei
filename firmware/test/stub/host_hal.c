/**
 * @file  host_hal.c   （主机侧假 HAL 实现 —— 只给 firmware/test 用）
 *
 * 提供：时间基准、GPIO/TIM/DWT/FLASH 的假实例。
 * 时钟用真实的单调时钟（C11 timespec_get），HAL_Delay 只推进假 tick 不真睡，
 * 这样跑几百万条 fuzz 输入也是秒级。
 */
#include "stm32g4xx_hal.h"
#include <time.h>
#include <string.h>

/* ---------------- 时间 ---------------- */
static uint32_t s_tick_ms;          /* 假的 HAL tick，HAL_Delay 推进它 */

uint32_t HAL_GetTick(void) { return s_tick_ms; }

void HAL_Delay(uint32_t ms) { s_tick_ms += ms; }

void host_tick_set(uint32_t ms) { s_tick_ms = ms; }
void host_tick_add(uint32_t ms) { s_tick_ms += ms; }

/* ---------------- 外设实例 ---------------- */
static GPIO_TypeDef s_gpio[5];
GPIO_TypeDef *const GPIOA = &s_gpio[0];
GPIO_TypeDef *const GPIOB = &s_gpio[1];
GPIO_TypeDef *const GPIOC = &s_gpio[2];
GPIO_TypeDef *const GPIOD = &s_gpio[3];
GPIO_TypeDef *const GPIOE = &s_gpio[4];

static TIM_HandleTypeDef s_tim1 __attribute__((unused));
static TIM_HandleTypeDef s_tim8 __attribute__((unused));
TIM_HandleTypeDef htim1;      /* bsp_board.h 里是对象，这里跟着定义对象 */
TIM_HandleTypeDef htim8;

/* NVIC_SystemReset 在主机上不能真复位：只记一次调用，测试可以断言"确实被调到了" */
int host_system_reset_calls;
void NVIC_SystemReset(void) { host_system_reset_calls++; }

/* 测试可以写这几个"比较寄存器"来模拟电机占空比 */
uint32_t host_tim_ccr[4];
uint32_t host_tim_get_compare(uint32_t channel)
{
    uint32_t i = channel / 4U;
    return (i < 4U) ? host_tim_ccr[i] : 0U;
}
void host_tim_set_compare(uint32_t channel, uint32_t value)
{
    uint32_t i = channel / 4U;
    if (i < 4U) host_tim_ccr[i] = value;
}

/* ---------------- DWT ---------------- */
static DWT_TypeDef s_dwt;
DWT_TypeDef *const DWT = &s_dwt;
uint32_t host_cycles(void) { return ++s_dwt.CYCCNT; }

/* ---------------- FLASH（把"页"放在 RAM 里） ---------------- */
#define HOST_FLASH_PAGES  64U
#define HOST_FLASH_PAGE   2048U
static uint8_t s_flash[HOST_FLASH_PAGES * HOST_FLASH_PAGE];
static FLASH_TypeDef s_flash_regs;
FLASH_TypeDef *const FLASH = &s_flash_regs;

static uint32_t s_flash_err;

/* host_flash_base(page) -> 页数据指针；页号越界返回 NULL（测试用来验越界） */
uint8_t *host_flash_page(uint32_t page)
{
    if (page >= HOST_FLASH_PAGES) return NULL;
    return &s_flash[page * HOST_FLASH_PAGE];
}

void host_flash_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(&s_flash_regs, 0, sizeof(s_flash_regs));
    s_flash_err = 0U;
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
uint32_t HAL_FLASH_GetError(void) { return s_flash_err; }
void FLASH_Unlock(void) { }
void FLASH_Lock(void) { }
void __HAL_FLASH_CLEAR_FLAG(uint32_t flags) { (void)flags; s_flash_err = 0U; }
void __HAL_FLASH_DATA_CACHE_DISABLE(void) { }
void __HAL_FLASH_DATA_CACHE_ENABLE(void) { }
void __HAL_FLASH_DATA_CACHE_RESET(void) { }

uint32_t FLASH_PageErase(uint32_t bank, uint32_t page)
{
    uint32_t p = (bank != 0U) ? (page + 32U) : page;   /* 假双 bank：bank2 从第 32 页起 */
    uint8_t *dst = host_flash_page(p);
    if (dst) memset(dst, 0xFF, HOST_FLASH_PAGE);
    return 0U;
}

uint32_t FLASH_Program(uint32_t address, uint64_t data)
{
    uint32_t off = address - FLASH_BASE;
    size_t   i;
    /* 主机上把地址折进假页数组：只有落到 s_flash 范围内的才写 */
    if (off < sizeof(s_flash)) {
        for (i = 0U; i < 8U; i++) s_flash[off + i] = (uint8_t)((data >> (8U * i)) & 0xFFU);
    }
    return 0U;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    FLASH_Program(Address, Data);
    return HAL_OK;
}

uint64_t FLASH_ReadDoubleWord(uint32_t address)
{
    uint32_t off = address - FLASH_BASE;
    uint64_t v = 0U;
    size_t   i;
    if (off + 8U <= sizeof(s_flash)) {
        for (i = 0U; i < 8U; i++) v |= ((uint64_t)s_flash[off + i]) << (8U * i);
    }
    return v;
}
