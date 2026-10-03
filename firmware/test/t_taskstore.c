/**
 * @file  t_taskstore.c   主机侧单元测试：任务程序在 Flash 里的存储（App/Svc/task_store.c）
 *
 * 页分配（App/Svc/task_store.h 是唯一权威）：
 *   页 58       目录页      magic/ver/count/seq + 8 条 40 B 记录
 *   页 59..61   程序区     任务镜像紧凑摆放（8 字节对齐）
 *   页 62       空着       属于 nvram 的擦除块 31，任务模块绝不碰
 *   页 63       nvram      本模块绝不碰
 *  （擦除粒度是 4KB 块 = 页号/2，所以存储区落在块 29/30；块 27/28 是固件镜像，
 *    见本文件最后一节的回归用例。）
 *
 * 盯六件事：
 *   1) **布局**：目录页每个字段的字节偏移、程序区里 off 的紧凑与 8 字节对齐、
 *      Image() 指到 Flash（XIP 直读）而不是拷贝；页 63 从头到尾一个字节都不许动。
 *   2) **烧写链路**：分片暂存 → 提交（CRC16 + 格式双重校验）→ 回读；提交前
 *      程序区和目录页必须**真的擦回 0xFF**（假 Flash 不模拟"只能 1→0"，所以
 *      "写前必擦"只能靠 host_flash_page() 直接看页内容来钉）。
 *   3) **容量**：8 槽上限、6144 B 程序区上限；装不下必须报错且一个 bit 都不动。
 *   4) **拒写**：车在动（模式非 IDLE / 速度环使能）或解释器在跑时 Commit/Erase 一律
 *      TASK_STORE_E_BUSY —— 擦一页会把 CPU 停住几十毫秒，正在跑的控制环会直接失控。
 *   5) **坏记录 = 空槽，且逐条独立**：某一槽的记录坏了（镜像翻位 / 头字段坏 / off 越界）
 *      只影响那一槽，别的槽照收；目录头（magic/ver）坏才是整片空。
 *   6) **掉电语义**：重新 Init（= 重启）后任务还在；坏记录绝不能被当成"能跑的任务"。
 *
 * 依赖：真 task_store.c + task_prog.c + task_run.c，控制模块用 host_appstub.c 顶替；
 *      Flash 用 host_hal.c 的假 Flash（VirtualAlloc 真映射到 0x08000000，所以这里
 *      能按绝对地址读页，跟固件里 XIP 直读一模一样）。
 */
#include <stdint.h>
#include <string.h>

#include "motion.h"
#include "task_prog.h"
#include "task_run.h"
#include "task_store.h"
#include "t_common.h"

extern void     host_flash_reset(void);
extern uint8_t *host_flash_page(uint32_t page);
extern int      host_flash_uses_real_addr(void);
/* 上锁语义（host_hal.c 的假 Flash 照真板演：LOCK=1 时硬件忽略 FLASH->CR 写） */
extern int      host_flash_locked(void);
extern int      host_flash_try_erase_page(uint32_t page);
extern void     host_flash_unlock(void);
extern void     host_flash_lock(void);

/* ---- host_appstub.c 的观测点 ---- */
extern int stub_motion_mode;
extern int stub_sc_enabled;

#define DIR_PAGE         TASK_STORE_DIR_PAGE                       /* 58 */
#define PROG_PAGE0       TASK_STORE_PROG_PAGE0                     /* 59 */
#define SNAP_PAGES       6u                                        /* 页 58..63（存储区 + 空闲页 62 + nvram 页 63） */
#define FW_PAGE0         54u    /* 真板上块 27 = 页 54/55（App 的字体/数据页） */
#define FW_PAGE1         57u    /* 块 28 = 页 56/57，页 56 是 App 镜像的最后一页 */
#define PROG_TAG_OFF     22u    /* build_prog() 把 tag 放在第一个块的 p0（12+8+2） */

/* ------------------------------------------------------------------ 小工具 */

static uint8_t g_snap[SNAP_PAGES][2048];

static const uint8_t *dir_page(void)
{
    return host_flash_page(DIR_PAGE);
}

/* 目录页里的第 i 条记录（40 B，只读视角） */
static const uint8_t *dir_ent(uint32_t i)
{
    return dir_page() + TASK_STORE_DIR_HDR_LEN + i * TASK_STORE_ENTRY_LEN;
}

/* 故意把目录页写坏时用的可写视角（只有本测试会这么干） */
static uint8_t *dir_page_w(void)
{
    return host_flash_page(DIR_PAGE);
}

static uint8_t *dir_ent_w(uint32_t i)
{
    return dir_page_w() + TASK_STORE_DIR_HDR_LEN + i * TASK_STORE_ENTRY_LEN;
}

static uint16_t dir_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* 页 58..63 全部快照 / 与快照逐字节相同（"一个 bit 都不许变"的判据） */
static void snap_all(void)
{
    uint32_t i;

    for (i = 0U; i < SNAP_PAGES; i++) {
        memcpy(g_snap[i], host_flash_page(DIR_PAGE + i), 2048U);
    }
}

static int snap_same(void)
{
    uint32_t i;

    for (i = 0U; i < SNAP_PAGES; i++) {
        if (memcmp(g_snap[i], host_flash_page(DIR_PAGE + i), 2048U) != 0) {
            return 0;
        }
    }
    return 1;
}

static int page_all_ff(uint32_t page)
{
    const uint8_t *p = host_flash_page(page);
    uint32_t       i;

    if (p == NULL) {
        return 0;
    }
    for (i = 0U; i < 2048U; i++) {
        if (p[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

/* 整页是不是全等于某个字节（本测试用来冒充"固件镜像页"：全 0xA5） */
static int page_all(uint32_t page, uint8_t v)
{
    const uint8_t *p = host_flash_page(page);
    uint32_t       i;

    if (p == NULL) {
        return 0;
    }
    for (i = 0U; i < 2048U; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ 造程序 */

static uint8_t  g_prog[TASK_STORE_CAP + 64U];
static uint16_t g_len;
static uint16_t g_crc;

static void wr_block(uint32_t off, uint8_t op, uint16_t p0, int16_t p1, uint16_t p2, uint16_t p3)
{
    uint16_t u;

    g_prog[off + 0U] = op;
    g_prog[off + 1U] = 0U;
    u = p0;
    g_prog[off + 2U] = (uint8_t)(u & 0xFFU);
    g_prog[off + 3U] = (uint8_t)(u >> 8);
    u = (uint16_t)p1;
    g_prog[off + 4U] = (uint8_t)(u & 0xFFU);
    g_prog[off + 5U] = (uint8_t)(u >> 8);
    u = p2;
    g_prog[off + 6U] = (uint8_t)(u & 0xFFU);
    g_prog[off + 7U] = (uint8_t)(u >> 8);
    u = p3;
    g_prog[off + 8U] = (uint8_t)(u & 0xFFU);
    g_prog[off + 9U] = (uint8_t)(u >> 8);
}

/**
 * 造一个**合法**的任务程序，长度正好 len：
 *   12 B 头 + 8 B 状态表（1 个状态，body 紧跟在状态表后面）+ k 个块 + 尾巴填充。
 * 末尾块是 END（Validate 的 tail 规则只约束 body），第一个块的 p0 放 tag
 * —— 用来区分不同槽里的镜像，抓"张冠李戴"。
 * 尾部填充故意用 0xA5 而不是 0x00/0xFF：真被写进 Flash 的话一眼能看出来。
 */
static void build_prog(uint16_t len, uint16_t tag)
{
    const uint32_t body_off = TASK_HDR_LEN + TASK_STATE_LEN;              /* 20 */
    uint32_t       n_body   = ((uint32_t)len - body_off) / TASK_BLOCK_LEN;
    uint32_t       i;

    CHECK(len >= (body_off + TASK_BLOCK_LEN));
    CHECK(n_body >= 1U);
    if (n_body > TASK_MAX_BLOCKS) {
        n_body = TASK_MAX_BLOCKS;                 /* 剩下的长度当名字/填充区 */
    }

    memset(g_prog, 0xA5, sizeof(g_prog));
    g_prog[0] = (uint8_t)(TASK_PROG_MAGIC & 0xFFU);
    g_prog[1] = (uint8_t)(TASK_PROG_MAGIC >> 8);
    g_prog[2] = (uint8_t)TASK_PROG_VER;
    g_prog[3] = 1U;                               /* n_states */
    g_prog[4] = 0U;                               /* main_state */
    /* 状态表：name_off=0 / enter_off=0 / body_off=20 / n_enter=0 / n_body */
    g_prog[12] = 0U;
    g_prog[13] = 0U;
    g_prog[14] = 0U;
    g_prog[15] = 0U;
    g_prog[16] = (uint8_t)(body_off & 0xFFU);
    g_prog[17] = 0U;
    g_prog[18] = 0U;
    g_prog[19] = (uint8_t)n_body;

    for (i = 0U; i < n_body; i++) {
        uint32_t off = body_off + (i * TASK_BLOCK_LEN);
        uint8_t  op  = ((i + 1U) == n_body) ? (uint8_t)TASK_OP_END : (uint8_t)TASK_OP_SET_SPEED;

        wr_block(off, op, (uint16_t)(tag + i), 0, 0xFFFFU, 0xFFFFU);
    }

    g_prog[8] = (uint8_t)(len & 0xFFU);
    g_prog[9] = (uint8_t)(len >> 8);
    g_len = len;
    g_crc = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], (uint32_t)len - TASK_HDR_LEN);
    g_prog[6] = (uint8_t)(g_crc & 0xFFU);
    g_prog[7] = (uint8_t)(g_crc >> 8);
}

/* 按 chunk 字节一片一片喂进暂存区（模拟桥上分片 T_TASK_PUT） */
static int stage_all(const uint8_t *img, uint16_t len, uint16_t chunk)
{
    uint16_t off = 0U;

    while (off < len) {
        uint32_t left = (uint32_t)len - off;
        uint16_t n    = (uint16_t)((left < chunk) ? left : chunk);
        int      e    = TaskStore_Stage(off, img + off, n);

        if (e != TASK_STORE_OK) {
            return e;
        }
        off = (uint16_t)((uint32_t)off + n);
    }
    return TASK_STORE_OK;
}

/* 造程序 + 分片喂 + 提交，一步到位（chunk=0 表示整包一次喂） */
static int put_slot(uint8_t slot, uint16_t len, uint16_t tag, uint8_t flags,
                    const char *name, uint8_t name_len, uint16_t chunk)
{
    int e;

    build_prog(len, tag);
    TaskStore_StageReset();
    e = stage_all(g_prog, g_len, (chunk == 0U) ? g_len : chunk);
    if (e != TASK_STORE_OK) {
        return e;
    }
    return TaskStore_Commit(slot, g_len, g_crc, flags, name, name_len);
}

/* 从程序区镜像里读回 tag（build_prog 把它写在第一个块的 p0） */
static uint16_t img_tag(const uint8_t *img)
{
    return (uint16_t)(img[PROG_TAG_OFF] | ((uint16_t)img[PROG_TAG_OFF + 1U] << 8));
}

/* 程序区里 [from,to) 是不是全是擦除态（"写前必擦"的判据） */
static int prog_ff(uint32_t from, uint32_t to)
{
    const uint8_t *p = host_flash_page(PROG_PAGE0);
    uint32_t       i;

    for (i = from; i < to; i++) {
        if (p[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

/* ==================================================================== */

int main(void)
{
    uint8_t       name[48];
    uint8_t       long_name[TASK_STORE_NAME_MAX + 8U];
    static uint8_t big[TASK_STORE_CAP];
    uint16_t      crc2 = 0;                /* 槽 2 落盘时的 CRC（后面 g_crc 会被别的程序覆盖） */
    int           i;

    host_flash_reset();                    /* 上电是 0x00，先抹成"已擦除"的 0xFF */
    /* task_store 是按绝对地址直读 Flash 的，主机上必须真映射到 0x08000000 */
    CHECK_EQ_I(host_flash_uses_real_addr(), 1);

    /* ===============================================================
     * ① 空 Flash 首次扫描：8 个槽都空，目录页/程序区/nvram 页全 0xFF
     * =============================================================== */
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_SlotCount(), TASK_STORE_SLOTS);
    CHECK_EQ_I(TASK_STORE_SLOTS, 8);
    CHECK_EQ_I(TASK_STORE_CAP, 4096);
    CHECK_EQ_I(TASK_STORE_PROG_BYTES, 6144);
    CHECK_EQ_I(TASK_STORE_PROG_PAGES, 3);
    CHECK_EQ_I(TASK_STORE_ENTRY_LEN, 40);
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 0);
        CHECK(TaskStore_Image((uint8_t)i) == NULL);
        CHECK_EQ_I(TaskStore_Len((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Crc((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Flags((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Page((uint8_t)i), TASK_STORE_NO_SLOT);
        CHECK_EQ_I(TaskStore_Block((uint8_t)i), TASK_STORE_NO_SLOT);
        CHECK_EQ_I(TaskStore_Name((uint8_t)i, (char *)name, sizeof name), 0);
        CHECK_EQ_I(name[0], '\0');
        CHECK_EQ_I(TaskStore_Load((uint8_t)i), TASK_STORE_E_EMPTY);
    }
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);
    CHECK_EQ_I(TaskStore_StageLen(), 0);
    CHECK_EQ_I(TaskStore_Erase(9u), TASK_STORE_E_PARAM);     /* 越界槽号也不许动 Flash */
    CHECK_EQ_I(TaskStore_Load(8u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Present(8u), 0);
    CHECK(page_all_ff(DIR_PAGE));
    CHECK(page_all_ff(PROG_PAGE0));
    CHECK(page_all_ff(PROG_PAGE0 + 1U));
    CHECK(page_all_ff(PROG_PAGE0 + 2U));
    CHECK(page_all_ff(63u));                                 /* nvram 页 */

    /* ===============================================================
     * ② 分片烧进槽 1（7 字节一片，故意不对齐）：回读、XIP 直读、目录页字段
     * =============================================================== */
    CHECK_EQ_I(put_slot(1u, 40u, 0x1100u, 0u, "循迹", 6u, 7u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Present(1u), 1);
    CHECK_EQ_I(TaskStore_Len(1u), 40);
    CHECK_EQ_I(TaskStore_Crc(1u), g_crc);
    CHECK_EQ_I(TaskStore_Flags(1u), 0);
    CHECK_EQ_I(TaskStore_Name(1u, (char *)name, sizeof name), 6);
    CHECK(memcmp(name, "循迹", 6u) == 0);
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 0);
    CHECK_EQ_I(TaskStore_Page(1u), 59);
    CHECK_EQ_I(TaskStore_Block(1u), 29);
    /* 第一个任务的 off 是 0（紧凑摆放），Image 就是程序区开头，XIP 直读 */
    CHECK(TaskStore_Image(1u) == (const uint8_t *)host_flash_page(PROG_PAGE0));
    CHECK(memcmp(TaskStore_Image(1u), g_prog, 40u) == 0);
    CHECK_EQ_I(img_tag(TaskStore_Image(1u)), 0x1100);
    CHECK_EQ_I(TaskStore_Load(1u), 0);                 /* 载入解释器：格式层也认 */
    CHECK_EQ_I(TaskRun_Loaded(), 1);
    CHECK_EQ_I(TaskRun_ProgLen(), 40);
    TaskRun_Stop();

    /* 目录页：头 8 B + 第 0 条记录 40 B，每个字段都在文档写的位置上 */
    CHECK_EQ_I(dir_u16(dir_page()), TASK_STORE_DIR_MAGIC);
    CHECK_EQ_I(dir_page()[2], TASK_STORE_VER);
    CHECK_EQ_I(dir_page()[3], 1);                      /* count */
    CHECK_EQ_I(dir_u16(dir_page()), 0x5453);
    CHECK_EQ_I(dir_u16(dir_page() + 4U), 1);           /* seq：一次成功落盘 */
    CHECK_EQ_I(dir_page()[6], 0xFF);                   /* 保留字节 = 擦除态 */
    CHECK_EQ_I(dir_page()[7], 0xFF);
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 0U), 1);          /* id = 槽号 */
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 2U), 0);          /* off = 0 */
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 4U), 40);         /* len */
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 6U), g_crc);
    CHECK_EQ_I(dir_ent(0U)[8], 0);                     /* flags */
    CHECK_EQ_I(dir_ent(0U)[9], 6);                     /* name_len */
    CHECK(memcmp(dir_ent(0U) + 10U, "循迹", 6u) == 0);
    CHECK_EQ_I(dir_ent(0U)[39], 0xFF);                 /* 名字区剩余 = 擦除态 */
    /* 目录里没有第二条：整条 + 后面全是擦除态 */
    CHECK_EQ_I(dir_ent(1U)[0], 0xFF);
    CHECK_EQ_I(dir_page()[TASK_STORE_DIR_HDR_LEN + TASK_STORE_ENTRY_LEN], 0xFF);
    CHECK_EQ_I(dir_page()[2047], 0xFF);
    /* 程序区：镜像在头 40 B，后面没多写；另外两页没被碰 */
    CHECK(memcmp(host_flash_page(PROG_PAGE0), g_prog, 40u) == 0);
    CHECK_EQ_I(host_flash_page(PROG_PAGE0)[40], 0xFF);
    CHECK(page_all_ff(PROG_PAGE0 + 1U));
    CHECK(page_all_ff(PROG_PAGE0 + 2U));
    CHECK(page_all_ff(63u));

    /* 覆盖写同一个槽：改名 + 换 tag，目录里还是 1 条 */
    CHECK_EQ_I(put_slot(1u, 40u, 0x2200u, 0u, "new", 3u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_page()[3], 1);
    CHECK_EQ_I(dir_u16(dir_page() + 4U), 2);           /* seq 涨到 2 */
    CHECK_EQ_I(dir_ent(0U)[9], 3);
    CHECK(memcmp(dir_ent(0U) + 10U, "new", 3u) == 0);
    CHECK_EQ_I(dir_ent(0U)[13], 0xFF);                 /* 旧名字的残留必须没 */
    CHECK_EQ_I(img_tag(TaskStore_Image(1u)), 0x2200);

    /* ===============================================================
     * ③ 紧凑摆放 + 8 字节对齐：50 B 之后的任务从 off=56 开始，中间是擦除态
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(0u, 50u, 0x3000u, 0u, "a", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(1u, 40u, 0x3100u, 0u, "b", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(2u, 40u, 0x3200u, 0u, "c", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_page()[3], 3);
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 2U), 0);
    CHECK_EQ_I(dir_u16(dir_ent(1U) + 2U), 56);         /* 50 → 对齐到 56 */
    CHECK_EQ_I(dir_u16(dir_ent(2U) + 2U), 96);
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 4U), 50);
    CHECK_EQ_I(dir_u16(dir_ent(1U) + 0U), 1);
    CHECK_EQ_I(dir_u16(dir_ent(2U) + 0U), 2);
    CHECK(prog_ff(50u, 56u));                          /* 对齐补的是 0xFF，不是垃圾 */
    CHECK_EQ_I(img_tag(TaskStore_Image(0u)), 0x3000u);
    CHECK_EQ_I(img_tag(TaskStore_Image(1u)), 0x3100u);
    CHECK_EQ_I(img_tag(TaskStore_Image(2u)), 0x3200u);
    CHECK(TaskStore_Image(1u) == (host_flash_page(PROG_PAGE0) + 56U));
    CHECK_EQ_I(TaskStore_Page(0u), 59);
    CHECK_EQ_I(TaskStore_Page(1u), 59);
    CHECK_EQ_I(TaskStore_Page(2u), 59);
    CHECK_EQ_I(TaskStore_Len(0u), 50);
    CHECK_EQ_I(TaskStore_Load(0u), 0);
    CHECK_EQ_I(TaskRun_ProgLen(), 50);
    TaskRun_Stop();

    /* ===============================================================
     * ④ 覆盖写 = 先擦后写：900 B 换成 40 B 后，旧镜像必须一点不剩
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(0u, 900u, 0x4000u, 0u, "big", 3u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Len(0u), 900);
    CHECK_EQ_I(host_flash_page(PROG_PAGE0)[899], 0xA5);   /* 尾巴真写进去了 */
    CHECK_EQ_I(host_flash_page(PROG_PAGE0 + 1U)[0], 0xFF);
    CHECK_EQ_I(put_slot(0u, 40u, 0x4100u, 0u, "s", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Len(0u), 40);
    CHECK_EQ_I(dir_page()[3], 1);
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 2U), 0);
    CHECK(prog_ff(40u, 2048u));                        /* 旧的 900 B 全没了 */
    CHECK(page_all_ff(PROG_PAGE0 + 1U));
    CHECK(page_all_ff(PROG_PAGE0 + 2U));
    CHECK_EQ_I(img_tag(TaskStore_Image(0u)), 0x4100u);
    /* 目录页也是整页重写：count 之后到页尾全是擦除态 */
    CHECK_EQ_I(dir_page()[TASK_STORE_DIR_HDR_LEN + TASK_STORE_ENTRY_LEN], 0xFF);
    CHECK_EQ_I(dir_page()[2047], 0xFF);

    /* ===============================================================
     * ⑤ 目录满 8 个：off = 0,40,…,280，每条镜像各是自己的
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        char n[2];

        n[0] = (char)('A' + i);
        n[1] = '\0';
        CHECK_EQ_I(put_slot((uint8_t)i, 40u, (uint16_t)(0x5000u + (uint16_t)i * 0x100u), 0u,
                            n, 1u, 0u), TASK_STORE_OK);
    }
    CHECK_EQ_I(dir_page()[3], 8);
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        char n[2];

        n[0] = (char)('A' + i);
        n[1] = '\0';
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 1);
        CHECK_EQ_I(TaskStore_Len((uint8_t)i), 40);
        CHECK_EQ_I(dir_u16(dir_ent((uint32_t)i) + 0U), i);
        CHECK_EQ_I(dir_u16(dir_ent((uint32_t)i) + 2U), (uint32_t)i * 40u);
        CHECK_EQ_I(img_tag(TaskStore_Image((uint8_t)i)), 0x5000u + (uint16_t)i * 0x100u);
        CHECK_EQ_I(TaskStore_Name((uint8_t)i, (char *)name, sizeof name), 1);
        CHECK(memcmp(name, n, 1u) == 0);
    }
    CHECK_EQ_I(dir_u16(dir_page() + 4U), 8);           /* seq = 8 次落盘 */
    CHECK_EQ_I(TaskStore_Erase(8u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(dir_page()[3], 8);
    /* 目录满 8 个也不影响覆盖写：把槽 7 换成一个大程序（off 仍是 280 ⇒ 还在第 0 页里） */
    CHECK_EQ_I(put_slot(7u, 2048u, 0x5100u, 0u, "z", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Len(7u), 2048);
    CHECK_EQ_I(TaskStore_Page(7u), 59);                /* off=280 ⇒ 第 0 页 */
    CHECK_EQ_I(TaskStore_Block(7u), 29u);

    /* ===============================================================
     * ⑥ 容量：3 × 2000 B 装满 6144 B 程序区，第 4 个必须被挡且一字未动
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(0u, 2000u, 0x6000u, 0u, "p0", 2u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(1u, 2000u, 0x6100u, 0u, "p1", 2u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(2u, 2000u, 0x6200u, 0u, "p2", 2u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_u16(dir_ent(1U) + 2U), 2000);
    CHECK_EQ_I(dir_u16(dir_ent(2U) + 2U), 4000);
    CHECK_EQ_I(TaskStore_Page(1u), 59);                /* 2000/2048 仍在第 0 页 */
    CHECK_EQ_I(TaskStore_Page(2u), 60);                /* 4000 → 第 1 页 */
    CHECK_EQ_I(img_tag(TaskStore_Image(1u)), 0x6100u);
    CHECK_EQ_I(img_tag(TaskStore_Image(2u)), 0x6200u);
    snap_all();
    build_prog(200u, 0x6300u);
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(3u, g_len, g_crc, 0u, "p3", 2u), TASK_STORE_E_PARAM);
    CHECK(snap_same());                                /* 一个 bit 都没变 */
    CHECK_EQ_I(TaskStore_Present(3u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    CHECK_EQ_I(TaskStore_Len(2u), 2000);
    /* 塞不下之后再删一个：空间让出来，新的就能进去（而且 off 会补到空位） */
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(3u, 200u, 0x6300u, 0u, "p3", 2u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Len(3u), 200);
    CHECK_EQ_I(dir_page()[3], 3);

    /* ===============================================================
     * ⑦ 坏输入全部挡在 Flash 之外
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(1u, 40u, 0x7000u, 0u, "keep", 4u, 0u), TASK_STORE_OK);
    snap_all();

    /* CRC 错 */
    build_prog(40u, 0x7100u);
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, (uint16_t)(g_crc ^ 0x0001u), 0u, "x", 1u),
               TASK_STORE_E_CRC);
    CHECK(snap_same());

    /* len 太小 / 太大 / 0 */
    CHECK_EQ_I(TaskStore_Commit(1u, 0u, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Commit(1u, 4u, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)(TASK_STORE_CAP + 1u), g_crc, 0u, "x", 1u),
               TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)(g_len + 8u), g_crc, 0u, "x", 1u),
               TASK_STORE_E_PARAM);                    /* 暂存里没有那么多字节 */
    /* 槽号越界 / name=NULL 但给了长度 */
    CHECK_EQ_I(TaskStore_Commit(8u, g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, NULL, 3u), TASK_STORE_E_PARAM);
    CHECK(snap_same());

    /* 分片没收全：只喂前半段 */
    TaskStore_StageReset();
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 8u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), 8);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK(snap_same());

    /* 偏移 0 的片 = 新一轮上传 ⇒ 顺手清掉上一轮的残留 */
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), g_len);
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 12u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), 12);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK(snap_same());

    /* 暂存越界 / 空指针 / 零长 */
    CHECK_EQ_I(TaskStore_Stage((uint16_t)(TASK_STORE_CAP - 4u), big, 8u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Stage((uint16_t)(TASK_STORE_CAP - 8u), big, 8u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Stage(0u, NULL, 4u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 0u), TASK_STORE_E_PARAM);
    TaskStore_StageReset();
    CHECK_EQ_I(TaskStore_StageLen(), 0);

    /* 格式坏（magic 翻一位；CRC 不覆盖头 12 字节，所以 CRC 仍然"对"） */
    build_prog(40u, 0x7200u);
    g_prog[0] ^= 0xFFu;
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_FORMAT);
    CHECK(snap_same());
    g_prog[0] ^= 0xFFu;

    /* 格式坏之"body 末尾不是跳转/结束"（Validate 的第二条判据） */
    build_prog(40u, 0x7300u);
    wr_block(30u, TASK_OP_SET_SPEED, 0x1234u, 0, 0xFFFFU, 0xFFFFU);   /* 末尾换成普通块 */
    g_crc = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], (uint32_t)g_len - TASK_HDR_LEN);
    g_prog[6] = (uint8_t)(g_crc & 0xFFU);
    g_prog[7] = (uint8_t)(g_crc >> 8);
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_FORMAT);
    CHECK(snap_same());

    /* ===============================================================
     * ⑧ 拒写：车在动 / 解释器在跑
     * =============================================================== */
    build_prog(40u, 0x7400u);
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, g_len, g_len), TASK_STORE_OK);
    snap_all();

    stub_motion_mode = MOTION_TRACE;
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    CHECK(snap_same());
    CHECK_EQ_I(TaskStore_Present(1u), 1);

    stub_motion_mode = MOTION_IDLE;
    stub_sc_enabled  = 1;
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    CHECK(snap_same());
    stub_sc_enabled = 0;

    /* 解释器在跑（哪怕电机模式是 IDLE）同样拒写 */
    CHECK_EQ_I(TaskStore_Load(1u), 0);
    CHECK_EQ_I(TaskRun_Start(1u), 0);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    CHECK(snap_same());
    TaskRun_Stop();
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* 停下来了 ⇒ 这次能写，名字也换掉 */
    CHECK_EQ_I(TaskStore_Commit(1u, g_len, g_crc, 0u, "换了", 6u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Name(1u, (char *)name, sizeof name), 6);
    CHECK(memcmp(name, "换了", 6u) == 0);

    /* ===============================================================
     * ⑨ 名字截断 / flags 只留 bit0 / 开机自启取最小 id
     * =============================================================== */
    for (i = 0; i < (int)sizeof long_name; i++) {
        long_name[i] = (uint8_t)('a' + (i % 26));
    }
    CHECK_EQ_I(put_slot(0u, 40u, 0x9000u, 0xFFu, (const char *)long_name,
                        (uint8_t)sizeof long_name, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Name(0u, (char *)name, sizeof name), TASK_STORE_NAME_MAX);
    CHECK(memcmp(name, long_name, TASK_STORE_NAME_MAX) == 0);
    CHECK_EQ_I(name[TASK_STORE_NAME_MAX], '\0');
    CHECK_EQ_I(TaskStore_Flags(0u), TASK_STORE_FLAG_AUTO);      /* 只留 bit0 */
    CHECK_EQ_I(dir_ent(0U)[9], TASK_STORE_NAME_MAX);            /* 目录里也只存 30 */
    /* 短缓冲读出：截断 + 补 0 */
    CHECK_EQ_I(TaskStore_Name(0u, (char *)name, 8u), TASK_STORE_NAME_MAX);
    CHECK_EQ_I(name[7], '\0');
    CHECK(memcmp(name, long_name, 7u) == 0);
    /* 空槽的短缓冲也要安全 */
    CHECK_EQ_I(TaskStore_Name(5u, (char *)name, 4u), 0);
    CHECK_EQ_I(name[0], '\0');

    CHECK_EQ_I(put_slot(2u, 40u, 0x9100u, TASK_STORE_FLAG_AUTO, "C", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(5u, 40u, 0x9200u, TASK_STORE_FLAG_AUTO, "F", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), 0u);                       /* 0 和 2 和 5 ⇒ 0 */
    CHECK_EQ_I(put_slot(0u, 40u, 0x9000u, 0u, "a", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), 2u);                       /* 只剩 2 和 5 ⇒ 2 */
    CHECK_EQ_I(put_slot(2u, 40u, 0x9100u, 0u, "c", 1u, 0u), TASK_STORE_OK);
    crc2 = g_crc;
    CHECK_EQ_I(TaskStore_AutoSlot(), 5u);
    CHECK_EQ_I(put_slot(5u, 40u, 0x9200u, 0u, "f", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);

    /* ===============================================================
     * ⑩ 掉电重启（重新 Init）后一切照旧；seq 跟着成功落盘次数走
     * =============================================================== */
    {
        uint16_t seq_before = dir_u16(dir_page() + 4U);

        TaskStore_Init();
        CHECK_EQ_I(TaskStore_Present(0u), 1);
        CHECK_EQ_I(TaskStore_Present(1u), 1);
        CHECK_EQ_I(TaskStore_Present(2u), 1);
        CHECK_EQ_I(TaskStore_Present(5u), 1);
        CHECK_EQ_I(TaskStore_Len(0u), 40);
        CHECK_EQ_I(TaskStore_Crc(2u), crc2);
        CHECK_EQ_I(TaskStore_Name(2u, (char *)name, sizeof name), 1);
        CHECK_EQ_I(name[0], 'c');
        CHECK_EQ_I(dir_u16(dir_page() + 4U), seq_before);
        CHECK(page_all_ff(63u));
    }

    /* ===============================================================
     * ⑪ 坏目录 / 坏记录：逐条独立，坏的只废那一槽
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(0u, 40u, 0xA000u, 0u, "0", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(1u, 40u, 0xA100u, 0u, "1", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(2u, 40u, 0xA200u, 0u, "2", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_page()[3], 3);

    /* 11a) 记录里的 id 越界 ⇒ 只废这一条 */
    dir_ent_w(1U)[0] = 99u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    dir_ent_w(1U)[0] = 1u;

    /* 11b) 记录里的 id 与前面重复 ⇒ 后一条被丢，前一条照收 */
    dir_ent_w(1U)[0] = 0u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    dir_ent_w(1U)[0] = 1u;

    /* 11c) len 超上限 / 等于 0 */
    dir_ent_w(1U)[4] = (uint8_t)((TASK_STORE_CAP + 1u) & 0xFFu);
    dir_ent_w(1U)[5] = (uint8_t)((TASK_STORE_CAP + 1u) >> 8);
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    dir_ent_w(1U)[4] = 0u;
    dir_ent_w(1U)[5] = 0u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    dir_ent_w(1U)[4] = 40u;
    dir_ent_w(1U)[5] = 0u;

    /* 11d) off 没对齐（双字编程的硬要求） */
    dir_ent_w(1U)[2] = 1u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    dir_ent_w(1U)[2] = 40u;
    dir_ent_w(1U)[3] = 0u;

    /* 11e) off + len 超过程序区 */
    dir_ent_w(1U)[2] = (uint8_t)((TASK_STORE_PROG_BYTES - 8u) & 0xFFu);
    dir_ent_w(1U)[3] = (uint8_t)((TASK_STORE_PROG_BYTES - 8u) >> 8);
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    dir_ent_w(1U)[2] = 40u;
    dir_ent_w(1U)[3] = 0u;

    /* 11f) name_len 超上限 ⇒ 结构不合格（不是"截断着用"：只有上位机才会截断） */
    dir_ent_w(1U)[9] = 200u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    dir_ent_w(1U)[9] = 1u;

    /* 11g) 程序区里镜像翻一位：CRC 那层兜底，只废这一槽 */
    host_flash_page(PROG_PAGE0)[20] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    CHECK_EQ_I(TaskStore_Image(0u), NULL);
    CHECK_EQ_I(TaskStore_Load(0u), TASK_STORE_E_EMPTY);
    CHECK_EQ_I(TaskStore_Present(1u), 1);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    host_flash_page(PROG_PAGE0)[20] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);             /* 翻回来又在了：是判据，不是坏页 */

    /* 11h) 镜像头里的 main_state 越界。镜像头那几个字段（magic/ver/n_states/
     *      main_state/len/crc16）**不在任何 CRC 的覆盖范围内**（记录头的 crc16 与镜像
     *      自带的 crc16 都只盖 [12,len)）⇒ 两层 CRC 都发现不了它，唯一挡得住的是
     *      cache_load() 里的 TaskProg_Validate。11g 是 CRC 那层挡的，这一条专门证明
     *      格式校验不是摆设（变异验证：把 TaskProg_Validate 换成恒真，只有这条会红）。 */
    {
        const uint8_t *base = host_flash_page(PROG_PAGE0);

        CHECK_EQ_I(base[40u + 4u], 0u);               /* 槽 1 的镜像：main_state 本来是 0 */
        ((uint8_t *)base)[40u + 4u] = 5u;             /* n_states = 1，5 越界 */
    }
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Load(1u), TASK_STORE_E_EMPTY);
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    host_flash_page(PROG_PAGE0)[40u + 4u] = 0u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 1);

    /* 11h2) 目录里那条记录自己的 crc16 翻一位 ⇒ 只废这一条。
     *       这是"擦完目录页、写了一半就掉电"的真实形态：镜像还完好，所以镜像那层的
     *       CRC 与格式校验都放它过去，唯一能挡的是记录头的 crc16
     *       （变异验证：把 cache_load 里这句比较换成恒真，本条是全场唯一变红的用例）。 */
    dir_ent_w(1U)[6] ^= 0x01u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Load(1u), TASK_STORE_E_EMPTY);
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    dir_ent_w(1U)[6] ^= 0x01u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(1u), 1);             /* 翻回来又在了 */

    /* 11i) count 超过槽数 ⇒ 整片空（count 本身已经不可信） */
    dir_page_w()[3] = 9u;
    TaskStore_Init();
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 0);
    }
    dir_page_w()[3] = 3u;

    /* 11j) 目录 magic / ver 坏 ⇒ 整片空；再写一次就"自愈"（其余任务真的丢了，
     *      但绝不会出现"半个新程序配着旧偏移"那种会跑错轨迹的状态） */
    dir_page_w()[0] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 0);
    CHECK_EQ_I(put_slot(4u, 40u, 0xA400u, 0u, "heal", 4u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_page()[3], 1);                     /* 目录被整页写回 */
    CHECK_EQ_I(TaskStore_Present(4u), 1);
    CHECK_EQ_I(TaskStore_Present(0u), 0);             /* 旧记录确实没了（已经写在文档里） */
    CHECK_EQ_I(dir_u16(dir_page()), TASK_STORE_DIR_MAGIC);
    CHECK_EQ_I(dir_page()[2], TASK_STORE_VER);

    dir_page_w()[2] = 9u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(4u), 0);
    dir_page_w()[2] = TASK_STORE_VER;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(4u), 1);

    /* ===============================================================
     * ⑫ 删除：幂等、不动别的槽、空出来的位置被回收
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    CHECK_EQ_I(put_slot(0u, 40u, 0xB000u, 0u, "0", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(1u, 40u, 0xB100u, 0u, "1", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(put_slot(2u, 40u, 0xB200u, 0u, "2", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(dir_u16(dir_ent(2U) + 2U), 80);
    snap_all();

    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK(TaskStore_Image(1u) == NULL);
    CHECK_EQ_I(TaskStore_Load(1u), TASK_STORE_E_EMPTY);
    CHECK_EQ_I(dir_page()[3], 2);
    CHECK_EQ_I(dir_u16(dir_ent(0U) + 0U), 0);
    CHECK_EQ_I(dir_u16(dir_ent(1U) + 0U), 2);         /* 后面的记录往前顶 */
    CHECK_EQ_I(dir_u16(dir_ent(1U) + 2U), 40);        /* 空间被回收（原来是 80） */
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    CHECK_EQ_I(img_tag(TaskStore_Image(2u)), 0xB200u);
    CHECK(prog_ff(80u, 2048u));
    CHECK_EQ_I(dir_page()[TASK_STORE_DIR_HDR_LEN + 2u * TASK_STORE_ENTRY_LEN], 0xFF);

    /* 幂等：再删一次不再动 Flash（省一次 4 页擦写） */
    snap_all();
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);
    CHECK(snap_same());
    CHECK_EQ_I(TaskStore_Erase(8u), TASK_STORE_E_PARAM);

    /* 全删空之后：AutoSlot 没有、目录 count=0、程序区全 0xFF */
    CHECK_EQ_I(TaskStore_Erase(0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Erase(2u), TASK_STORE_OK);
    CHECK_EQ_I(dir_page()[3], 0);
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);
    CHECK(prog_ff(0u, TASK_STORE_PROG_BYTES));
    CHECK(page_all_ff(PROG_PAGE0 + 1U));
    CHECK(page_all_ff(PROG_PAGE0 + 2U));
    CHECK_EQ_I(TaskStore_Load(0u), TASK_STORE_E_EMPTY);

    /* ===============================================================
     * ⑬ 整个 Flash 被抹掉（全擦/换芯片）：目录和程序区一起空
     * =============================================================== */
    CHECK_EQ_I(put_slot(6u, 40u, 0xC000u, TASK_STORE_FLAG_AUTO, "x", 1u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), 6u);
    host_flash_reset();
    TaskStore_Init();
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Load((uint8_t)i), TASK_STORE_E_EMPTY);
    }
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);
    CHECK_EQ_I(dir_u16(dir_page() + 4U), 0xFFFF);     /* 目录页整页是擦除态 */
    CHECK(page_all_ff(63u));

    /* ===============================================================
     * ⑭ Flash 上锁语义（复位后 FLASH->CR.LOCK = 1，与真板一致）
     *    2026-10-04 真板 Bug 的回归点：task_store.c 漏 HAL_FLASH_Unlock()，
     *    上锁时写 CR 被硬件忽略（SR 不置错、STRT 没被接受）⇒ ts_block_erase()
     *    "成功返回"却一页没擦 ⇒ E_FLASH ⇒ 上位机看到「提交任务被车拒绝：参数错」。
     * =============================================================== */
    host_flash_reset();
    CHECK_EQ_I(host_flash_locked(), 1);                /* 复位值就是上锁 */
    TaskStore_Init();
    CHECK_EQ_I(put_slot(3u, 40u, 0xD000u, 0u, "lock", 4u, 0u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Present(3u), 1);
    CHECK_EQ_I(img_tag(TaskStore_Image(3u)), 0xD000);
    CHECK_EQ_I(host_flash_locked(), 1);                /* 落盘后要自己锁回去 */

    /* 上锁时手写 CR 擦页必须被忽略：页内容一个字都不许变；解锁后同一个请求才真擦得掉 */
    memcpy(dir_page_w(), "MARK", 4u);
    CHECK_EQ_I(host_flash_try_erase_page(DIR_PAGE), 0);      /* 上锁 ⇒ 擦不动 */
    CHECK(memcmp(dir_page(), "MARK", 4u) == 0);
    host_flash_unlock();
    CHECK_EQ_I(host_flash_try_erase_page(DIR_PAGE), 1);      /* 解锁 ⇒ 真擦掉 */
    CHECK_EQ_I(dir_page()[0], 0xFF);
    host_flash_lock();

    /* ===============================================================
     * ⑮ 「别擦掉固件自己」：擦除目标公式错位的真机事故回归
     *    真板 2026-10-04 实测：硬件按 **4KB 块**擦，块号 = PNB，块地址 =
     *    0x08000000 + PNB×0x1000，BKER 被忽略。老代码却写 PNB = 页号 - 32：
     *      擦"页 59" ⇒ PNB 27 ⇒ 块 27 = 页 54/55（App 的字体/数据页）
     *      擦"页 60" ⇒ PNB 28 ⇒ 块 28 = 页 56/57（页 56 是 App 镜像的最后一页）
     *    ⇒ 上位机一提交任务，App 自己立刻 HardFault（PC 0x0800FA90 / 0x0800127A）。
     *    新代码用块号 = 页号/2（存储区只占块 29/30 = 页 58..61），所以这里把块
     *    27/28 全刷成一个假"固件镜像"字节，Commit/Erase 之后必须一个字节都不变。
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    for (i = (int)FW_PAGE0; i <= (int)FW_PAGE1; i++) {
        memset(host_flash_page((uint32_t)i), 0xA5, 2048U);   /* 冒充固件镜像 */
    }
    CHECK_EQ_I(put_slot(1u, 40u, 0xE000u, 0u, "fw", 2u, 0u), TASK_STORE_OK);
    for (i = (int)FW_PAGE0; i <= (int)FW_PAGE1; i++) {
        CHECK(page_all((uint32_t)i, 0xA5));                  /* 固件页没被擦 */
    }
    CHECK_EQ_I(img_tag(TaskStore_Image(1u)), 0xE000u);       /* 而任务确实存进去了 */
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);          /* 删任务同样不碰固件页 */
    for (i = (int)FW_PAGE0; i <= (int)FW_PAGE1; i++) {
        CHECK(page_all((uint32_t)i, 0xA5));
    }

    return T_SUMMARY();
}
