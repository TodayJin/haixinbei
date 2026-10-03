/**
 * @file  t_taskstore.c   主机侧单元测试：任务程序在 Flash 里的存储（App/Svc/task_store.c）
 *
 * 盯四件事：
 *   1) **布局**：槽 0/1/2 = 页 60/61/62，头 40 B 的每个字段都要对；页 63（nvram）绝不碰。
 *   2) **烧写链路**：分片暂存 → 提交（CRC16 + 格式双重校验）→ 回读复查。暂存缺片、
 *      CRC 错、格式坏、槽号越界都必须被挡在 Flash 之外（页内容一个 bit 都不能变）。
 *   3) **拒写**：车在动（模式非 IDLE / 速度环使能 / 陀螺环使能）或任务在跑时，
 *      Commit/Erase 一律 TASK_STORE_E_BUSY —— 擦一页会把 CPU 停住几十毫秒，
 *      正在跑的控制环会直接失控。
 *   4) **掉电与坏记录**：重新 Init（= 重启）后任务还在；写完翻一位就退化成"空槽"，
 *      绝不把半个程序当成能跑的任务（车会照跑，这是最危险的）；槽里的 id 必须
 *      等于槽号，防止把任务拷到别的槽还认。
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

/* ---- host_appstub.c 的观测点 ---- */
extern int stub_motion_mode;
extern int stub_sc_enabled;

#define SLOT_PAGE(slot) (60u + (uint32_t)(slot))

/* ------------------------------------------------------------------ 构造 */

static uint8_t  g_prog[128];
static uint32_t g_len;
static uint16_t g_crc;

static void wr_block(uint32_t off, uint8_t op, uint16_t p0, int16_t p1, uint16_t p2,
                     uint16_t p3)
{
    uint16_t u;

    g_prog[off + 0u] = op;
    g_prog[off + 1u] = 0u;
    u = p0;
    g_prog[off + 2u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 3u] = (uint8_t)(u >> 8);
    u = (uint16_t)p1;
    g_prog[off + 4u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 5u] = (uint8_t)(u >> 8);
    u = p2;
    g_prog[off + 6u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 7u] = (uint8_t)(u >> 8);
    u = p3;
    g_prog[off + 8u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 9u] = (uint8_t)(u >> 8);
}

/* 最小合法任务：1 个状态，body = { SET_MODE(三级串级), END }，全长 40 B */
static void build_prog(void)
{
    uint32_t len = TASK_HDR_LEN + TASK_STATE_LEN + 2u * TASK_BLOCK_LEN;   /* 40 */

    memset(g_prog, 0, sizeof g_prog);
    g_prog[0] = (uint8_t)(TASK_PROG_MAGIC & 0xFFu);
    g_prog[1] = (uint8_t)(TASK_PROG_MAGIC >> 8);
    g_prog[2] = (uint8_t)TASK_PROG_VER;
    g_prog[3] = 1u;                        /* n_states */
    g_prog[4] = 0u;                        /* main_state */
    /* 状态表：name_off=0 / enter_off=0 / body_off=20 / n_enter=0 / n_body=2 */
    g_prog[12] = 0u;
    g_prog[13] = 0u;
    g_prog[14] = 0u;
    g_prog[15] = 0u;
    g_prog[16] = (uint8_t)((TASK_HDR_LEN + TASK_STATE_LEN) & 0xFFu);
    g_prog[17] = 0u;
    g_prog[18] = 0u;
    g_prog[19] = 2u;

    wr_block(20u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_TRACE_GYRO, 0, 0xFFFFu, 0xFFFFu);
    wr_block(30u, TASK_OP_END, 0u, 0, 0xFFFFu, 0xFFFFu);

    g_len = len;
    g_prog[8] = (uint8_t)(len & 0xFFu);
    g_prog[9] = (uint8_t)(len >> 8);
    g_crc = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], len - TASK_HDR_LEN);
    g_prog[6] = (uint8_t)(g_crc & 0xFFu);
    g_prog[7] = (uint8_t)(g_crc >> 8);
}

/* 按 chunk 字节一片一片喂进暂存区（模拟桥上分片 T_TASK_PUT） */
static int stage_all(const uint8_t *img, uint16_t len, uint16_t chunk)
{
    uint16_t off = 0u;

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

static int page_all_ff(uint32_t page)
{
    const uint8_t *p = host_flash_page(page);
    uint32_t i;

    if (p == NULL) {
        return 0;
    }
    for (i = 0u; i < 2048u; i++) {
        if (p[i] != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    uint8_t  name[48];
    uint8_t  before[2048];
    uint8_t *pg;
    int      i;
    uint8_t  long_name[TASK_STORE_NAME_MAX + 8u];
    static uint8_t big[TASK_STORE_CAP];

    host_flash_reset();                    /* 上电是 0x00，先抹成"已擦除"的 0xFF */
    /* task_store 是按绝对地址直读 Flash 的，主机上必须真映射到 0x08000000 */
    CHECK_EQ_I(host_flash_uses_real_addr(), 1);

    build_prog();
    CHECK_EQ_I(g_len, 40);

    /* ===============================================================
     * ① 空 Flash：三个槽都空，AutoSlot 没有，nvram 页没被碰
     * =============================================================== */
    TaskStore_Init();
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 0);
        CHECK(TaskStore_Image((uint8_t)i) == NULL);
        CHECK_EQ_I(TaskStore_Len((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Crc((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Flags((uint8_t)i), 0);
        CHECK_EQ_I(TaskStore_Name((uint8_t)i, (char *)name, sizeof name), 0);
        CHECK_EQ_I(name[0], '\0');
        CHECK_EQ_I(TaskStore_Load((uint8_t)i), TASK_STORE_E_EMPTY);
    }
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);
    CHECK_EQ_I(TaskStore_SlotCount(), TASK_STORE_SLOTS);
    CHECK_EQ_I(TASK_STORE_CAP, 2008);
    CHECK_EQ_I(TaskStore_Page(0u), 60);
    CHECK_EQ_I(TaskStore_Page(1u), 61);
    CHECK_EQ_I(TaskStore_Page(2u), 62);
    CHECK_EQ_I(TaskStore_BankPage(0u), 28);
    CHECK_EQ_I(TaskStore_BankPage(2u), 30);
    CHECK(page_all_ff(SLOT_PAGE(0u)));
    CHECK(page_all_ff(SLOT_PAGE(1u)));
    CHECK(page_all_ff(SLOT_PAGE(2u)));
    CHECK(page_all_ff(63u));

    /* ===============================================================
     * ② 分片烧进槽 1：头字段、回读、直读 Flash、载入解释器全对
     * =============================================================== */
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 7u), TASK_STORE_OK);   /* 7 字节一片，故意不对齐 */
    CHECK_EQ_I(TaskStore_StageLen(), g_len);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "循迹", 6u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Present(1u), 1);
    CHECK_EQ_I(TaskStore_Len(1u), g_len);
    CHECK_EQ_I(TaskStore_Crc(1u), g_crc);
    CHECK_EQ_I(TaskStore_Flags(1u), 0);
    CHECK_EQ_I(TaskStore_Name(1u, (char *)name, sizeof name), 6);
    CHECK(strcmp((char *)name, "循迹") == 0);
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    CHECK_EQ_I(TaskStore_Present(2u), 0);

    {
        const uint8_t *img = TaskStore_Image(1u);

        CHECK(img != NULL);
        /* 镜像就在页 61 的头上（XIP 直读） */
        CHECK(img == (const uint8_t *)(0x08000000UL + 61u * 0x800UL + TASK_STORE_HDR_LEN));
        CHECK(memcmp(img, g_prog, g_len) == 0);
    }
    CHECK_EQ_I(TaskStore_Load(1u), 0);              /* 载入解释器：格式层也认 */
    CHECK_EQ_I(TaskRun_Loaded(), 1);
    CHECK_EQ_I(TaskRun_ProgLen(), g_len);
    TaskRun_Stop();

    pg = host_flash_page(SLOT_PAGE(1u));
    CHECK(pg != NULL);
    CHECK_EQ_I(pg[0] | (pg[1] << 8), TASK_STORE_MAGIC);
    CHECK_EQ_I(pg[2], TASK_STORE_VER);
    CHECK_EQ_I(pg[3], 1);                           /* id = 槽号 */
    CHECK_EQ_I(pg[4] | (pg[5] << 8), g_len);
    CHECK_EQ_I(pg[6] | (pg[7] << 8), g_crc);
    CHECK_EQ_I(pg[8], 0);                           /* flags */
    CHECK_EQ_I(pg[9], 6);                           /* name_len */
    CHECK(memcmp(pg + 10u, "循迹", 6u) == 0);
    CHECK_EQ_I(pg[39], 0xFF);                       /* 名字区剩下的是擦除态 */
    CHECK(memcmp(pg + TASK_STORE_HDR_LEN, g_prog, g_len) == 0);
    CHECK_EQ_I(pg[TASK_STORE_HDR_LEN + g_len], 0xFF);   /* 镜像后面没多写 */
    CHECK_EQ_I(pg[2047], 0xFF);
    CHECK(page_all_ff(63u));                        /* 页 63 留给 nvram */

    /* ===============================================================
     * ③ 坏输入全部挡在 Flash 之外：页内容一个 bit 都不许变
     * =============================================================== */
    memcpy(before, pg, sizeof before);

    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, (uint16_t)(g_crc ^ 0x0001u), 0u, "x", 1u),
               TASK_STORE_E_CRC);
    CHECK(memcmp(pg, before, sizeof before) == 0);

    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)(g_len + 8u), g_crc, 0u, "x", 1u),
               TASK_STORE_E_PARAM);                 /* 长度 > 暂存覆盖范围 */
    CHECK_EQ_I(TaskStore_Commit(1u, 4u, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)(TASK_STORE_CAP + 1u), g_crc, 0u, "x", 1u),
               TASK_STORE_E_PARAM);                 /* 超一页放不下 */
    CHECK_EQ_I(TaskStore_Commit(3u, (uint16_t)g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK(memcmp(pg, before, sizeof before) == 0);

    /* 格式坏（magic 翻一位；CRC 不覆盖头 12 字节，所以 CRC 仍然是"对"的） */
    TaskStore_StageReset();
    g_prog[0] ^= 0xFFu;
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_FORMAT);
    CHECK(memcmp(pg, before, sizeof before) == 0);
    g_prog[0] ^= 0xFFu;                             /* 还原 */

    /* 分片没传够：只喂前半段 */
    TaskStore_StageReset();
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 8u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), 8);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK(memcmp(pg, before, sizeof before) == 0);

    /* 偏移 0 的片 = 新一轮上传 ⇒ 顺手清掉上一轮残留 */
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), g_len);
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 12u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_StageLen(), 12);           /* 被清零后重新计 */
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "x", 1u), TASK_STORE_E_PARAM);
    CHECK(memcmp(pg, before, sizeof before) == 0);

    /* 暂存越界 */
    CHECK_EQ_I(TaskStore_Stage((uint16_t)(TASK_STORE_CAP - 4u), big, 8u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Stage((uint16_t)(TASK_STORE_CAP - 8u), big, 8u), TASK_STORE_OK);   /* 正好到页尾 */
    CHECK_EQ_I(TaskStore_Stage(0u, NULL, 4u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Stage(0u, g_prog, 0u), TASK_STORE_E_PARAM);
    TaskStore_StageReset();

    /* 名字 NULL：合法（当无名任务），但要重写一次槽位 ⇒ 放在后面单独验 */
    /* ===============================================================
     * ④ 拒写：车在动 / 任务在跑
     * =============================================================== */
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);

    stub_motion_mode = MOTION_TRACE;
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    CHECK(memcmp(pg, before, sizeof before) == 0);
    CHECK_EQ_I(TaskStore_Present(1u), 1);

    stub_motion_mode = MOTION_IDLE;
    stub_sc_enabled  = 1;
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    stub_sc_enabled = 0;

    /* 解释器在跑（哪怕电机模式是 IDLE）同样拒写 */
    CHECK_EQ_I(TaskStore_Load(1u), 0);
    CHECK_EQ_I(TaskRun_Start(1u), 0);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "y", 1u), TASK_STORE_E_BUSY);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_E_BUSY);
    TaskRun_Stop();
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* 车停下来了 ⇒ 这次能写，名字也换掉 */
    CHECK_EQ_I(TaskStore_Commit(1u, (uint16_t)g_len, g_crc, 0u, "新的名字", 12u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Name(1u, (char *)name, sizeof name), 12);
    CHECK(memcmp(name, "新的名字", 12u) == 0);

    /* ===============================================================
     * ⑤ 三个槽互不影响 + 名字截断 + flags 只留 bit0
     * =============================================================== */
    for (i = 0; i < (int)sizeof long_name; i++) {
        long_name[i] = (uint8_t)('a' + (i % 26));
    }
    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(0u, (uint16_t)g_len, g_crc, 0xFFu, (const char *)long_name,
                                (uint8_t)sizeof long_name), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Name(0u, (char *)name, sizeof name), TASK_STORE_NAME_MAX);
    CHECK(memcmp(name, long_name, TASK_STORE_NAME_MAX) == 0);
    CHECK_EQ_I(name[TASK_STORE_NAME_MAX], '\0');
    CHECK_EQ_I(TaskStore_Flags(0u), TASK_STORE_FLAG_AUTO);   /* 只留 bit0 */
    /* 短缓冲读出：截断 + 补 0 */
    CHECK_EQ_I(TaskStore_Name(0u, (char *)name, 8u), TASK_STORE_NAME_MAX);
    CHECK_EQ_I(name[7], '\0');
    CHECK(memcmp(name, long_name, 7u) == 0);

    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(2u, (uint16_t)g_len, g_crc, 0u, "B", 1u), TASK_STORE_OK);

    /* 槽 1 的头和镜像没被槽 0/2 的写入影响：字段各自对 */
    CHECK_EQ_I(TaskStore_Len(0u), g_len);
    CHECK_EQ_I(TaskStore_Len(2u), g_len);
    CHECK_EQ_I(TaskStore_Crc(0u), g_crc);
    CHECK_EQ_I(TaskStore_Crc(2u), g_crc);
    CHECK_EQ_I(TaskStore_Name(2u, (char *)name, sizeof name), 1);
    CHECK_EQ_I(name[0], 'B');
    CHECK(memcmp(host_flash_page(SLOT_PAGE(0u)) + TASK_STORE_HDR_LEN, g_prog, g_len) == 0);
    CHECK(memcmp(host_flash_page(SLOT_PAGE(2u)) + TASK_STORE_HDR_LEN, g_prog, g_len) == 0);

    /* ===============================================================
     * ⑥ 开机自启：flags.bit0 里取 id 最小的那个
     * =============================================================== */
    CHECK_EQ_I(TaskStore_AutoSlot(), 0u);           /* 槽 0 带 AUTO */

    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(2u, (uint16_t)g_len, g_crc, TASK_STORE_FLAG_AUTO, "B", 1u),
               TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), 0u);           /* 两个都带 ⇒ 还是 0 */

    TaskStore_StageReset();
    CHECK_EQ_I(stage_all(g_prog, (uint16_t)g_len, 200u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Commit(0u, (uint16_t)g_len, g_crc, 0u, "A", 1u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_AutoSlot(), 2u);           /* 只剩槽 2 */

    /* ===============================================================
     * ⑦ 重启（重新 Init）后任务还在；页 63 依旧没被碰
     * =============================================================== */
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(1u), 1);
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    CHECK_EQ_I(TaskStore_Len(2u), g_len);
    CHECK_EQ_I(TaskStore_Crc(2u), g_crc);
    CHECK_EQ_I(TaskStore_Name(2u, (char *)name, sizeof name), 1);
    CHECK_EQ_I(TaskStore_AutoSlot(), 2u);
    CHECK(page_all_ff(63u));

    /* ===============================================================
     * ⑧ 删除：擦干净、幂等、不动别的槽
     * =============================================================== */
    memcpy(before, host_flash_page(SLOT_PAGE(0u)), sizeof before);
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);
    CHECK_EQ_I(TaskStore_Present(1u), 0);
    CHECK(TaskStore_Image(1u) == NULL);
    CHECK_EQ_I(TaskStore_Load(1u), TASK_STORE_E_EMPTY);
    CHECK(page_all_ff(SLOT_PAGE(1u)));
    CHECK_EQ_I(TaskStore_Erase(1u), TASK_STORE_OK);           /* 幂等 */
    CHECK_EQ_I(TaskStore_Erase(9u), TASK_STORE_E_PARAM);
    CHECK_EQ_I(TaskStore_Present(0u), 1);                     /* 别的槽没动 */
    CHECK_EQ_I(TaskStore_Present(2u), 1);
    CHECK(memcmp(host_flash_page(SLOT_PAGE(0u)), before, sizeof before) == 0);

    /* 空槽此时不能当"有任务"：删掉再载入必须报空 */
    CHECK_EQ_I(TaskStore_Load(1u), TASK_STORE_E_EMPTY);

    /* ===============================================================
     * ⑨ 坏记录 = 空槽（掉电写坏 / 别人乱写都走这条）
     * =============================================================== */
    /* 9a) 镜像里翻一位：CRC 兜底 */
    pg = host_flash_page(SLOT_PAGE(0u));
    pg[TASK_STORE_HDR_LEN + 20u] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    CHECK(TaskStore_Image(0u) == NULL);
    CHECK_EQ_I(TaskStore_Load(0u), TASK_STORE_E_EMPTY);
    CHECK_EQ_I(TaskStore_Present(2u), 1);                     /* 隔壁不受影响 */
    pg[TASK_STORE_HDR_LEN + 20u] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);                     /* 翻回来又在了（是判据，不是坏页） */

    /* 9b) 头 magic 坏 */
    pg[0] ^= 0xFFu;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    pg[0] ^= 0xFFu;

    /* 9c) 版本号不认识 */
    pg[2] = 9u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);
    pg[2] = TASK_STORE_VER;

    /* 9d) 头里的 id 必须等于槽号：整页拷到隔壁槽也不能被认 */
    memcpy(host_flash_page(SLOT_PAGE(1u)), host_flash_page(SLOT_PAGE(0u)), 2048u);
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 1);
    CHECK_EQ_I(TaskStore_Present(1u), 0);                     /* id=0 却躺在槽 1 ⇒ 不认 */

    /* 9e) len 超出一页 */
    pg = host_flash_page(SLOT_PAGE(0u));
    pg[4] = (uint8_t)((TASK_STORE_CAP + 1u) & 0xFFu);
    pg[5] = (uint8_t)((TASK_STORE_CAP + 1u) >> 8);
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(0u), 0);

    /* 9f) 程序镜像头里的 main_state 越界。镜像里那几个头字段（magic/ver/n_states/
     *     main_state/len/crc16）**不在 CRC 覆盖范围内**（CRC 只盖 [12,len)），记录头的
     *     crc16 也只盖 [12,len) ⇒ 两层 CRC 都发现不了它，唯一能挡住的就是 refresh()
     *     里那层 TaskProg_Validate。9a 是 CRC 那层挡的，这一条专门证明格式校验不是摆设
     *     （变异验证：把 TaskProg_Validate 换成恒真，只有这条用例会红）。 */
    pg = host_flash_page(SLOT_PAGE(2u));
    CHECK_EQ_I(pg[TASK_STORE_HDR_LEN + 4u], 0u);      /* 主页状态本来是 0 */
    pg[TASK_STORE_HDR_LEN + 4u] = 5u;                 /* n_states = 1，5 越界 */
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(2u), 0);
    CHECK_EQ_I(TaskStore_Load(2u), TASK_STORE_E_EMPTY);
    pg[TASK_STORE_HDR_LEN + 4u] = 0u;
    TaskStore_Init();
    CHECK_EQ_I(TaskStore_Present(2u), 1);                     /* 改回来又在了（判据，不是坏页） */

    /* ===============================================================
     * ⑩ 整个 Flash 被抹掉（真掉电/全擦）：三个槽一起变空
     * =============================================================== */
    host_flash_reset();
    TaskStore_Init();
    for (i = 0; i < (int)TASK_STORE_SLOTS; i++) {
        CHECK_EQ_I(TaskStore_Present((uint8_t)i), 0);
    }
    CHECK_EQ_I(TaskStore_AutoSlot(), TASK_STORE_NO_SLOT);
    CHECK(page_all_ff(63u));

    return T_SUMMARY();
}
