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
#ifdef _WIN32
#include <windows.h>   /* VirtualAlloc：把假 Flash 映射到 0x08000000 */
#endif

/* ---------------- 时间 ---------------- */
static uint32_t s_tick_ms;          /* 假的 HAL tick，HAL_Delay 推进它 */

static void flash_erase_hook(void); /* 见下面 FLASH 段：CR 里的擦除请求在这里兑现 */

uint32_t HAL_GetTick(void) { flash_erase_hook(); return s_tick_ms; }

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
#define HOST_FLASH_SIZE   (HOST_FLASH_PAGES * HOST_FLASH_PAGE)

static uint8_t s_flash_ram[HOST_FLASH_SIZE];
static uint8_t *s_flash = s_flash_ram;
static int s_flash_real_addr;

static FLASH_TypeDef s_flash_regs;
FLASH_TypeDef *const FLASH = &s_flash_regs;

static uint32_t s_flash_err;

/*
 * 固件里有的代码**按地址直读 Flash**（nvram.c 的
 * *(const volatile uint32_t *)0x0801F800），不走 HAL_FLASH_*。
 * 主机上就让假 Flash 真的映射在 0x08000000：地址直读也成立。
 * VirtualAlloc 失败（地址被占）就退回 RAM 数组，此时只有走 HAL 的读写
 * 可用；host_flash_uses_real_addr() 让上层知道能不能跑那种用例。
 */
void host_flash_map(void)
{
#ifdef _WIN32
    if (!s_flash_real_addr) {
        void *p = VirtualAlloc((LPVOID)(uintptr_t)FLASH_BASE, HOST_FLASH_SIZE,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p != NULL) {
            s_flash = (uint8_t *)p;
            s_flash_real_addr = 1;
        }
    }
#endif
}

int host_flash_uses_real_addr(void) { return s_flash_real_addr; }

__attribute__((constructor)) static void host_flash_map_early(void) { host_flash_map(); }

/* host_flash_base(page) -> 页数据指针；页号越界返回 NULL（测试用来验越界） */
uint8_t *host_flash_page(uint32_t page)
{
    if (page >= HOST_FLASH_PAGES) return NULL;
    return &s_flash[page * HOST_FLASH_PAGE];
}

/*
 * 主机上往 FLASH->CR 写位没有硬件效果，但真代码的擦除就是这么发的：
 * nvram_page_erase() 写 CR = BKER|PNB=31|PER|STRT，紧接着 HAL_GetTick()
 * 取超时起点（然后才查 SR/复验页）。所以在这里兑现：看见 STRT 就把
 * PNB/BKER 指定的那页抹成 0xFF，并清掉 STRT（只做一次）。
 * 等价于硬件在 STRT 之后自己把擦除做完、SR.BSY 归零。
 */
static void flash_erase_hook(void)
{
    uint32_t cr = s_flash_regs.CR;

    if ((cr & FLASH_CR_STRT) == 0U) return;
    s_flash_regs.CR = cr & ~(uint32_t)FLASH_CR_STRT;

    if ((cr & FLASH_CR_PER) != 0U) {
        uint32_t bank = ((cr & FLASH_CR_BKER) != 0U) ? 1U : 0U;
        uint32_t page = (cr & FLASH_CR_PNB) >> FLASH_CR_PNB_Pos;
        uint8_t *dst  = host_flash_page((bank != 0U) ? (page + 32U) : page);
        if (dst != NULL) memset(dst, 0xFF, HOST_FLASH_PAGE);
    }
    s_flash_regs.SR = 0U;
}

void host_flash_reset(void)
{
    memset(s_flash, 0xFF, HOST_FLASH_SIZE);
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
    /* 主机上把地址折进假页：只有落到假 Flash 范围内的才写 */
    if (off + 8U <= HOST_FLASH_SIZE) {
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
    if (off + 8U <= HOST_FLASH_SIZE) {
        for (i = 0U; i < 8U; i++) v |= ((uint64_t)s_flash[off + i]) << (8U * i);
    }
    return v;
}
