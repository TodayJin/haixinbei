/**
 * @file  t_task.c   主机侧单元测试：任务程序格式 + 解释器（App/Svc/task_prog.c, task_run.c）
 *
 * 这是「任务编程」模块的地基测试，盯三件事：
 *
 *   1) **格式层**（task_prog.c）
 *      金样本字节（magic 'T''K'、小端、10 字节块布局）钉住固件与上位机的字节口径；
 *      反向用例逐个打坏 magic/ver/CRC/长度/状态数/主页/偏移/末尾块，必须报对应的错。
 *      尤其 E_TAIL：序列末尾没有「跳到状态/结束/带超时跳转的等待」= 车会保持最后
 *      一个模式一直开下去，这是最危险的一类错，格式层必须拦住。
 *
 *   2) **解释器**（task_run.c）
 *      SET_SPEED / SET_MODE / ACTION / END 的调用顺序与停车；WAIT_COND 满足与
 *      **超时改道**；里程、丢线、角度到位三类条件的判定；GOTO 自环单拍不卡死。
 *
 *   3) **安全**：跑着不许换程序、没载入不许启动、END 必须 Motion_SetMode(IDLE)+
 *      SpeedControl_Stop()。
 *
 * 依赖：真 task_prog.c + 真 task_run.c，控制模块用 host_appstub.c 顶替
 *      （桩的默认值与旧常量逐字节一致，所以这是纯记录式替身）。
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "task_prog.h"
#include "task_run.h"
#include "motion.h"
#include "t_common.h"

/* ---- host_appstub.c 的观测点 ---- */
extern int      stub_motion_mode;
extern int      stub_motion_speed;
extern int      stub_clear_faults;
extern int      stub_sc_enabled;
extern int      stub_sc_l;
extern int      stub_sc_r;
extern int      stub_sc_stops;
extern float    stub_gyro_target;
extern int      stub_servo_apply_n;
extern int      stub_servo_ctl[3];
extern float    stub_trace_pos;
extern uint8_t  stub_cross;
extern uint8_t  stub_gray[8];
extern float    stub_gyro_err;
extern uint8_t  stub_motion_fault;
extern int      stub_cross_clears;
extern int      stub_speed_mmps[2];

/* ------------------------------------------------------------------ */
/* 程序构造器：把块数组序列化成上位机将来要生成的那种字节流            */
/* ------------------------------------------------------------------ */
#define NST 4

static uint8_t      g_prog[1024];
static task_block_t g_enter[NST][8];
static uint8_t      g_ne[NST];
static task_block_t g_body[NST][24];
static uint8_t      g_nb[NST];
static uint8_t      g_ns;
static uint8_t      g_main;
static char         g_name[NST][12];

static void pg_reset(uint8_t ns, uint8_t main)
{
    memset(g_prog, 0, sizeof g_prog);
    memset(g_enter, 0, sizeof g_enter);
    memset(g_ne, 0, sizeof g_ne);
    memset(g_body, 0, sizeof g_body);
    memset(g_nb, 0, sizeof g_nb);
    memset(g_name, 0, sizeof g_name);
    g_ns   = ns;
    g_main = main;
}

static void wr_blk(task_block_t *a, uint8_t *n, uint8_t op, uint16_t p0, int16_t p1,
                   uint16_t p2, uint16_t p3, uint8_t flags)
{
    task_block_t *b = &a[*n];

    b->op    = op;
    b->flags = flags;
    b->p0    = p0;
    b->p1    = p1;
    b->p2    = p2;
    b->p3    = p3;
    (*n)++;
}

static void add_enter(uint8_t st, uint8_t op, uint16_t p0, int16_t p1, uint16_t p2,
                      uint16_t p3, uint8_t flags)
{
    wr_blk(g_enter[st], &g_ne[st], op, p0, p1, p2, p3, flags);
}

static void add_body(uint8_t st, uint8_t op, uint16_t p0, int16_t p1, uint16_t p2,
                     uint16_t p3, uint8_t flags)
{
    wr_blk(g_body[st], &g_nb[st], op, p0, p1, p2, p3, flags);
}

static void wr_blk_bytes(uint32_t off, const task_block_t *b)
{
    uint16_t u;

    g_prog[off + 0u] = b->op;
    g_prog[off + 1u] = b->flags;
    u = b->p0;
    g_prog[off + 2u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 3u] = (uint8_t)(u >> 8);
    u = (uint16_t)b->p1;
    g_prog[off + 4u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 5u] = (uint8_t)(u >> 8);
    u = b->p2;
    g_prog[off + 6u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 7u] = (uint8_t)(u >> 8);
    u = b->p3;
    g_prog[off + 8u] = (uint8_t)(u & 0xFFu);
    g_prog[off + 9u] = (uint8_t)(u >> 8);
}

/* 重新算 CRC（改了块/状态表之后必须调，否则先被 CRC 挡住、测不到真正想测的错） */
static void pg_fix_crc(void)
{
    uint32_t len = (uint32_t)task_rd_u16(&g_prog[8]);
    uint16_t c   = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], len - TASK_HDR_LEN);

    g_prog[6] = (uint8_t)(c & 0xFFu);
    g_prog[7] = (uint8_t)(c >> 8);
}

static uint32_t pg_build(void)
{
    uint16_t eoff[NST], boff[NST], noff[NST];
    uint32_t off = TASK_HDR_LEN + (uint32_t)g_ns * TASK_STATE_LEN;
    uint8_t  i, j;
    uint16_t c;

    memset(eoff, 0, sizeof eoff);
    memset(boff, 0, sizeof boff);
    memset(noff, 0, sizeof noff);

    /* 1) 先排布：块区（状态顺序：先 enter 后 body），再名字区 */
    for (i = 0u; i < g_ns; i++) {
        if (g_ne[i] > 0u) {
            eoff[i] = (uint16_t)off;
            off += (uint32_t)g_ne[i] * TASK_BLOCK_LEN;
        }
        if (g_nb[i] > 0u) {
            boff[i] = (uint16_t)off;
            off += (uint32_t)g_nb[i] * TASK_BLOCK_LEN;
        }
    }
    for (i = 0u; i < g_ns; i++) {
        if (g_name[i][0] != '\0') {
            noff[i] = (uint16_t)off;
            for (j = 0u; g_name[i][j] != '\0'; j++) {
                g_prog[off++] = (uint8_t)g_name[i][j];
            }
        }
    }

    /* 2) 头部 */
    g_prog[0] = (uint8_t)(TASK_PROG_MAGIC & 0xFFu);
    g_prog[1] = (uint8_t)(TASK_PROG_MAGIC >> 8);
    g_prog[2] = (uint8_t)TASK_PROG_VER;
    g_prog[3] = g_ns;
    g_prog[4] = g_main;
    g_prog[5] = 0u;
    g_prog[8] = (uint8_t)(off & 0xFFu);
    g_prog[9] = (uint8_t)(off >> 8);

    /* 3) 状态表 + 块区 */
    for (i = 0u; i < g_ns; i++) {
        uint32_t so = TASK_HDR_LEN + (uint32_t)i * TASK_STATE_LEN;

        g_prog[so + 0u] = (uint8_t)(noff[i] & 0xFFu);
        g_prog[so + 1u] = (uint8_t)(noff[i] >> 8);
        g_prog[so + 2u] = (uint8_t)(eoff[i] & 0xFFu);
        g_prog[so + 3u] = (uint8_t)(eoff[i] >> 8);
        g_prog[so + 4u] = (uint8_t)(boff[i] & 0xFFu);
        g_prog[so + 5u] = (uint8_t)(boff[i] >> 8);
        g_prog[so + 6u] = g_ne[i];
        g_prog[so + 7u] = g_nb[i];

        for (j = 0u; j < g_ne[i]; j++) {
            wr_blk_bytes((uint32_t)eoff[i] + (uint32_t)j * TASK_BLOCK_LEN, &g_enter[i][j]);
        }
        for (j = 0u; j < g_nb[i]; j++) {
            wr_blk_bytes((uint32_t)boff[i] + (uint32_t)j * TASK_BLOCK_LEN, &g_body[i][j]);
        }
    }

    /* 4) CRC 只覆盖头部之后（长度字段/主页号改了不该动 CRC） */
    c = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], off - TASK_HDR_LEN);
    g_prog[6] = (uint8_t)(c & 0xFFu);
    g_prog[7] = (uint8_t)(c >> 8);
    return off;
}

/* 造一个"三状态、能跑完"的规范程序（多个用例共用） */
static uint32_t pg_build_basic(void)
{
    pg_reset(3u, 0u);
    strcpy(g_name[0], "trace");
    strcpy(g_name[1], "hold");
    strcpy(g_name[2], "bail");

    add_body(0u, TASK_OP_SET_SPEED, 6480u, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_TRACE_GYRO, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_WAIT_COND, (uint16_t)TASK_C_CROSS, 0, 1000u, 2u, 0u);
    add_body(0u, TASK_OP_GOTO, 1u, 0, 0u, 0xFFFFu, 0u);

    add_enter(1u, TASK_OP_SET_SPEED, 3000u, 0, 0u, 0xFFFFu, 0u);   /* 进状态先减速（用来验 enter 序列真的跑了） */
    add_body(1u, TASK_OP_ACTION, 3u, 0, 40u, 0xFFFFu, 0u);
    add_body(1u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);

    add_body(2u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_IDLE, 0, 0u, 0xFFFFu, 0u);
    add_body(2u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    return pg_build();
}

int main(void)
{
    uint32_t len;

    /* ===============================================================
     * 1) 格式层：CRC 金样本 + 字节口径
     * =============================================================== */
    CHECK_EQ_I(TaskProg_Crc16((const uint8_t *)"123456789", 9u), 0x29B1);

    len = pg_build_basic();
    /* 头 12B + 3 状态 × 8B + 块：0号4块 + 1号(1+2)块 + 2号2块 = 9 块，名字 5+4+4=13B */
    CHECK_EQ_I((long long)len, 12 + 24 + 90 + 13);
    CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);
    /* 金样本字节：magic 是小端 'T'(0x54) 'K'(0x4B)，块区第 1 块 = SET_SPEED 6480 */
    CHECK_EQ_I(g_prog[0], 0x54);
    CHECK_EQ_I(g_prog[1], 0x4B);
    CHECK_EQ_I(g_prog[2], TASK_PROG_VER);
    CHECK_EQ_I(g_prog[3], 3);
    CHECK_EQ_I(g_prog[4], 0);
    CHECK_EQ_I(task_rd_u16(&g_prog[8]), len);
    {
        uint32_t b0 = TASK_HDR_LEN + 3u * TASK_STATE_LEN;   /* 0 号状态 body 起点 */
        CHECK_EQ_I(g_prog[b0 + 0u], TASK_OP_SET_SPEED);
        CHECK_EQ_I(g_prog[b0 + 1u], 0);
        CHECK_EQ_I(g_prog[b0 + 2u], 0x50);                 /* 6480 = 0x1950，小端 */
        CHECK_EQ_I(g_prog[b0 + 3u], 0x19);
        CHECK_EQ_I(g_prog[b0 + 4u], 0);
        /* 说明：SET_SPEED 不读 p3，测试构造器给未用字段填的是 0xFFFF（填充值不规定，
         * 固件只对 WAIT_COND 读 p3）。这里把"确实没被读到"钉住即可。 */
        CHECK_EQ_I(g_prog[b0 + 9u], 0xFF);
    }
    /* 读回：enter/body 偏移与块数、名字偏移 */
    {
        task_state_t st;
        task_block_t b;

        TaskProg_ReadState(g_prog, 1u, &st);
        CHECK_EQ_I(st.n_enter, 1);
        CHECK_EQ_I(st.n_body, 2);
        CHECK_EQ_I(st.name_off, 12 + 24 + 90 + 5);   /* 0 号状态名字 "trace" 占 5 字节 */
        TaskProg_ReadBlock(g_prog, st.body_off, &b);
        CHECK_EQ_I(b.op, TASK_OP_ACTION);
        CHECK_EQ_I(b.p0, 3);
        CHECK_EQ_I(b.p2, 40);
        CHECK_EQ_I(b.p3, 0xFFFF);
    }
    /* 跨语言的字节口径不在这里手抄十六进制：唯一权威样本走生成链
     *   tools/console/tests/c/taskgen.c（链真 task_prog.c）
     *   → tools/console/tests/fixtures/task.golden.json
     *   → tools/console/web/src/lib/task.golden.ts → task.golden.test.ts
     * 新鲜度由 tools/console/tests/test_task_bytes.py 每次重编比对。 */

    /* ===============================================================
     * 2) 格式层反向用例：一类错一个用例
     * =============================================================== */
    CHECK_EQ_I(TaskProg_Validate(0, 100u), TASK_E_SHORT);
    CHECK_EQ_I(TaskProg_Validate(g_prog, 6u), TASK_E_SHORT);
    {
        uint8_t save = g_prog[0];
        g_prog[0] = (uint8_t)(save ^ 0xFFu);
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_MAGIC);
        g_prog[0] = save;
    }
    {
        uint8_t save = g_prog[2];
        g_prog[2] = 9u;
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_VER);
        g_prog[2] = save;
    }
    {
        uint8_t save = g_prog[3];
        g_prog[3] = 0u;
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_STATES);
        g_prog[3] = (uint8_t)(TASK_MAX_STATES + 1u);
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_STATES);
        g_prog[3] = save;
    }
    {
        uint8_t save = g_prog[4];
        g_prog[4] = 3u;                                    /* == n_states */
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_MAIN);
        g_prog[4] = save;
    }
    {
        uint16_t save = task_rd_u16(&g_prog[8]);
        g_prog[8] = (uint8_t)((save - 1u) & 0xFFu);
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_LEN);
        g_prog[8] = (uint8_t)(save & 0xFFu);
        g_prog[9] = (uint8_t)(save >> 8);
    }
    {
        uint8_t save = g_prog[20];                         /* payload 里翻一位 → CRC 错 */
        g_prog[20] = (uint8_t)(save ^ 0x01u);
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_CRC);
        g_prog[20] = save;
    }
    {   /* 偏移/块数越界：body 块数改成 200（>TASK_MAX_BLOCKS），修好 CRC 再测 */
        uint8_t save = g_prog[TASK_HDR_LEN + 7u];
        g_prog[TASK_HDR_LEN + 7u] = 200u;
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_RANGE);
        g_prog[TASK_HDR_LEN + 7u] = save;
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);   /* 复原后必须又干净 */
    }
    {   /* 名字偏移越界 */
        uint8_t s0 = g_prog[12], s1 = g_prog[13];
        g_prog[12] = 0xFFu;
        g_prog[13] = 0xFFu;
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_RANGE);
        g_prog[12] = s0;
        g_prog[13] = s1;
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);
    }
    {   /* 末尾块：状态 2 的 body 最后一块改成 WAIT_MS（无跳转） → 必须报 E_TAIL */
        task_state_t st;
        task_block_t b;

        TaskProg_ReadState(g_prog, 2u, &st);
        b.op = TASK_OP_WAIT_MS; b.p0 = 10u; b.p1 = 0; b.p2 = 0u; b.p3 = 0xFFFFu; b.flags = 0u;
        wr_blk_bytes((uint32_t)st.body_off + (uint32_t)(st.n_body - 1u) * TASK_BLOCK_LEN, &b);
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_E_TAIL);
        /* 带超时跳转的 WAIT_COND 是合法收尾 */
        b.op = TASK_OP_WAIT_COND; b.p0 = TASK_C_CROSS; b.p1 = 0; b.p2 = 500u; b.p3 = 0u;
        wr_blk_bytes((uint32_t)st.body_off + (uint32_t)(st.n_body - 1u) * TASK_BLOCK_LEN, &b);
        pg_fix_crc();
        CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);
    }
    CHECK(strcmp(TaskProg_ErrStr(TASK_E_TAIL), "ok") != 0);   /* 错误串不能是空的 */

    /* ===============================================================
     * 3) 解释器：载入 / 启动 / SET_SPEED→SET_MODE→ACTION→END
     * =============================================================== */
    CHECK_EQ_I(TaskRun_Start(1u), -1);                    /* 没载入过不许启动 */
    CHECK_EQ_I(TaskRun_Loaded(), 0);

    /* 注意：上一节最后把状态 2 改成了"WAIT_COND 超时跳 0"，所以这里重建一份规范的 */
    len = pg_build_basic();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    CHECK_EQ_I(TaskRun_Loaded(), 1);
    CHECK_EQ_I(TaskRun_ProgLen(), (long long)len);

    /* 坏程序不许顶掉已载入的好程序 */
    {
        uint8_t save = g_prog[2];
        g_prog[2] = 9u;
        CHECK_EQ_I(TaskRun_Load(g_prog, len), -TASK_E_VER);
        g_prog[2] = save;
        CHECK_EQ_I(TaskRun_Loaded(), 1);
        CHECK_EQ_I(TaskRun_ProgLen(), (long long)len);
    }

    stub_motion_speed   = -1;
    stub_motion_mode    = -1;
    stub_servo_apply_n  = 0;
    stub_trace_pos      = 4.25f;
    stub_gyro_err       = 1.25f;
    stub_motion_fault   = 0u;
    stub_cross          = 1u;
    stub_cross_clears   = 0;
    {
        int cf0 = stub_clear_faults;

        CHECK_EQ_I(TaskRun_Start(7u), 0);
        CHECK_EQ_I(TaskRun_IsRunning(), 1);
        CHECK_EQ_I(TaskRun_Id(), 7);
        CHECK_EQ_I(TaskRun_State(), 0);                   /* 从主页状态开始 */
        CHECK_EQ_I(stub_clear_faults - cf0, 1);            /* 启动要清故障 */
        CHECK(stub_cross_clears >= 1);                     /* 启动要清横线计数 */
    }
    /* 跑着不换程序 */
    CHECK_EQ_I(TaskRun_Load(g_prog, len), -1);
    CHECK_EQ_I(TaskRun_Loaded(), 1);

    TaskRun_Tick(1000u);                                   /* 第一拍：一路推进到 GOTO */
    CHECK_EQ_I(stub_motion_speed, 3000);                   /* hold 状态的 enter 序列跑过了 */
    CHECK_EQ_I(stub_motion_mode, MOTION_TRACE_GYRO);
    CHECK_EQ_I(TaskRun_State(), 1);                        /* 跳到 hold 状态 */
    CHECK_EQ_I(stub_servo_apply_n, 3);                     /* ACTION 3 已下发 */
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Tick(1020u);                                   /* ACTION 的 40ms 还没到 */
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Tick(1041u);                                   /* 41ms 之后 */
    CHECK_EQ_I(TaskRun_IsRunning(), 0);                    /* END 停车 */
    CHECK_EQ_I(TaskRun_Done(), 1);
    CHECK_EQ_I(TaskRun_Flags() & TASK_RUN_F_DONE, TASK_RUN_F_DONE);
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);             /* END 必须回 IDLE */
    CHECK(TaskRun_ElapsedMs() > 0u);
    TaskRun_Stop();                                        /* 幂等（已经在跑完状态） */

    /* ===============================================================
     * 4) WAIT_COND：满足 → 清横线；不满足 → 超时改道
     * =============================================================== */
    len = pg_build_basic();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_cross = 1u;                                       /* 立刻检测到横线 */
    stub_cross_clears = 0;
    CHECK_EQ_I(TaskRun_Start(1u), 0);
    TaskRun_Tick(2000u);
    CHECK_EQ_I(TaskRun_State(), 1);                        /* 条件满足 → GOTO 1 */
    CHECK(stub_cross_clears >= 2);                         /* 启动清一次 + 条件满足清一次 */
    TaskRun_Tick(2100u);                                   /* hold: ACTION 40ms + END */
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* 改道：横线一直不来，1s 超时 → 跳 bail 状态 */
    len = pg_build_basic();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_cross = 0u;
    CHECK_EQ_I(TaskRun_Start(2u), 0);
    TaskRun_Tick(3000u);
    CHECK_EQ_I(TaskRun_State(), 0);                        /* 还在等 */
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Tick(3050u);
    CHECK_EQ_I(TaskRun_State(), 0);                        /* 50ms < 1000ms */
    TaskRun_Tick(4001u);                                   /* 1001ms → 超时改道到状态 2 */
    CHECK_EQ_I(TaskRun_State(), 2);
    CHECK_EQ_I(TaskRun_Flags() & TASK_RUN_F_TIMEOUT, TASK_RUN_F_TIMEOUT);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);                    /* 状态 2 是 SET_MODE IDLE + END */
    CHECK_EQ_I(TaskRun_Done(), 1);

    /* ===============================================================
     * 5) 里程条件：按实际 dt 积分轮速（mm/s）
     * =============================================================== */
    pg_reset(1u, 0u);
    strcpy(g_name[0], "run");
    add_body(0u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_WHEELS, 0, 0u, 0xFFFFu, 0x01u);
    add_body(0u, TASK_OP_WAIT_COND, (uint16_t)TASK_C_DIST_MM, 100, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_GOTO, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_speed_mmps[0] = 1000;
    stub_speed_mmps[1] = 1000;
    stub_sc_enabled = 0;
    stub_sc_l = -1;
    stub_sc_r = -1;
    CHECK_EQ_I(TaskRun_Start(3u), 0);
    TaskRun_Tick(5000u);                                   /* 第一拍 dt=0：块 0 完成、块 1 还差 100mm */
    CHECK_EQ_I(stub_sc_enabled, 1);                        /* 两轮直给走速度环 */
    CHECK_EQ_I(stub_sc_l, 0);
    CHECK_EQ_I(stub_sc_r, 0);
    CHECK_EQ_I(TaskRun_DistMm(), 0);
    TaskRun_Tick(5100u);                                   /* dt=100ms、1000mm/s → 100mm */
    CHECK(TaskRun_DistMm() >= 100u);
    CHECK_EQ_I(TaskRun_State(), 0u);                       /* 条件满足 → 走到 GOTO 0（自环） */
    CHECK_EQ_I(TaskRun_Block(), 0u);
    TaskRun_Tick(5200u);                                   /* 自环再跑一圈也不卡 */
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Stop();
    stub_speed_mmps[0] = 0;
    stub_speed_mmps[1] = 0;

    /* ===============================================================
     * 6) 丢线 / 找回线：8 路灰度阈值
     * =============================================================== */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_WAIT_COND, (uint16_t)TASK_C_LINE_LOST, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    {
        uint8_t i;

        for (i = 0u; i < 8u; i++) {
            stub_gray[i] = 10u;                            /* 全黑 → 丢线 */
        }
    }
    CHECK_EQ_I(TaskRun_Start(4u), 0);
    TaskRun_Tick(6000u);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);                    /* 丢线条件立刻满足 → END */

    CHECK_EQ_I(TaskRun_Start(4u), 0);
    stub_gray[3] = 120u;                                   /* 有一路压线 → 不算丢 */
    TaskRun_Tick(6100u);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    stub_gray[3] = 10u;
    TaskRun_Tick(6200u);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* ===============================================================
     * 7) 转到角度：相对角 = 当前环内航向 + 相对量；到位/超时两条腿
     * =============================================================== */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_TURN_DEG, 0u, 90, 0u, 0xFFFFu, 0u);   /* 无超时、容差 1.0° */
    add_body(0u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_gyro_target = 0.0f;
    stub_gyro_err    = 30.0f;                              /* 当前航向 = 0-30 = -30° */
    CHECK_EQ_I(TaskRun_Start(5u), 0);
    TaskRun_Tick(7000u);
    CHECK(fabsf(stub_gyro_target - 60.0f) < 0.01f);        /* -30 + 90 = 60 */
    CHECK_EQ_I(stub_motion_mode, MOTION_GYRO);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);                    /* 误差 30° 还没到位 */
    stub_gyro_err = 0.5f;                                  /* 进容差 */
    TaskRun_Tick(7100u);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);
    CHECK_EQ_I(TaskRun_Flags() & TASK_RUN_F_TIMEOUT, 0);

    /* 超时那条腿：角度一直不到位 → 100ms 后带 TIMEOUT 标志继续 */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_TURN_DEG, 100u, -45, 20u, 0xFFFFu, 0u);  /* 超时 100ms、容差 2.0° */
    add_body(0u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_gyro_target = 10.0f;
    stub_gyro_err    = 30.0f;                              /* 航向 -20 → 目标 -65 */
    CHECK_EQ_I(TaskRun_Start(6u), 0);
    TaskRun_Tick(8000u);
    CHECK(fabsf(stub_gyro_target - (-65.0f)) < 0.01f);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Tick(8050u);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    TaskRun_Tick(8101u);                                   /* 超时 → 放行到 END */
    CHECK_EQ_I(TaskRun_IsRunning(), 0);
    CHECK_EQ_I(TaskRun_Flags() & TASK_RUN_F_TIMEOUT, TASK_RUN_F_TIMEOUT);

    /* ===============================================================
     * 8) 故障条件 + 摆舵机 + 清横线 + GOTO 自环不卡死
     * =============================================================== */
    pg_reset(2u, 0u);
    add_body(0u, TASK_OP_WAIT_COND, (uint16_t)TASK_C_FAULT, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_SET_SERVO, 2u, 999, 0u, 0xFFFFu, 0u);   /* 角度越界 → 要钳到 180 */
    add_body(0u, TASK_OP_CROSS_CLEAR, 0u, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_GOTO, 1u, 0, 0u, 0xFFFFu, 0u);
    add_body(1u, TASK_OP_GOTO, 1u, 0, 0u, 0xFFFFu, 0u);          /* 状态 1 自环 */
    len = pg_build();
    CHECK_EQ_I(TaskProg_Validate(g_prog, len), TASK_OK);
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_motion_fault = 0u;
    stub_servo_ctl[2] = -1;
    stub_cross_clears = 0;
    CHECK_EQ_I(TaskRun_Start(8u), 0);
    TaskRun_Tick(9000u);
    CHECK_EQ_I(TaskRun_IsRunning(), 1);                    /* 无故障 → 卡在等待 */
    CHECK_EQ_I(TaskRun_State(), 0);
    stub_motion_fault = 1u;
    TaskRun_Tick(9100u);                                   /* 条件满足 → 摆舵机 → 清横线 → 进状态 1 */
    CHECK_EQ_I(TaskRun_State(), 1);
    CHECK_EQ_I(stub_servo_ctl[2], 180);                    /* 越界角度被钳住 */
    CHECK(stub_cross_clears >= 1);
    {   /* 自环：单拍只推进 TICK_MAX_BLOCKS 个块就返回，不挂死 */
        int k;

        for (k = 0; k < 5; k++) {
            TaskRun_Tick(9200u + (uint32_t)k);
        }
        CHECK_EQ_I(TaskRun_IsRunning(), 1);
        CHECK_EQ_I(TaskRun_State(), 1);
    }
    TaskRun_Stop();
    CHECK_EQ_I(TaskRun_IsRunning(), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);
    TaskRun_Tick(9300u);                                   /* 停了以后再拍也不动 */
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* ===============================================================
     * 9) 里程钳位：主循环被拖长 5s，单拍积分不能按 5s 算
     * =============================================================== */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_WHEELS, 0, 0u, 0xFFFFu, 0x01u);
    add_body(0u, TASK_OP_WAIT_COND, (uint16_t)TASK_C_DIST_MM, 30000, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_GOTO, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_speed_mmps[0] = 1000;
    stub_speed_mmps[1] = 1000;
    CHECK_EQ_I(TaskRun_Start(9u), 0);
    TaskRun_Tick(10000u);                                  /* 起始 */
    TaskRun_Tick(15000u);                                  /* +5000ms → 被钳到 200ms = 200mm */
    CHECK_EQ_I(TaskRun_DistMm(), 200);
    TaskRun_Stop();
    stub_speed_mmps[0] = 0;
    stub_speed_mmps[1] = 0;

    /* ===============================================================
     * 10) SET_SPEED 真的落到 Motion_SetSpeed
     * =============================================================== */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_SET_SPEED, 6480u, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_motion_speed = -1;
    CHECK_EQ_I(TaskRun_Start(10u), 0);
    TaskRun_Tick(20000u);
    CHECK_EQ_I(stub_motion_speed, 6480);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* ===============================================================
     * 11) 启动时按 HP_TASK_SPEED 设一次基准速度（默认 6480；host_paramstub.c 回落兜底）
     *     程序里**没有** SET_SPEED，所以唯一动过 stub_motion_speed 的就是启动那一下
     * =============================================================== */
    pg_reset(1u, 0u);
    add_body(0u, TASK_OP_END, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_motion_speed = -1;
    CHECK_EQ_I(TaskRun_Start(11u), 0);
    CHECK_EQ_I(stub_motion_speed, 6480);       /* 参数表默认值，旧板内置流程的"直行速度" */
    TaskRun_Tick(1000u);
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    /* ===============================================================
     * 12) 「SET_MODE 3 + GOTO 自己」不许把速度环反复关掉（用户 m28949：
     *     跑循迹"根本就不动"）—— enter 块每次进状态都会重放，而 SetMode 对
     *     同模式是空操作，所以 SET_MODE 块无脑先 Stop 的话，速度环永远开着
     *     不了、PWM 每拍被清 0。
     * =============================================================== */
    pg_reset(1u, 0u);
    add_enter(0u, TASK_OP_SET_MODE, (uint16_t)TASK_MODE_TRACE_GYRO, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_SET_SPEED, 7200u, 0, 0u, 0xFFFFu, 0u);
    add_body(0u, TASK_OP_GOTO, 0u, 0, 0u, 0xFFFFu, 0u);
    len = pg_build();
    CHECK_EQ_I(TaskRun_Load(g_prog, len), 0);
    stub_motion_mode  = (int)MOTION_IDLE;
    stub_motion_speed = -1;
    stub_sc_stops     = 0;
    stub_sc_enabled   = 0;
    CHECK_EQ_I(TaskRun_Start(12u), 0);
    TaskRun_Tick(30000u);                                  /* 第一拍：切进循迹模式 */
    CHECK_EQ_I(stub_motion_mode, (int)TASK_MODE_TRACE_GYRO);
    CHECK_EQ_I(stub_motion_speed, 7200);                   /* SET_SPEED 的 7200 真的落了 */
    CHECK_EQ_I(TaskRun_IsRunning(), 1);
    {
        int stops0 = stub_sc_stops;                        /* 切换那一次是允许的 */

        for (uint32_t k = 0u; k < 2000u; k++) {
            TaskRun_Tick(30001u + k);                      /* 一直绕在 GOTO 自己里 */
        }
        CHECK_EQ_I(TaskRun_IsRunning(), 1);
        CHECK_EQ_I(TaskRun_State(), 0);
        CHECK_EQ_I(stub_sc_stops, stops0);                 /* 2000 拍里一次都不许再 Stop */
        CHECK_EQ_I(stub_motion_mode, (int)TASK_MODE_TRACE_GYRO);
        CHECK_EQ_I(stub_motion_speed, 7200);
    }
    TaskRun_Stop();
    CHECK_EQ_I(TaskRun_IsRunning(), 0);

    return T_SUMMARY();
}
