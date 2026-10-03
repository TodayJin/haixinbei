/**
 * @file  task_store.c
 * @brief 上位机烧进 Flash 的任务程序（实现）
 *
 * 见 task_store.h 的布局说明。这里只依赖 task_prog（格式校验）、task_run（拒写判据）
 * 和三个控制模块（同上），不碰别的业务。
 */
#include "task_store.h"

#include "board.h"
#include "gyro_control.h"
#include "motion.h"
#include "speed_control.h"
#include "task_prog.h"
#include "task_run.h"

#include <string.h>

#define TASK_STORE_FLASH_BASE       0x08000000UL
#define TASK_STORE_PAGE0            60UL   /* 页 60/61/62，页 63 留给 nvram */
#define TASK_STORE_BANK_PAGE0       28UL   /* bank 内页号 = 页号 − 32（页 63 → 31） */
#define TASK_STORE_ERASE_TIMEOUT_MS 50U    /* 一页擦除典型 <5ms，超时就不死等 */

/* 编译期断言：头必须 8 的整数倍（按双字编程），否则尾部那几字节永远写不进去 */
typedef char task_store_hdr_must_be_8x[(sizeof(task_store_hdr_t) % 8U == 0U) ? 1 : -1];
typedef char task_store_cap_must_be_8x[(TASK_STORE_CAP % 8U == 0U) ? 1 : -1];

/* ---- 缓存（开机扫一次；Commit/Erase 写完立刻回读刷新） ---- */
static uint8_t  s_ok[TASK_STORE_SLOTS];
static uint16_t s_len[TASK_STORE_SLOTS];
static uint16_t s_crc[TASK_STORE_SLOTS];
static uint8_t  s_flags[TASK_STORE_SLOTS];

/* 名字在 Flash 里是定长区、不保证 NUL，所以拷一份到 RAM 当字符串用 */
static char     s_name[TASK_STORE_SLOTS][TASK_STORE_NAME_MAX + 1u];
static uint8_t  s_name_len[TASK_STORE_SLOTS];

/* ---- 上传暂存（T_TASK_PUT 的分片拼在这里，提交时才落 Flash） ---- */
static uint8_t  s_stage[TASK_STORE_CAP];
static uint16_t s_stage_hi;

/* 记录头暂存：按双字写，所以借联合体拿到 5 个 uint64 */
static union {
    task_store_hdr_t hdr;
    uint64_t         words[TASK_STORE_HDR_LEN / 8U];
} s_hdr;

uint8_t TaskStore_SlotCount(void)        { return TASK_STORE_SLOTS; }
uint8_t TaskStore_Page(uint8_t slot)     { return (uint8_t)(TASK_STORE_PAGE0 + slot); }
uint8_t TaskStore_BankPage(uint8_t slot) { return (uint8_t)(TASK_STORE_BANK_PAGE0 + slot); }

/* 槽位基址。返回 uintptr_t 而不是 uint32_t：主机测试里指针是 64 位，直接拿
 * uint32_t 转指针会被 -Wint-to-pointer-cast 抓（nvram.c 用 0x0801F800UL 躲过这条）。 */
static uintptr_t slot_addr(uint8_t slot)
{
    return (uintptr_t)(TASK_STORE_FLASH_BASE + (TASK_STORE_PAGE0 + (uint32_t)slot) * 0x800UL);
}

/* ---------------------------------------------------------------- 擦除 */

/**
 * @brief 擦掉一个槽位所在的页
 *
 * 与 nvram.c:301 的 nvram_page_erase() 同一套写法（也同一套理由）：本板 FLASH_OPTR
 * 的 DBANK 位读出来是 0，HAL_FLASHEx_Erase 会清掉 BKER 再用 PNB=63 越界擦，
 * 硬件静默忽略 ⇒ 页没擦、接着编程得到 PROGERR。所以这里手写 BKER + PNB。
 */
static uint8_t page_erase(uint8_t slot)
{
    uint8_t  dcache = (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) ? 1U : 0U;
    uint32_t page   = (uint32_t)TASK_STORE_BANK_PAGE0 + (uint32_t)slot;
    uint32_t t0;

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG | FLASH_CR_PER | FLASH_CR_PNB |
                         FLASH_CR_BKER | FLASH_CR_MER1 | FLASH_CR_MER2);
    SET_BIT(FLASH->CR, FLASH_CR_BKER);                          /* 页 60/61/62 都在 bank2 */
    MODIFY_REG(FLASH->CR, FLASH_CR_PNB, (page << FLASH_CR_PNB_Pos));
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    SET_BIT(FLASH->CR, FLASH_CR_STRT);

    t0 = HAL_GetTick();
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if ((uint32_t)(HAL_GetTick() - t0) > TASK_STORE_ERASE_TIMEOUT_MS) {
            break;
        }
    }
    CLEAR_BIT(FLASH->CR, FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER);

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }

    if ((FLASH->SR & (FLASH_FLAG_PGSERR | FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                      FLASH_FLAG_PROGERR | FLASH_FLAG_OPERR)) != 0U) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        return 0U;
    }
    return 1U;
}

/* 擦除静默失败只剩这一道防线：页首还是 0xFFFFFFFF 才算真擦干净 */
static uint8_t page_erased(uint8_t slot)
{
    return (uint8_t)(*(volatile uint32_t *)slot_addr(slot) == 0xFFFFFFFFUL);
}

/* ---------------------------------------------------------------- 编程 */

/* 凑一个双字：不足 8 字节的尾巴用 0xFF 补（擦除态，等价于没写） */
static uint64_t word_from(const uint8_t *src, uint32_t avail)
{
    uint8_t  tmp[8];
    uint64_t w;
    uint32_t i;

    for (i = 0U; i < 8U; i++) {
        tmp[i] = (i < avail) ? src[i] : 0xFFU;
    }
    memcpy(&w, tmp, sizeof(w));
    return w;
}

static uint8_t program_slot(uint8_t slot, const uint8_t *img, uint16_t len)
{
    uint32_t addr = slot_addr(slot);
    uint32_t i;

    for (i = 0U; i < (uint32_t)(TASK_STORE_HDR_LEN / 8U); i++, addr += 8U) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, s_hdr.words[i]) != HAL_OK) {
            return 0U;
        }
    }
    for (i = 0U; i < (uint32_t)len; i += 8U, addr += 8U) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr,
                              word_from(img + i, (uint32_t)len - i)) != HAL_OK) {
            return 0U;
        }
    }
    return 1U;
}

/* 擦 + 写 + 回读复查；返回 TASK_STORE_* */
static int write_slot(uint8_t slot, const uint8_t *img, uint16_t len)
{
    uint8_t dcache = (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) ? 1U : 0U;
    uint8_t ok;

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        if (dcache != 0U) {
            __HAL_FLASH_DATA_CACHE_RESET();
            __HAL_FLASH_DATA_CACHE_ENABLE();
        }
        return TASK_STORE_E_FLASH;
    }

    ok = page_erase(slot);
    if (ok == 0U) {
        /* 兜底：清一次标志再擦一遍（还没写数据，重来是安全的） */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        ok = page_erase(slot);
    }
    if (ok != 0U) {
        ok = page_erased(slot);
    }
    if (ok != 0U) {
        ok = program_slot(slot, img, len);
    }

    (void)HAL_FLASH_Lock();

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_RESET();     /* 不复位的话下面的回读可能命中旧 cache */
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }

    /* 写完读回来对一遍：擦写失败（页坏了/写保护）能立刻发现，别以为存上了 */
    if (ok != 0U) {
        ok = (uint8_t)(memcmp((const void *)slot_addr(slot), &s_hdr, TASK_STORE_HDR_LEN) == 0);
    }
    if (ok != 0U) {
        ok = (uint8_t)(memcmp((const void *)(slot_addr(slot) + TASK_STORE_HDR_LEN), img, len) == 0);
    }

    return (ok != 0U) ? TASK_STORE_OK : TASK_STORE_E_FLASH;
}

/* ---------------------------------------------------------------- 缓存 */

/**
 * @brief 重新读一个槽位并刷新缓存
 *
 * 判据三层：记录头自洽（magic/ver/id/名字长度/长度范围）→ 镜像格式校验
 * （TaskProg_Validate：magic/ver/长度/CRC16/各序列偏移）→ 头上的 crc16 与镜像里
 * 自带的那份一致。掉电写坏、擦除态、别人的数据都过不了这三层，一律当"空槽"。
 */
static void refresh(uint8_t slot)
{
    const task_store_hdr_t *h   = (const task_store_hdr_t *)slot_addr(slot);
    const uint8_t          *img = (const uint8_t *)(slot_addr(slot) + TASK_STORE_HDR_LEN);
    uint8_t ok = 0U;
    uint8_t n;

    s_ok[slot]       = 0U;
    s_len[slot]      = 0U;
    s_crc[slot]      = 0U;
    s_flags[slot]    = 0U;
    s_name_len[slot] = 0U;
    s_name[slot][0]  = '\0';

    if (h->magic == TASK_STORE_MAGIC && h->ver == TASK_STORE_VER && h->id == slot &&
        h->name_len <= TASK_STORE_NAME_MAX &&
        h->len >= TASK_HDR_LEN && h->len <= TASK_STORE_CAP) {
        if (TaskProg_Validate(img, h->len) == TASK_OK) {
            if (h->crc16 == TaskProg_Crc16(img + TASK_HDR_LEN,
                                           (uint32_t)h->len - TASK_HDR_LEN)) {
                ok = 1U;
            }
        }
    }

    if (ok != 0U) {
        n = h->name_len;
        memcpy(s_name[slot], h->name, n);
        s_name[slot][n] = '\0';

        s_ok[slot]       = 1U;
        s_len[slot]      = h->len;
        s_crc[slot]      = h->crc16;
        s_flags[slot]    = h->flags;
        s_name_len[slot] = n;
    }
}

/**
 * @brief 车在动 / 任务在跑 ⇒ 不许碰 Flash
 *
 * 擦一页会把 CPU 停住几十毫秒（G4 擦写期间取指停摆），正在跑的控制环或任务会直接
 * 失控。判据与 nvram.c:449 的 nvram_motion_active() 一致，另加解释器在跑。
 */
static uint8_t store_busy(void)
{
    if (Motion_GetMode() != MOTION_IDLE) {
        return 1U;
    }
    if (SpeedControl_IsEnabled() != 0U) {
        return 1U;
    }
    if (GyroControl_IsEnabled() != 0U) {
        return 1U;
    }
    if (TaskRun_IsRunning() != 0U) {
        return 1U;
    }
    return 0U;
}

/* ------------------------------------------------------------ 对外接口 */

void TaskStore_Init(void)
{
    uint8_t slot;

    s_stage_hi = 0U;
    memset(s_stage, 0xFF, sizeof(s_stage));

    for (slot = 0U; slot < TASK_STORE_SLOTS; slot++) {
        refresh(slot);
    }
}

uint8_t TaskStore_Present(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_ok[slot] : 0U;
}

uint16_t TaskStore_Len(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_len[slot] : 0U;
}

uint16_t TaskStore_Crc(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_crc[slot] : 0U;
}

uint8_t TaskStore_Flags(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_flags[slot] : 0U;
}

uint8_t TaskStore_Name(uint8_t slot, char *out, uint8_t cap)
{
    uint8_t n;

    if (slot >= TASK_STORE_SLOTS) {
        return 0U;
    }
    n = s_name_len[slot];
    if (out != NULL && cap > 0U) {
        uint8_t i;
        uint8_t m = (uint8_t)((n < (uint8_t)(cap - 1u)) ? n : (uint8_t)(cap - 1u));
        for (i = 0U; i < m; i++) {
            out[i] = s_name[slot][i];
        }
        out[m] = '\0';
    }
    return n;
}

const uint8_t *TaskStore_Image(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS || s_ok[slot] == 0U) {
        return NULL;
    }
    return (const uint8_t *)(slot_addr(slot) + TASK_STORE_HDR_LEN);
}

uint8_t TaskStore_AutoSlot(void)
{
    uint8_t slot;

    /* 从 0 开始扫 ⇒ 多个任务都置了自启位时跑 id 最小的那个 */
    for (slot = 0U; slot < TASK_STORE_SLOTS; slot++) {
        if (s_ok[slot] != 0U && (s_flags[slot] & TASK_STORE_FLAG_AUTO) != 0U) {
            return slot;
        }
    }
    return TASK_STORE_NO_SLOT;
}

int TaskStore_Load(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (s_ok[slot] == 0U) {
        refresh(slot);                     /* 别人（调试器）可能刚改过 Flash */
        if (s_ok[slot] == 0U) {
            return TASK_STORE_E_EMPTY;
        }
    }
    return TaskRun_Load(TaskStore_Image(slot), s_len[slot]);
}

void TaskStore_StageReset(void)
{
    s_stage_hi = 0U;
    memset(s_stage, 0xFF, sizeof(s_stage));
}

int TaskStore_Stage(uint16_t off, const uint8_t *d, uint16_t n)
{
    if (d == NULL || n == 0U) {
        return TASK_STORE_E_PARAM;
    }
    if ((uint32_t)off + (uint32_t)n > (uint32_t)TASK_STORE_CAP) {
        return TASK_STORE_E_PARAM;
    }
    if (off == 0U) {
        /* 偏移 0 = 新的一轮上传开始（分片都带绝对偏移）⇒ 顺手清掉上一轮的残留 */
        s_stage_hi = 0U;
    }

    memcpy(&s_stage[off], d, n);
    if ((uint32_t)off + (uint32_t)n > (uint32_t)s_stage_hi) {
        s_stage_hi = (uint16_t)((uint32_t)off + (uint32_t)n);
    }
    return TASK_STORE_OK;
}

uint16_t TaskStore_StageLen(void)
{
    return s_stage_hi;
}

int TaskStore_Commit(uint8_t slot, uint16_t len, uint16_t crc,
                     uint8_t flags, const char *name, uint8_t name_len)
{
    uint8_t n;
    int     e;

    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (len < TASK_HDR_LEN || len > TASK_STORE_CAP) {
        return TASK_STORE_E_PARAM;
    }
    if (s_stage_hi < len) {
        return TASK_STORE_E_PARAM;                  /* 分片没传够 */
    }
    if (name == NULL) {
        name_len = 0U;
    }
    if (name_len > TASK_STORE_NAME_MAX) {
        name_len = TASK_STORE_NAME_MAX;             /* 超长就截断，不报错 */
    }

    if (store_busy() != 0U) {
        return TASK_STORE_E_BUSY;
    }
    /* 先按 CRC 对一遍暂存：分片丢片/错位/上一轮残留都在这里现形 */
    if (TaskProg_Crc16(&s_stage[TASK_HDR_LEN], (uint32_t)len - TASK_HDR_LEN) != crc) {
        return TASK_STORE_E_CRC;
    }
    if (TaskProg_Validate(s_stage, len) != TASK_OK) {
        return TASK_STORE_E_FORMAT;
    }

    memset(&s_hdr, 0xFF, sizeof(s_hdr));
    s_hdr.hdr.magic    = TASK_STORE_MAGIC;
    s_hdr.hdr.ver      = TASK_STORE_VER;
    s_hdr.hdr.id       = slot;
    s_hdr.hdr.len      = len;
    s_hdr.hdr.crc16    = crc;
    s_hdr.hdr.flags    = (uint8_t)(flags & TASK_STORE_FLAG_AUTO);
    s_hdr.hdr.name_len = name_len;
    for (n = 0U; n < name_len; n++) {
        s_hdr.hdr.name[n] = (uint8_t)name[n];
    }

    e = write_slot(slot, s_stage, len);
    refresh(slot);                                  /* 只信回读的结果 */
    if (e == TASK_STORE_OK && s_ok[slot] == 0U) {
        e = TASK_STORE_E_FLASH;                     /* 回读没过三层校验 ⇒ 当写失败 */
    }
    if (e == TASK_STORE_OK) {
        TaskStore_StageReset();                     /* 落盘成功：暂存不留旧数据 */
    }
    return e;
}

int TaskStore_Erase(uint8_t slot)
{
    uint8_t dcache;
    uint8_t ok;
    int     e;

    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (store_busy() != 0U) {
        return TASK_STORE_E_BUSY;
    }

    dcache = (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) ? 1U : 0U;
    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        if (dcache != 0U) {
            __HAL_FLASH_DATA_CACHE_RESET();
            __HAL_FLASH_DATA_CACHE_ENABLE();
        }
        return TASK_STORE_E_FLASH;
    }

    ok = page_erase(slot);
    if (ok == 0U) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        ok = page_erase(slot);
    }
    if (ok != 0U) {
        ok = page_erased(slot);
    }
    (void)HAL_FLASH_Lock();

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }

    refresh(slot);
    /* 擦完的槽位必然"不在"；只要擦除本身没报错，这次删除就算成功 */
    e = (ok != 0U) ? TASK_STORE_OK : TASK_STORE_E_FLASH;
    return e;
}
