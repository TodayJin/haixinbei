/**
 * @file  taskgen.c
 * @brief 「任务程序」金样本生成器 —— 固件格式层（firmware/App/Svc/task_prog.c）的字节标准答案
 *
 * 上位机侧 `tools/console/web/src/lib/task.ts` 必须编出**逐字节相同**的镜像。
 * 这个程序把同一份任务（下面 pg_* 里写死的场景，与 `task.test.ts` 的 TS 场景一一对应）
 * 编成 .bin，供两条测试对拍：
 *   - `tools/console/tests/test_task_bytes.py`：现场用 gcc 编本文件跑一遍，和 fixtures 里的金样本比；
 *   - `tools/console/web/src/lib/task.test.ts`：TS 编出同样的字节，和金样本比。
 * 两边都红不了，才说明 C 与 TS 没分家。**改格式必须同时改这里、task.ts、t_task.c**。
 *
 * 编译（主机侧，不需要板子；把真格式层 task_prog.c 一起链进来）：
 *   gcc -std=gnu11 -I firmware/App/Svc tools/console/tests/c/taskgen.c \
 *       firmware/App/Svc/task_prog.c -o taskgen.exe
 * 用法：
 *   taskgen.exe <out.bin>      # 写镜像
 *   taskgen.exe <out.bin> -hex # 写镜像并打印 CRC/长度
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "task_prog.h"

#define NST 3

static uint8_t      g_prog[1024];
static task_block_t g_enter[NST][8];
static uint8_t      g_ne[NST];
static task_block_t g_body[NST][24];
static uint8_t      g_nb[NST];
static const char  *g_name[NST] = { "常态循迹", "路口转90", "摆舵机" };

static void blk(task_block_t *a, uint8_t *n, uint8_t op, uint8_t flags, uint16_t p0,
                int16_t p1, uint16_t p2, uint16_t p3)
{
    task_block_t *b = &a[*n];

    b->op = op; b->flags = flags; b->p0 = p0; b->p1 = p1; b->p2 = p2; b->p3 = p3;
    (*n)++;
}

static void wr_block_bytes(uint32_t off, const task_block_t *b)
{
    uint16_t u;

    g_prog[off + 0u] = b->op;
    g_prog[off + 1u] = b->flags;
    u = b->p0;
    g_prog[off + 2u] = (uint8_t)(u & 0xFFu); g_prog[off + 3u] = (uint8_t)(u >> 8);
    u = (uint16_t)b->p1;
    g_prog[off + 4u] = (uint8_t)(u & 0xFFu); g_prog[off + 5u] = (uint8_t)(u >> 8);
    u = b->p2;
    g_prog[off + 6u] = (uint8_t)(u & 0xFFu); g_prog[off + 7u] = (uint8_t)(u >> 8);
    u = b->p3;
    g_prog[off + 8u] = (uint8_t)(u & 0xFFu); g_prog[off + 9u] = (uint8_t)(u >> 8);
}

/* 与 task.ts 的 compileTask() 同一套排布：
 *   header(12) | 状态表(8×n) | 块区（每状态先 enter 后 body，按状态顺序） | 名字区（按状态顺序，UTF-8，无 NUL）
 */
static uint32_t build(uint8_t main_state)
{
    uint16_t eoff[NST], boff[NST], noff[NST];
    uint32_t off = TASK_HDR_LEN + (uint32_t)NST * TASK_STATE_LEN;
    uint8_t  i, j;
    uint16_t c;

    /* ---- 场景：就是 task.test.ts 里那三个状态 ---- */
    blk(g_enter[0], &g_ne[0], TASK_OP_SET_MODE, 0, TASK_MODE_TRACE_GYRO, 0, 0, 0);
    blk(g_body[0], &g_nb[0], TASK_OP_SET_SPEED, 0, 6480, 0, 0, 0);
    blk(g_body[0], &g_nb[0], TASK_OP_ACTION, 0, 3, 0, 500, 0);
    blk(g_body[0], &g_nb[0], TASK_OP_WAIT_COND, 0, TASK_C_CROSS, 0, 3000, 1);
    blk(g_body[0], &g_nb[0], TASK_OP_GOTO, 0, 0, 0, 0, 0);

    blk(g_enter[1], &g_ne[1], TASK_OP_WAIT_MS, 0, 200, 0, 0, 0);
    blk(g_body[1], &g_nb[1], TASK_OP_TURN_DEG, 1, 2000, -90, 10, 0);
    blk(g_body[1], &g_nb[1], TASK_OP_CROSS_CLEAR, 0, 0, 0, 0, 0);
    blk(g_body[1], &g_nb[1], TASK_OP_WAIT_COND, 0, TASK_C_ANGLE_DONE, 10, 0, 0xFFFF);
    blk(g_body[1], &g_nb[1], TASK_OP_GOTO, 0, 1, 0, 0, 0);

    blk(g_body[2], &g_nb[2], TASK_OP_SET_SERVO, 0, 1, 180, 0, 0);
    blk(g_body[2], &g_nb[2], TASK_OP_SET_SERVO, 0, 2, 0, 0, 0);
    blk(g_body[2], &g_nb[2], TASK_OP_WAIT_COND, 0, TASK_C_ALWAYS, 0, 500, 0xFFFF);
    blk(g_body[2], &g_nb[2], TASK_OP_END, 0, 0, 0, 0, 0);

    memset(eoff, 0, sizeof eoff);
    memset(boff, 0, sizeof boff);
    memset(noff, 0, sizeof noff);

    for (i = 0u; i < NST; i++) {
        if (g_ne[i] > 0u) { eoff[i] = (uint16_t)off; off += (uint32_t)g_ne[i] * TASK_BLOCK_LEN; }
        if (g_nb[i] > 0u) { boff[i] = (uint16_t)off; off += (uint32_t)g_nb[i] * TASK_BLOCK_LEN; }
    }
    for (i = 0u; i < NST; i++) {
        size_t nl = strlen(g_name[i]);

        if (nl > 0u) {
            noff[i] = (uint16_t)off;
            memcpy(&g_prog[off], g_name[i], nl);
            off += (uint32_t)nl;
        }
    }

    g_prog[0] = (uint8_t)(TASK_PROG_MAGIC & 0xFFu);
    g_prog[1] = (uint8_t)(TASK_PROG_MAGIC >> 8);
    g_prog[2] = (uint8_t)TASK_PROG_VER;
    g_prog[3] = (uint8_t)NST;
    g_prog[4] = main_state;
    g_prog[5] = 0u;
    g_prog[8] = (uint8_t)(off & 0xFFu);
    g_prog[9] = (uint8_t)(off >> 8);

    for (i = 0u; i < NST; i++) {
        uint32_t so = TASK_HDR_LEN + (uint32_t)i * TASK_STATE_LEN;

        g_prog[so + 0u] = (uint8_t)(noff[i] & 0xFFu); g_prog[so + 1u] = (uint8_t)(noff[i] >> 8);
        g_prog[so + 2u] = (uint8_t)(eoff[i] & 0xFFu); g_prog[so + 3u] = (uint8_t)(eoff[i] >> 8);
        g_prog[so + 4u] = (uint8_t)(boff[i] & 0xFFu); g_prog[so + 5u] = (uint8_t)(boff[i] >> 8);
        g_prog[so + 6u] = g_ne[i];
        g_prog[so + 7u] = g_nb[i];

        for (j = 0u; j < g_ne[i]; j++) wr_block_bytes(eoff[i] + (uint32_t)j * TASK_BLOCK_LEN, &g_enter[i][j]);
        for (j = 0u; j < g_nb[i]; j++) wr_block_bytes(boff[i] + (uint32_t)j * TASK_BLOCK_LEN, &g_body[i][j]);
    }

    c = TaskProg_Crc16(&g_prog[TASK_HDR_LEN], off - TASK_HDR_LEN);
    g_prog[6] = (uint8_t)(c & 0xFFu);
    g_prog[7] = (uint8_t)(c >> 8);
    return off;
}

int main(int argc, char **argv)
{
    uint32_t len;
    FILE    *f;
    int      e;

    if (argc < 2) {
        fprintf(stderr, "usage: taskgen <out.bin> [-hex]\n");
        return 2;
    }
    memset(g_prog, 0, sizeof g_prog);
    memset(g_enter, 0, sizeof g_enter);
    memset(g_body, 0, sizeof g_body);
    memset(g_ne, 0, sizeof g_ne);
    memset(g_nb, 0, sizeof g_nb);

    len = build(0u);
    e   = TaskProg_Validate(g_prog, len);
    if (e != TASK_OK) {
        fprintf(stderr, "self-check failed: %s\n", TaskProg_ErrStr(e));
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    fwrite(g_prog, 1u, len, f);
    fclose(f);

    if (argc > 2 && strcmp(argv[2], "-hex") == 0) {
        uint32_t i;

        printf("len=%u crc16=0x%04X\n", (unsigned)len,
               (unsigned)task_rd_u16(&g_prog[6]));
        for (i = 0u; i < len; i++) {
            printf("%02X", g_prog[i]);
            if ((i % 20u) == 19u) printf("\n");
        }
        printf("\n");
    }
    return 0;
}
