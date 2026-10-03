/**
 * @file  task_store.c
 * @brief  任务程序的 Flash 持久层：页 59 目录 + 页 60..62 程序区（见 task_store.h）
 *
 * 设计要点（为什么长这样）：
 *   1. 目录页 + 紧凑程序区，而不是"一任务一页"：用户的页预算是 4 页 8KB / 最多 8 个任务
 *      （用户口径），一任务一页最多 3 个任务，装不下。
 *   2. 每条记录独立校验（结构 + CRC16），坏记录不影响别的记录；目录 magic/ver 坏 =
 *      整片空（自愈：下一次 Commit 把目录整页写回去）。
 *   3. 程序区里的镜像**逐字节就是 task_prog 的线上格式**，回读时按 off 直接指 Flash
 *      （XIP），没有第二份拷贝、没有第二种编码。
 *   4. 擦写顺序：**先擦目录页** ⇒ 掉电中断时车上看到的是"没有任务"，而不是
 *      "半个新程序配着旧偏移"。只有前者是安全的（跑错轨迹比不跑更糟）。
 *   5. 全片重排（写 1 个任务 = 擦 4 页 + 写 8KB）：这是"程序区紧凑摆放"的必然代价。
 *      擦一页 CPU 停几十毫秒 ⇒ 与 nvram.c 同一条铁律：**运动中绝不擦写**（判据见
 *      store_busy()）。
 *   6. 擦除本板会静默失败（HAL_FLASHEx_Erase / FLASH_PageErase 在此板不可用，见
 *      nvram.c:294-296 的铁律注释），所以这里手写 FLASH->CR 序列，并且在编程前
 *      **逐页确认真的擦回 0xFFFFFFFF**，写后再 memcmp 回读。
 */
#include "task_store.h"

#include <string.h>

#include "board.h"
#include "gyro_control.h"
#include "motion.h"
#include "speed_control.h"
#include "task_prog.h"
#include "task_run.h"

/* ---- Flash 物理参数（G474RB：单 bank 2KB × 64 页；bank 内页号 = 绝对页号 - 32） ---- */
#define TS_FLASH_BASE       0x08000000UL
#define TS_BANK_SHIFT       32UL
#define TS_ERASE_TIMEOUT_MS 50U

/* ---- 目录页记录 / 镜像（与 task_store.h 里的字节布局一一对应） ---- */
typedef struct {
    uint16_t id;
    uint16_t off;
    uint16_t len;
    uint16_t crc16;
    uint8_t  flags;
    uint8_t  name_len;
    char     name[TASK_STORE_NAME_MAX];
} ts_ent_t;

typedef struct {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  count;
    uint16_t seq;
    uint8_t  rsv[2];
    ts_ent_t ent[TASK_STORE_SLOTS];
} ts_dir_t;

/* 布局漂移必须是编译错误：40 B 记录 / 8 字节对齐的双字编程 / 目录页放得下 */
typedef char ts_ent_size_ck[(sizeof(ts_ent_t) == TASK_STORE_ENTRY_LEN) ? 1 : -1];
typedef char ts_dir_hdr_ck[((TASK_STORE_DIR_HDR_LEN + TASK_STORE_SLOTS * TASK_STORE_ENTRY_LEN) ==
                            sizeof(ts_dir_t)) ? 1 : -1];
typedef char ts_dir_dw_ck[((sizeof(ts_dir_t) % 8u) == 0u) ? 1 : -1];
typedef char ts_prog_dw_ck[((TASK_STORE_PROG_BYTES % 8u) == 0u) ? 1 : -1];

/* ---- 目录缓存（上电扫描一次，之后由本模块自己维护） ---- */
static uint8_t  s_present[TASK_STORE_SLOTS];
static uint16_t s_off[TASK_STORE_SLOTS];
static uint16_t s_len[TASK_STORE_SLOTS];
static uint16_t s_crc[TASK_STORE_SLOTS];
static uint8_t  s_flags[TASK_STORE_SLOTS];
static uint8_t  s_nlen[TASK_STORE_SLOTS];
static char     s_name[TASK_STORE_SLOTS][TASK_STORE_NAME_MAX + 1u];
static uint16_t s_seq;

/* ---- 工作区 ---- */
static uint8_t  s_stage[TASK_STORE_CAP];            /* T_TASK_PUT 的 RAM 暂存 */
static uint16_t s_stage_hi;
static uint8_t  s_prog[TASK_STORE_PROG_BYTES];      /* 待写的程序区镜像 */
static ts_dir_t s_new;                              /* 待写的目录镜像 */
static ts_dir_t s_dir;                              /* 最近一次读/写过的目录 */

/* ------------------------------------------------------------------ 地址与 Flash 原语 */

static uintptr_t ts_page_addr(uint32_t page)
{
    return (uintptr_t)(TS_FLASH_BASE + (page * TASK_STORE_PAGE_SIZE));
}

static const uint8_t *ts_prog(void)
{
    return (const uint8_t *)ts_page_addr(TASK_STORE_PROG_PAGE0);
}

/* 擦一页：照抄 firmware/App/Svc/nvram.c 的手写 CR 序列（HAL_FLASHEx_Erase 在本板静默失败）。
 * 返回 1 = 没看到错误标志（真正的"擦干净了"由调用方按页首字确认）。 */
static uint8_t ts_page_erase(uint32_t page)
{
    uint8_t  dcache = (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) ? 1U : 0U;
    uint32_t bank_page = page - TS_BANK_SHIFT;
    uint32_t t0;

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG | FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER |
                         FLASH_CR_MER1 | FLASH_CR_MER2);
    SET_BIT(FLASH->CR, FLASH_CR_BKER);                                   /* 页号 ≥ 32 ⇒ BKER=1 */
    MODIFY_REG(FLASH->CR, FLASH_CR_PNB, (bank_page << FLASH_CR_PNB_Pos));
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    SET_BIT(FLASH->CR, FLASH_CR_STRT);

    t0 = HAL_GetTick();
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if ((uint32_t)(HAL_GetTick() - t0) > TS_ERASE_TIMEOUT_MS) {
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
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    return 1U;
}

/* 按双字编程：len 必须是 8 的整数倍，addr 必须 8 字节对齐 */
static uint8_t ts_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; i += 8U, addr += 8U) {
        uint64_t w;

        memcpy(&w, &src[i], sizeof(w));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, w) != HAL_OK) {
            return 0U;
        }
    }
    return 1U;
}

/* ------------------------------------------------------------------ 目录缓存 */

static void cache_reset(void)
{
    memset(s_present, 0, sizeof(s_present));
    memset(s_off, 0, sizeof(s_off));
    memset(s_len, 0, sizeof(s_len));
    memset(s_crc, 0, sizeof(s_crc));
    memset(s_flags, 0, sizeof(s_flags));
    memset(s_nlen, 0, sizeof(s_nlen));
    memset(s_name, 0, sizeof(s_name));
    s_seq = 0U;
}

/* 目录页镜像 → 缓存。**逐条独立判定**：坏记录标成空槽（别的记录照收）。
 * 只有记录头（magic/ver/count）坏了才是"整片空" —— 那种情况下 count 本身不可信。 */
static void cache_load(const ts_dir_t *d)
{
    uint8_t i;

    cache_reset();
    if (d->magic != TASK_STORE_DIR_MAGIC || d->ver != TASK_STORE_VER) {
        return;
    }
    if (d->count > TASK_STORE_SLOTS) {
        return;
    }
    s_seq = d->seq;

    for (i = 0U; i < d->count; i++) {
        const ts_ent_t *e = &d->ent[i];
        const uint8_t  *img;
        uint8_t         nl;

        if (e->id >= TASK_STORE_SLOTS || s_present[e->id] != 0U) {
            continue;                                   /* 槽号越界 / 目录里有重复 id */
        }
        if (e->len < TASK_HDR_LEN || e->len > TASK_STORE_CAP) {
            continue;
        }
        if ((e->off & 7U) != 0U) {                      /* 双字编程的硬要求 */
            continue;
        }
        if (((uint32_t)e->off + (uint32_t)e->len) > TASK_STORE_PROG_BYTES) {
            continue;
        }
        if (e->name_len > TASK_STORE_NAME_MAX) {
            continue;                                   /* 名字长度越界 = 结构不合格 */
        }
        nl = e->name_len;
        img = ts_prog() + e->off;
        if (TaskProg_Validate(img, e->len) != TASK_OK) {
            continue;
        }
        if (e->crc16 != TaskProg_Crc16(&img[TASK_HDR_LEN], (uint32_t)e->len - TASK_HDR_LEN)) {
            continue;
        }

        s_present[e->id] = 1U;
        s_off[e->id]     = e->off;
        s_len[e->id]     = e->len;
        s_crc[e->id]     = e->crc16;
        s_flags[e->id]   = (uint8_t)(e->flags & TASK_STORE_FLAG_AUTO);
        s_nlen[e->id]    = nl;
        if (nl != 0U) {
            memcpy(s_name[e->id], e->name, nl);
        }
        s_name[e->id][nl] = '\0';
    }
}

/* 车在动 / 解释器在跑 ⇒ 绝不允许擦写（擦一页 CPU 停几十毫秒，控制环会失控）。
 * 判据与 nvram.c:449 的 nvram_motion_active 同源，外加解释器自己在跑这一条。 */
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

/* ------------------------------------------------------------------ 重排 + 落盘 */

/**
 * 按当前缓存把"目录 + 程序区"重排一遍：
 *   target 槽：adding=1 用入参里的新镜像；adding=0 表示删掉它；
 *   其余在场任务从 Flash 里原样搬过来（保持目录顺序、紧凑摆放、8 字节对齐）。
 * 结果写进 s_new / s_prog。返回 TASK_STORE_OK 或 TASK_STORE_E_PARAM（程序区装不下）。
 */
static int store_build(uint8_t target, const uint8_t *img, uint16_t len, uint16_t crc,
                       uint8_t flags, const char *name, uint8_t name_len, uint8_t adding)
{
    uint32_t used = 0U;
    uint8_t  i;
    uint8_t  n = 0U;

    memset(s_prog, 0xFF, sizeof(s_prog));
    memset(&s_new, 0xFF, sizeof(s_new));
    s_new.magic = TASK_STORE_DIR_MAGIC;
    s_new.ver   = TASK_STORE_VER;
    s_new.seq   = (uint16_t)(s_seq + 1U);
    s_new.count = 0U;

    for (i = 0U; i < TASK_STORE_SLOTS; i++) {
        const uint8_t *src;
        const char    *nm;
        uint16_t       l;
        uint8_t        fl;
        uint8_t        nl;
        uint32_t       off;
        ts_ent_t      *e;

        if (i == target) {
            if (adding == 0U) {
                continue;
            }
            src = img;
            l   = len;
            fl  = (uint8_t)(flags & TASK_STORE_FLAG_AUTO);
            nm  = name;
            nl  = name_len;
        } else if (s_present[i] != 0U) {
            src = ts_prog() + s_off[i];
            l   = s_len[i];
            fl  = (uint8_t)(s_flags[i] & TASK_STORE_FLAG_AUTO);
            nm  = s_name[i];
            nl  = s_nlen[i];
        } else {
            continue;
        }

        if (nl > TASK_STORE_NAME_MAX) {
            nl = TASK_STORE_NAME_MAX;
        }
        if (nm == NULL) {
            nl = 0U;
        }
        off = (used + 7U) & ~7U;                        /* 8 字节对齐 */
        if ((off + (uint32_t)l) > TASK_STORE_PROG_BYTES) {
            return TASK_STORE_E_PARAM;                  /* 程序区装不下，一个字节都不动 Flash */
        }
        if (l != 0U && src != NULL) {
            memcpy(&s_prog[off], src, l);
        }
        e            = &s_new.ent[n];
        e->id        = i;
        e->off       = (uint16_t)off;
        e->len       = l;
        e->crc16     = (i == target) ? crc : s_crc[i];
        e->flags     = fl;
        e->name_len  = nl;
        if (nl != 0U) {
            memcpy(e->name, nm, nl);
        }
        used = off + (uint32_t)l;
        n++;
    }

    s_new.count = n;
    return TASK_STORE_OK;
}

/* 把 s_new / s_prog 写进 Flash（先擦目录页 → 再擦程序区 → 编程 → 回读复查） */
static int store_flash_write(void)
{
    uint32_t prog_addr = (uint32_t)ts_page_addr(TASK_STORE_PROG_PAGE0);
    uint32_t dir_addr  = (uint32_t)ts_page_addr(TASK_STORE_DIR_PAGE);
    uint32_t i;

    /* 先擦目录页：中断在这里 = 车上"没有任务"（安全） */
    if (ts_page_erase(TASK_STORE_DIR_PAGE) == 0U) {
        return TASK_STORE_E_FLASH;
    }
    for (i = 0U; i < TASK_STORE_PROG_PAGES; i++) {
        if (ts_page_erase(TASK_STORE_PROG_PAGE0 + i) == 0U) {
            return TASK_STORE_E_FLASH;
        }
    }

    /* 擦除静默失败（本板 PNB 越界时不置错误标志）只剩这一道防线：
     * 每页首字必须真的回到 0xFFFFFFFF，否则宁可不写。 */
    if (*(volatile uint32_t *)ts_page_addr(TASK_STORE_DIR_PAGE) != 0xFFFFFFFFUL) {
        return TASK_STORE_E_FLASH;
    }
    for (i = 0U; i < TASK_STORE_PROG_PAGES; i++) {
        if (*(volatile uint32_t *)ts_page_addr(TASK_STORE_PROG_PAGE0 + i) != 0xFFFFFFFFUL) {
            return TASK_STORE_E_FLASH;
        }
    }

    if (ts_program(prog_addr, s_prog, TASK_STORE_PROG_BYTES) == 0U) {
        return TASK_STORE_E_FLASH;
    }
    if (ts_program(dir_addr, (const uint8_t *)&s_new, (uint32_t)sizeof(s_new)) == 0U) {
        return TASK_STORE_E_FLASH;
    }

    /* 回读复查（擦除里已复位 data cache） */
    if (memcmp((const void *)(uintptr_t)prog_addr, s_prog, TASK_STORE_PROG_BYTES) != 0) {
        return TASK_STORE_E_FLASH;
    }
    if (memcmp((const void *)(uintptr_t)dir_addr, &s_new, sizeof(s_new)) != 0) {
        return TASK_STORE_E_FLASH;
    }

    s_dir = s_new;
    cache_load(&s_dir);
    return TASK_STORE_OK;
}

/* ------------------------------------------------------------------ 公共 API */

void TaskStore_Init(void)
{
    memcpy(&s_dir, (const void *)ts_page_addr(TASK_STORE_DIR_PAGE), sizeof(s_dir));
    cache_load(&s_dir);
    TaskStore_StageReset();
}

uint8_t TaskStore_SlotCount(void)
{
    return TASK_STORE_SLOTS;
}

uint8_t TaskStore_Present(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_present[slot] : 0U;
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

uint8_t TaskStore_Page(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS || s_present[slot] == 0U) {
        return TASK_STORE_NO_SLOT;
    }
    return (uint8_t)(TASK_STORE_PROG_PAGE0 + (s_off[slot] / TASK_STORE_PAGE_SIZE));
}

uint8_t TaskStore_BankPage(uint8_t slot)
{
    uint8_t page = TaskStore_Page(slot);

    return (page == TASK_STORE_NO_SLOT) ? TASK_STORE_NO_SLOT : (uint8_t)(page - TS_BANK_SHIFT);
}

uint8_t TaskStore_Name(uint8_t slot, char *out, uint8_t cap)
{
    uint8_t n;
    uint8_t k;

    if (slot >= TASK_STORE_SLOTS || s_present[slot] == 0U) {
        if (out != NULL && cap != 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    n = s_nlen[slot];
    if (out != NULL && cap != 0U) {
        k = (uint8_t)(((uint8_t)(cap - 1U) < n) ? (uint8_t)(cap - 1U) : n);
        if (k != 0U) {
            memcpy(out, s_name[slot], k);
        }
        out[k] = '\0';
    }
    return n;
}

const uint8_t *TaskStore_Image(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS || s_present[slot] == 0U) {
        return NULL;
    }
    return ts_prog() + s_off[slot];
}

uint8_t TaskStore_AutoSlot(void)
{
    uint8_t i;

    for (i = 0U; i < TASK_STORE_SLOTS; i++) {
        if (s_present[i] != 0U && (s_flags[i] & TASK_STORE_FLAG_AUTO) != 0U) {
            return i;
        }
    }
    return TASK_STORE_NO_SLOT;
}

int TaskStore_Load(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (s_present[slot] == 0U) {
        return TASK_STORE_E_EMPTY;
    }
    return TaskRun_Load(TaskStore_Image(slot), (uint32_t)s_len[slot]);
}

void TaskStore_StageReset(void)
{
    s_stage_hi = 0U;
}

uint16_t TaskStore_StageLen(void)
{
    return s_stage_hi;
}

int TaskStore_Stage(uint16_t off, const uint8_t *d, uint16_t n)
{
    if (d == NULL || n == 0U) {
        return TASK_STORE_E_PARAM;
    }
    if (((uint32_t)off + (uint32_t)n) > TASK_STORE_CAP) {
        return TASK_STORE_E_PARAM;
    }
    if (off == 0U) {
        s_stage_hi = 0U;                                /* 新一轮上传：清掉上一轮的残留 */
    }
    memcpy(&s_stage[off], d, n);
    if ((uint16_t)(off + n) > s_stage_hi) {
        s_stage_hi = (uint16_t)(off + n);
    }
    return TASK_STORE_OK;
}

int TaskStore_Commit(uint8_t slot, uint16_t len, uint16_t crc, uint8_t flags,
                     const char *name, uint8_t name_len)
{
    int rc;

    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (len < TASK_HDR_LEN || len > TASK_STORE_CAP) {
        return TASK_STORE_E_PARAM;
    }
    if (s_stage_hi < len) {
        return TASK_STORE_E_PARAM;                      /* 分片没收全 */
    }
    if (name_len != 0U && name == NULL) {
        return TASK_STORE_E_PARAM;
    }
    if (store_busy() != 0U) {
        return TASK_STORE_E_BUSY;
    }
    if (TaskProg_Crc16(&s_stage[TASK_HDR_LEN], (uint32_t)len - TASK_HDR_LEN) != crc) {
        return TASK_STORE_E_CRC;
    }
    if (TaskProg_Validate(s_stage, len) != TASK_OK) {
        return TASK_STORE_E_FORMAT;
    }

    rc = store_build(slot, s_stage, len, crc, flags, name, name_len, 1U);
    if (rc != TASK_STORE_OK) {
        return rc;
    }
    rc = store_flash_write();
    if (rc == TASK_STORE_OK) {
        TaskStore_StageReset();
    }
    return rc;
}

int TaskStore_Erase(uint8_t slot)
{
    int rc;

    if (slot >= TASK_STORE_SLOTS) {
        return TASK_STORE_E_PARAM;
    }
    if (s_present[slot] == 0U) {
        return TASK_STORE_OK;                           /* 幂等：空槽不碰 Flash */
    }
    if (store_busy() != 0U) {
        return TASK_STORE_E_BUSY;
    }

    rc = store_build(slot, NULL, 0U, 0U, 0U, NULL, 0U, 0U);
    if (rc != TASK_STORE_OK) {
        return rc;
    }
    return store_flash_write();
}
