/**
 * @file  task_prog.c
 * @brief 任务程序格式的读取与校验（纯函数，主机上可测；见 firmware/test/t_task.c）
 *
 * 这里**只做格式层**：字节 → 结构、CRC、越界检查。执行语义在 task_run.c。
 * 刻意不做指针强转（结构体对齐/填充会让同一个字节流在不同编译器上读出不同结果）。
 */
#include "task_prog.h"

uint16_t TaskProg_Crc16(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t  b;

    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (b = 0u; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void TaskProg_ReadBlock(const uint8_t *prog, uint32_t off, task_block_t *b)
{
    const uint8_t *p = &prog[off];

    b->op    = p[0];
    b->flags = p[1];
    b->p0    = task_rd_u16(&p[2]);
    b->p1    = task_rd_i16(&p[4]);
    b->p2    = task_rd_u16(&p[6]);
    b->p3    = task_rd_u16(&p[8]);
}

void TaskProg_ReadState(const uint8_t *prog, uint8_t idx, task_state_t *st)
{
    const uint8_t *p = &prog[TASK_HDR_LEN + (uint32_t)idx * TASK_STATE_LEN];

    st->name_off  = task_rd_u16(&p[0]);
    st->enter_off = task_rd_u16(&p[2]);
    st->body_off  = task_rd_u16(&p[4]);
    st->n_enter   = p[6];
    st->n_body    = p[7];
}

/* 一个序列（enter 或 body）是否落在程序体内 */
static int seq_ok(uint32_t off, uint8_t n, uint32_t len)
{
    if (n == 0u) {
        return (off == 0u) ? TASK_OK : TASK_E_RANGE;
    }
    if (n > TASK_MAX_BLOCKS) {
        return TASK_E_RANGE;
    }
    if (off < TASK_HDR_LEN || (uint32_t)off + (uint32_t)n * TASK_BLOCK_LEN > len) {
        return TASK_E_RANGE;
    }
    return TASK_OK;
}

/* 序列必须以"能改变执行流"的块收尾：跳到状态 / 结束 / 带超时跳转的等待。
 * 否则状态跑完就停在那里，车会保持最后一个模式一直开下去 —— 这是最危险的一类错。 */
static int tail_ok(const uint8_t *prog, uint32_t off, uint8_t n)
{
    task_block_t b;

    if (n == 0u) {
        return TASK_OK;
    }
    TaskProg_ReadBlock(prog, off + (uint32_t)(n - 1u) * TASK_BLOCK_LEN, &b);
    if (b.op == TASK_OP_GOTO || b.op == TASK_OP_END) {
        return TASK_OK;
    }
    if (b.op == TASK_OP_WAIT_COND && b.p3 != 0xFFFFu) {
        return TASK_OK;
    }
    return TASK_E_TAIL;
}

int TaskProg_Validate(const uint8_t *prog, uint32_t len)
{
    uint32_t i;
    uint8_t  n_states;

    if (prog == 0 || len < TASK_HDR_LEN) {
        return TASK_E_SHORT;
    }
    if (task_rd_u16(&prog[0]) != TASK_PROG_MAGIC) {
        return TASK_E_MAGIC;
    }
    if (prog[2] != (uint8_t)TASK_PROG_VER) {
        return TASK_E_VER;
    }
    n_states = prog[3];
    if (n_states == 0u || n_states > (uint8_t)TASK_MAX_STATES) {
        return TASK_E_STATES;
    }
    if (prog[4] >= n_states) {
        return TASK_E_MAIN;
    }
    if (task_rd_u16(&prog[8]) != (uint16_t)len) {
        return TASK_E_LEN;
    }
    if (TASK_HDR_LEN + (uint32_t)n_states * TASK_STATE_LEN > len) {
        return TASK_E_RANGE;
    }
    if (TaskProg_Crc16(&prog[TASK_HDR_LEN], len - TASK_HDR_LEN) != task_rd_u16(&prog[6])) {
        return TASK_E_CRC;
    }

    for (i = 0u; i < (uint32_t)n_states; i++) {
        task_state_t st;
        int e;

        TaskProg_ReadState(prog, (uint8_t)i, &st);
        if (st.name_off != 0u && (uint32_t)st.name_off >= len) {
            return TASK_E_RANGE;
        }
        e = seq_ok(st.enter_off, st.n_enter, len);
        if (e != TASK_OK) {
            return e;
        }
        e = seq_ok(st.body_off, st.n_body, len);
        if (e != TASK_OK) {
            return e;
        }
        /* 只有 body（状态的"主循环"）必须以跳转收尾；enter 序列是"进门做一次"的
         * 准备工作，末尾是普通块完全正常（跑完自动接 body）。 */
        e = tail_ok(prog, st.body_off, st.n_body);
        if (e != TASK_OK) {
            return e;
        }
    }
    return TASK_OK;
}

const char *TaskProg_ErrStr(int e)
{
    switch (e) {
    case TASK_OK:       return "ok";
    case TASK_E_SHORT:  return "程序太短";
    case TASK_E_MAGIC:  return "magic 不对";
    case TASK_E_VER:    return "版本不支持";
    case TASK_E_STATES: return "状态数越界";
    case TASK_E_MAIN:   return "主页状态号越界";
    case TASK_E_RANGE:  return "偏移/块数越界";
    case TASK_E_CRC:    return "CRC 不对";
    case TASK_E_LEN:    return "长度字段与实际不符";
    case TASK_E_TAIL:   return "序列末尾必须是 跳到状态/结束/带超时跳转的等待";
    default:            return "未知错误";
    }
}
