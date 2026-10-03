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
 * 取超时起点（然后才查 SR/复验页）。所以在这里兑现：看见 STRT 就把 **PNB
 * 指定的那个 4KB 块**抹成 0xFF，并清掉 STRT（只做一次）。
 * 等价于硬件在 STRT 之后自己把擦除做完、SR.BSY 归零。
 *
 * **块模型必须照真板 2026-10-04 实测来，别再改回"页"模型**：
 *   - 擦除粒度 = 4KB 块，块地址 = 0x08000000 + PNB×0x1000，一个块 = 相邻两个 2KB 页；
 *   - PNB 0..31 全部有效，**BKER 被硬件忽略**（单 bank，不存在"bank2 从第 32 页起"）；
 *   - PNB 越界（>31）时硬件静默忽略：SR 不置错、内容不变。
 * 这正是 task_store.c 老代码（PNB = 页号 - 32）在真机上擦掉固件页 54/55/56 的原因，
 * 也是 t_taskstore.c「固件所在的块必须原样保留」那条回归断言的依据。
 *
 * **LOCK 也必须照硬件演**：上锁时写 FLASH->CR 被硬件直接忽略（SR 不置错、
 * STRT 根本没被接受），于是"擦除成功返回、页却还是老内容"。真板靠这一条把
 * task_store.c 漏掉 HAL_FLASH_Unlock() 变成了 E_FLASH（见 t_taskstore.c 的
 * 「上锁时 Commit 必须失败」用例）。
 */
static void flash_erase_hook(void)
{
    uint32_t cr = s_flash_regs.CR;

    if ((cr & FLASH_CR_STRT) == 0U) return;
    s_flash_regs.CR = cr & ~(uint32_t)FLASH_CR_STRT;

    if ((cr & FLASH_CR_LOCK) != 0U) {
        return;                                 /* 上锁：请求被忽略，SR 保持干净 */
    }
    if ((cr & FLASH_CR_PER) != 0U) {
        uint32_t blk = (cr & FLASH_CR_PNB) >> FLASH_CR_PNB_Pos;   /* 块号（BKER 无关） */
        uint32_t i;
        for (i = 0U; i < 2U; i++) {             /* 一个块 = 两个 2KB 页 */
            uint8_t *dst = host_flash_page((blk * 2U) + i);
            if (dst != NULL) memset(dst, 0xFF, HOST_FLASH_PAGE);
        }
    }
    s_flash_regs.SR = 0U;
}

void host_flash_reset(void)
{
    memset(s_flash, 0xFF, HOST_FLASH_SIZE);
    memset(&s_flash_regs, 0, sizeof(s_flash_regs));
    s_flash_regs.CR = FLASH_CR_LOCK;            /* 上电复位值：LOCK = 1（真板同理） */
    s_flash_err = 0U;
}

int host_flash_locked(void) { return (s_flash_regs.CR & FLASH_CR_LOCK) != 0U; }

/* 测试用的解锁/上锁（等价于 HAL_FLASH_Unlock/Lock，但测试文件不必认识 HAL 类型） */
void host_flash_unlock(void) { (void)HAL_FLASH_Unlock(); }
void host_flash_lock(void) { (void)HAL_FLASH_Lock(); }

/* 测试用：照真代码的写法手发一次"擦这一页所在的块"（绝对页号 → 块号 = 页号/2），
 * 返回 1 = 页首真的变成 0xFF，0 = 没擦动（上锁时硬件就是这么忽略请求的）。 */
int host_flash_try_erase_page(uint32_t page)
{
    uint8_t *p = host_flash_page(page);
    uint32_t pnb = (page / 2U) << FLASH_CR_PNB_Pos;

    if (p == NULL) {
        return 0;
    }
    s_flash_regs.CR |= (FLASH_CR_PER | pnb | FLASH_CR_STRT | FLASH_CR_BKER);
    (void)HAL_GetTick();                       /* 擦除请求在这里被兑现/拒绝 */
    return (p[0] == 0xFFU && p[HOST_FLASH_PAGE - 1U] == 0xFFU) ? 1 : 0;
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    s_flash_regs.CR &= ~(uint32_t)FLASH_CR_LOCK;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    s_flash_regs.CR |= FLASH_CR_LOCK;
    return HAL_OK;
}
uint32_t HAL_FLASH_GetError(void) { return s_flash_err; }
void FLASH_Unlock(void) { (void)HAL_FLASH_Unlock(); }
void FLASH_Lock(void) { (void)HAL_FLASH_Lock(); }
void __HAL_FLASH_CLEAR_FLAG(uint32_t flags) { (void)flags; s_flash_err = 0U; }
void __HAL_FLASH_DATA_CACHE_DISABLE(void) { }
void __HAL_FLASH_DATA_CACHE_ENABLE(void) { }
void __HAL_FLASH_DATA_CACHE_RESET(void) { }

/* 真 HAL 的 FLASH_PageErase(bank, page)：本板单 bank，page 就是 4KB **块号**。
 * 固件并不走这条路（App/Svc/task_store.c 头部第 7 条说明本板的 HAL_FLASHEx_Erase
 * 静默失败），这里只为让桩完整：擦掉那一个块（两个 2KB 页）。 */
uint32_t FLASH_PageErase(uint32_t bank, uint32_t page)
{
    uint32_t i;
    (void)bank;
    if (host_flash_locked()) return 1U;                /* 上锁：擦不动 */
    for (i = 0U; i < 2U; i++) {
        uint8_t *dst = host_flash_page((page * 2U) + i);
        if (dst) memset(dst, 0xFF, HOST_FLASH_PAGE);
    }
    return 0U;
}

uint32_t FLASH_Program(uint32_t address, uint64_t data)
{
    uint32_t off = address - FLASH_BASE;
    size_t   i;
    if (host_flash_locked()) return 1U;                /* 上锁：写不进 */
    /* 主机上把地址折进假页：只有落到假 Flash 范围内的才写 */
    if (off + 8U <= HOST_FLASH_SIZE) {
        for (i = 0U; i < 8U; i++) s_flash[off + i] = (uint8_t)((data >> (8U * i)) & 0xFFU);
    }
    return 0U;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    if (host_flash_locked()) return HAL_ERROR;         /* 真 HAL：PG 写不进 ⇒ 等 EOP 超时 ⇒ HAL_ERROR */
    (void)FLASH_Program(Address, Data);
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
