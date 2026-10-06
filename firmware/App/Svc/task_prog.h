/**
 * @file  task_prog.h
 * @brief 上位机可编程任务（状态机）的**程序格式** —— 与 tools/console/web/src/lib/task.ts 逐字节对齐
 *
 * 设计文档 `docs/任务编程.md`。车只是执行机构：上位机把任务拼成这段二进制、烧进 Flash，
 * 车离线自主执行（比赛不能遥控）。
 *
 * 改这个格式必须同时改三处，否则就是静默的字节错位：
 *   1) 本文件 + task_prog.c
 *   2) tools/console/web/src/lib/task.ts
 *   3) firmware/test/t_task.c 里的**金样本**（那串写死的字节）
 *
 * 字节序一律小端；所有偏移是相对程序起点（u16 ⇒ 单个任务 ≤ 64KB）。
 */
#ifndef __TASK_PROG_H__
#define __TASK_PROG_H__

#include <stdint.h>

#define TASK_PROG_MAGIC   0x4B54u   /* 小端字节序 'T'(0x54) 'K'(0x4B) */
#define TASK_PROG_VER     1u
#define TASK_MAX_STATES   16u
#define TASK_MAX_TASKS    16u
#define TASK_MAX_NAME     32u       /* 名字区单个名字最长（上位机截断） */
#define TASK_MAX_BLOCKS   64u       /* 单个序列最多块数（防越界/防炸） */

#define TASK_HDR_LEN      12u
#define TASK_STATE_LEN    8u
#define TASK_BLOCK_LEN    10u

/* ---- 块 op ---- */
enum {
    TASK_OP_SET_SPEED   = 0x01,  /* p0 = 基准速度（PWM 刻度 0..7200） */
    TASK_OP_SET_MODE    = 0x02,  /* p0 = 控制模式；flags.bit0 = 两轮直给（p1=左 p2=右） */
    TASK_OP_TURN_DEG    = 0x03,  /* p1 = 相对角（度）；p2 = 容差（0.1°，0→1.0°）；p0 = 超时 ms */
    TASK_OP_ACTION      = 0x04,  /* p0 = 动作号 1..14；p2 = 动作后等待 ms */
    TASK_OP_WAIT_MS     = 0x05,  /* p0 = 毫秒 */
    TASK_OP_WAIT_COND   = 0x06,  /* p0 = 条件号；p1 = 阈值；p2 = 超时 ms（0=无限）；p3 = 超时后跳的状态（0xFFFF=继续）；
                                  * flags.bit7 = **满足后**跳到低 7 位的状态号（0 = 满足后继续往下；见 TASK_WAIT_COND_JUMP） */
    TASK_OP_GOTO        = 0x07,  /* p0 = 目标状态号 */
    TASK_OP_END         = 0x08,  /* 停车、任务结束 */
    TASK_OP_SET_SERVO   = 0x09,  /* p0 = 通道 1/2；p1 = 角度 0..180 */
    TASK_OP_CROSS_CLEAR = 0x0A,  /* 清横线/路口计数 */
    TASK_OP_CAL_SET     = 0x0B,  /* ★p0 = 用第几套灰度标定：0 = 普通 / 1 = 亚克力（用户 m 2026-10-07）。
                                  *   对应 trace_cal_select_set(p0)。两套标定一直存在（cal / cal_b），
                                  *   但在这块加出来之前，**跑任务时没有任何办法切换它**。 */
};

/* ---- TASK_OP_SET_MODE 的 p0 ---- */
enum {
    TASK_MODE_IDLE       = 0,   /* 不接管电机（PWM 由别处 Load） */
    TASK_MODE_GYRO       = 1,   /* 角度环单独跑 */
    TASK_MODE_TRACE      = 2,   /* 循迹环单独跑 */
    TASK_MODE_TRACE_GYRO = 3,   /* 三级串级（正常循迹） */
    TASK_MODE_WHEELS     = 4,   /* 两轮速度直给：p1=左 p2=右（PWM 刻度） */
};

/* ---- TASK_OP_WAIT_COND 的 flags（2026-10-07 用户 m 要求）----
 * 4 个参数位 p0..p3 已被"条件/阈值/超时/超时后跳"占满，第 5 个参数只能用那个一直空着的
 * flags 字节。语义：**满足后**去哪 —— 与"超时后跳 p3"是两条互不影响的出路。
 *   老任务（含 renwu-lap.json 与所有既有镜像）的 flags 全是 0 ⇒ bit7 = 0 ⇒ **行为完全不变**。
 * 注意别和 TASK_OP_SET_MODE 的 flags.bit0（两轮直给）搞混：那是另一个 op 的位。 */
#define TASK_WAIT_COND_JUMP     0x80u  /* bit7 = 1：条件满足后跳到 TASK_WAIT_COND_TGT_MSK 那个状态 */
#define TASK_WAIT_COND_TGT_MSK  0x7Fu  /* 低 7 位 = 目标状态号（状态最多 16 个，够用） */

/* ---- TASK_OP_WAIT_COND 的条件号 ---- */
enum {
    TASK_C_LINE_CENTER = 1,  /* |循迹重心-4.5| ≤ p1/10（p1=0 → 0.5） */
    TASK_C_LINE_LOST   = 2,  /* 8 路灰度全部 < p1（p1=0 → 60） */
    TASK_C_LINE_FOUND  = 3,  /* 至少一路 ≥ p1（p1=0 → 60） */
    TASK_C_CROSS       = 4,  /* 检测到横线/路口（满足后自动清） */
    TASK_C_CROSS_COUNT = 5,  /* 累计横线次数 ≥ p1 */
    TASK_C_ANGLE_DONE  = 6,  /* |角度环误差| ≤ p1/10（p1=0 → 1.0°） */
    TASK_C_DIST_MM     = 7,  /* 里程 ≥ p1 毫米 */
    TASK_C_FAULT       = 8,  /* Motion_GetFault() != 0 */
    TASK_C_ALWAYS      = 9,  /* 恒真（配合超时 = 纯等待） */
    TASK_C_CH_BLACK    = 10, /* ★「指定哪几路是黑的」（用户 m 2026-10-07）：**p1 当 8 位掩码**
                              *   （bit0..bit7 = 第 1..8 路），判据 = 勾中的那几路**当前全是黑的**
                              *   （其余通道不管）。掩码 = 0 ⇒ **恒不满足**（见 task_run.c 与文档）。
                              *   取"黑"用 trace_cal_channel(i)，判不出来时按"没压黑"保守处理。 */
    TASK_C_CH_WHITE    = 11, /* ★「指定哪几路是白的」（用户 m 2026-10-07）：上面那条的**镜像**。
                              *   同一个 8 位掩码，判据 = 勾中的那几路**当前一路都不黑**（其余不管）。
                              *   掩码 = 0 ⇒ **恒不满足**（与条件 10 口径一致；理由见 task_run.c）。
                              *   旧内置流程 renwu.c 有 5 段入口就是"指定那几路是白的"。 */
};

/* ---- 校验错误码（>=0 都是"没通过"的原因，负数是 TaskRun_Load 的返回值） ---- */
enum {
    TASK_OK        = 0,
    TASK_E_SHORT   = 1,  /* 长度不到一个头部 */
    TASK_E_MAGIC   = 2,
    TASK_E_VER     = 3,
    TASK_E_STATES  = 4,  /* n_states 为 0 或超上限 */
    TASK_E_MAIN    = 5,  /* main_state 越界 */
    TASK_E_RANGE   = 6,  /* 某个偏移/块数越界 */
    TASK_E_CRC     = 7,
    TASK_E_LEN     = 8,  /* total_len 与实际长度不符 */
    TASK_E_TAIL    = 9,  /* 序列最后一块不是"跳到状态/结束/带超时跳转的等待" */
};

/* 块（10 字节）—— 这里只是内存里的展开形式，载体永远是字节流，不做指针强转 */
typedef struct {
    uint8_t  op;
    uint8_t  flags;
    uint16_t p0;
    int16_t  p1;
    uint16_t p2;
    uint16_t p3;
} task_block_t;

/* 状态表项（8 字节） */
typedef struct {
    uint16_t name_off;    /* 0 = 无名 */
    uint16_t enter_off;   /* 0 = 没有进入块 */
    uint16_t body_off;
    uint8_t  n_enter;
    uint8_t  n_body;
} task_state_t;

/* CRC16/CCITT-FALSE（与 host_link.c:220 的 crc16_ccitt 同一份算法，校验值 "123456789" → 0x29B1） */
uint16_t TaskProg_Crc16(const uint8_t *d, uint32_t n);

static inline uint8_t  task_rd_u8 (const uint8_t *p) { return p[0]; }
static inline uint16_t task_rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static inline int16_t task_rd_i16(const uint8_t *p) { return (int16_t)task_rd_u16(p); }

void TaskProg_ReadBlock(const uint8_t *prog, uint32_t off, task_block_t *b);
void TaskProg_ReadState(const uint8_t *prog, uint8_t idx, task_state_t *st);

/* 返回 TASK_OK(0) 或 TASK_E_xxx */
int  TaskProg_Validate(const uint8_t *prog, uint32_t len);
const char *TaskProg_ErrStr(int e);

#endif /* __TASK_PROG_H__ */
