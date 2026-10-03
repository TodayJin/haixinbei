/**
 * @file  host_taskstub.c
 * @brief 任务存储/解释器的测试替身：给 t_hostlink（协议层）用
 *
 * 为什么要有它：host_link.c 的任务帧（T_TASK_*）只负责**把帧翻译成 TaskStore_ 与
 * TaskRun_ 的调用**，真正的 Flash 语义在 App/Svc/task_store.c，执行语义在 App/Svc/task_run.c ——
 * 那两块由 t_taskstore.c / t_task.c 用真源码测。这里给协议层一套 RAM 假实现，好处是：
 *   - t_hostlink 不用链 Flash 代码（它已经链了真 jy61p.c，而 host_appstub.c 也定义
 *     Yaw/Pitch/Roll，直接链真 task_store.c 还得连真控制模块，符号会打架）；
 *   - 分片上传 / 收尾空帧 / ACK 状态码这些**帧级**约定可以在这里被钉死；
 *   - 提交失败路径（车在动、CRC 对不上）用 stubt_commit_rc 一行就能造。
 *
 * 观测变量命名 stubt_ 开头（t_ 是"task 替身"）。默认值与真实现一致：没有任务、没在跑。
 */
#include <string.h>

#include "task_run.h"
#include "task_store.h"

/* ---- 观测变量（t_hostlink.c extern 读它们） ---- */
int      stubt_stage_resets;
int      stubt_commits;
int      stubt_erases;
int      stubt_loads;
int      stubt_starts;
int      stubt_stops;
uint16_t stubt_last_len;         /* COMMIT 收到的 len */
uint16_t stubt_last_crc;         /* COMMIT 收到的 crc（协议有没有原样搬运） */
uint8_t  stubt_last_flags;       /* COMMIT 收到的 flags（当前约定只能是 0） */
uint8_t  stubt_last_namelen;
char     stubt_last_name[TASK_STORE_NAME_MAX + 1];
uint16_t stubt_last_stage_off;   /* 最近一片 PUT 的偏移 */
uint16_t stubt_last_stage_n;     /* 最近一片 PUT 的长度 */

/* 测试钩子：让下一步操作按指定错误码失败 */
int      stubt_commit_rc = TASK_STORE_OK;
int      stubt_load_rc   = 0;
int      stubt_erase_rc  = TASK_STORE_OK;

/* 解释器观测点：让 t_hostlink 能造出"正在跑第 3 个状态、超时标志置位"这类场景 */
uint8_t  stubt_run_loaded;
uint8_t  stubt_run_running;
uint16_t stubt_run_id;
uint8_t  stubt_run_state = TASK_RUN_OFF;
uint8_t  stubt_run_phase;
uint16_t stubt_run_block;
uint16_t stubt_run_blockcount;
uint32_t stubt_run_elapsed;
uint8_t  stubt_run_flags;

/* ---- 假的三个槽位 + 暂存区 ---- */
static uint8_t  s_img[TASK_STORE_SLOTS][TASK_STORE_CAP];
static uint16_t s_len[TASK_STORE_SLOTS];
static uint16_t s_crc[TASK_STORE_SLOTS];
static uint8_t  s_flags[TASK_STORE_SLOTS];
static uint8_t  s_nlen[TASK_STORE_SLOTS];
static char     s_name[TASK_STORE_SLOTS][TASK_STORE_NAME_MAX + 1u];
static uint8_t  s_present[TASK_STORE_SLOTS];

static uint8_t  s_stage[TASK_STORE_CAP];
static uint16_t s_stage_hi;

/* 测试在每个用例前清空（host_link_init 不碰任务模块） */
void TaskStoreStub_Reset(void);

void TaskStoreStub_Reset(void)
{
    memset(s_img, 0, sizeof(s_img));
    memset(s_len, 0, sizeof(s_len));
    memset(s_crc, 0, sizeof(s_crc));
    memset(s_flags, 0, sizeof(s_flags));
    memset(s_nlen, 0, sizeof(s_nlen));
    memset(s_name, 0, sizeof(s_name));
    memset(s_present, 0, sizeof(s_present));
    memset(s_stage, 0, sizeof(s_stage));
    s_stage_hi = 0u;

    stubt_stage_resets = 0;
    stubt_commits = 0;
    stubt_erases = 0;
    stubt_loads = 0;
    stubt_starts = 0;
    stubt_stops = 0;
    stubt_last_len = 0u;
    stubt_last_crc = 0u;
    stubt_last_flags = 0u;
    stubt_last_namelen = 0u;
    stubt_last_name[0] = '\0';
    stubt_last_stage_off = 0u;
    stubt_last_stage_n = 0u;
    stubt_commit_rc = TASK_STORE_OK;
    stubt_load_rc = 0;
    stubt_erase_rc = TASK_STORE_OK;

    stubt_run_loaded = 0u;
    stubt_run_running = 0u;
    stubt_run_id = 0u;
    stubt_run_state = TASK_RUN_OFF;
    stubt_run_phase = 0u;
    stubt_run_block = 0u;
    stubt_run_blockcount = 0u;
    stubt_run_elapsed = 0u;
    stubt_run_flags = 0u;
}

/* ---- TaskStore_* ---- */
void TaskStore_Init(void) { }

uint8_t TaskStore_SlotCount(void) { return TASK_STORE_SLOTS; }

uint8_t TaskStore_Present(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_present[slot] : 0u;
}

uint16_t TaskStore_Len(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_len[slot] : 0u;
}

uint16_t TaskStore_Crc(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_crc[slot] : 0u;
}

uint8_t TaskStore_Flags(uint8_t slot)
{
    return (slot < TASK_STORE_SLOTS) ? s_flags[slot] : 0u;
}

uint8_t TaskStore_Name(uint8_t slot, char *out, uint8_t cap)
{
    uint8_t n;
    if (slot >= TASK_STORE_SLOTS) return 0u;
    n = s_nlen[slot];
    if (out == NULL || cap == 0u) return n;
    if (n > (uint8_t)(cap - 1u)) n = (uint8_t)(cap - 1u);
    memcpy(out, s_name[slot], n);
    out[n] = '\0';                 /* 真实现也是"必带结尾 0"（调用方要多留 1 字节） */
    return n;
}

const uint8_t *TaskStore_Image(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS || s_present[slot] == 0u) return NULL;
    return s_img[slot];
}

uint8_t TaskStore_AutoSlot(void)
{
    uint8_t i;
    for (i = 0u; i < TASK_STORE_SLOTS; i++) {
        if (s_present[i] != 0u && (s_flags[i] & TASK_STORE_FLAG_AUTO) != 0u) return i;
    }
    return TASK_STORE_NO_SLOT;
}

void TaskStore_StageReset(void)
{
    stubt_stage_resets++;
    s_stage_hi = 0u;
}

int TaskStore_Stage(uint16_t off, const uint8_t *d, uint16_t n)
{
    if (d == NULL && n != 0u) return TASK_STORE_E_PARAM;
    if ((uint32_t)off + (uint32_t)n > TASK_STORE_CAP) return TASK_STORE_E_PARAM;   /* 越界：真实现也拒绝 */
    if (n != 0u) memcpy(&s_stage[off], d, n);
    if (off + n > s_stage_hi) s_stage_hi = (uint16_t)(off + n);
    stubt_last_stage_off = off;
    stubt_last_stage_n = n;
    return TASK_STORE_OK;
}

uint16_t TaskStore_StageLen(void) { return s_stage_hi; }

int TaskStore_Commit(uint8_t slot, uint16_t len, uint16_t crc, uint8_t flags,
                     const char *name, uint8_t name_len)
{
    uint8_t i;
    if (slot >= TASK_STORE_SLOTS) return TASK_STORE_E_PARAM;
    if (name_len > TASK_STORE_NAME_MAX) return TASK_STORE_E_PARAM;
    if (name == NULL && name_len != 0u) return TASK_STORE_E_PARAM;
    if (len > TASK_STORE_CAP || s_stage_hi < len) return TASK_STORE_E_PARAM;

    stubt_last_len = len;
    stubt_last_crc = crc;
    stubt_last_flags = flags;
    stubt_last_namelen = name_len;
    memset(stubt_last_name, 0, sizeof(stubt_last_name));
    if (name != NULL && name_len != 0u) {
        for (i = 0u; i < name_len; i++) {
            char c = name[i];
            if (c == '\0') break;          /* 名字里不该有 0，遇到就当结束 */
            stubt_last_name[i] = c;
        }
    }

    if (stubt_commit_rc != TASK_STORE_OK) return stubt_commit_rc;   /* 测试钩子：车在动/CRC 不过 */

    stubt_commits++;
    memcpy(s_img[slot], s_stage, len);
    s_len[slot] = len;
    s_crc[slot] = crc;
    s_flags[slot] = flags;
    s_nlen[slot] = name_len;
    memset(s_name[slot], 0, sizeof(s_name[slot]));
    memcpy(s_name[slot], stubt_last_name, name_len);
    s_present[slot] = 1u;
    return TASK_STORE_OK;
}

int TaskStore_Erase(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS) return TASK_STORE_E_PARAM;
    stubt_erases++;
    if (stubt_erase_rc != TASK_STORE_OK) return stubt_erase_rc;
    s_present[slot] = 0u;
    s_len[slot] = 0u;
    s_crc[slot] = 0u;
    s_flags[slot] = 0u;
    s_nlen[slot] = 0u;
    return TASK_STORE_OK;
}

int TaskStore_Load(uint8_t slot)
{
    if (slot >= TASK_STORE_SLOTS) return TASK_STORE_E_PARAM;
    if (s_present[slot] == 0u) return TASK_STORE_E_EMPTY;
    stubt_loads++;
    if (stubt_load_rc != 0) return stubt_load_rc;
    stubt_run_loaded = 1u;
    return 0;
}

uint8_t TaskStore_Page(uint8_t slot) { return (uint8_t)(TASK_STORE_PROG_PAGE0 + (slot % TASK_STORE_PROG_PAGES)); }
uint8_t TaskStore_BankPage(uint8_t slot) { return (uint8_t)(TaskStore_Page(slot) - 32u); }

/* ---- TaskRun_* ---- */
int TaskRun_Load(const uint8_t *prog, uint32_t len)
{
    (void)prog; (void)len;
    stubt_run_loaded = 1u;
    return 0;
}

int TaskRun_Start(uint16_t task_id)
{
    if (stubt_run_loaded == 0u) return -1;      /* 真实现的语义：没载入过就拒绝 */
    stubt_starts++;
    stubt_run_running = 1u;
    stubt_run_id = task_id;
    stubt_run_state = 0u;
    stubt_run_phase = 0u;
    stubt_run_block = 0u;
    stubt_run_flags = 0u;
    return 0;
}

void TaskRun_Stop(void)
{
    stubt_stops++;
    stubt_run_running = 0u;
}

void TaskRun_Tick(uint32_t now_ms) { (void)now_ms; }

uint8_t  TaskRun_IsRunning(void) { return stubt_run_running; }
uint16_t TaskRun_Id(void) { return stubt_run_id; }
uint8_t  TaskRun_State(void) { return stubt_run_state; }
uint8_t  TaskRun_Phase(void) { return stubt_run_phase; }
uint16_t TaskRun_Block(void) { return stubt_run_block; }
uint16_t TaskRun_BlockCount(void) { return stubt_run_blockcount; }
uint32_t TaskRun_ElapsedMs(void) { return stubt_run_elapsed; }
uint32_t TaskRun_BlockMs(void) { return 0u; }
uint8_t  TaskRun_Flags(void) { return stubt_run_flags; }
uint8_t  TaskRun_Done(void) { return (uint8_t)((stubt_run_flags & TASK_RUN_F_DONE) != 0u); }
uint8_t  TaskRun_Loaded(void) { return stubt_run_loaded; }
uint16_t TaskRun_ProgLen(void) { return 0u; }
uint16_t TaskRun_CrossCount(void) { return 0u; }
uint32_t TaskRun_DistMm(void) { return 0u; }
