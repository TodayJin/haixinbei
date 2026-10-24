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
#define TASK_MAX_STATES   32u       /* ★2026-10-09：16 → 32。旧全程流程改成"主页分派 + 一段一状态"
                                     *   要 **22 个状态**（1 主页 + 21 段），16 装不下 —— 原来只能把连续段
                                     *   合并（状态名才写成「台阶2(段0/1/2)」）。n_states 是 u8 ⇒ 32 无压力。
                                     *   老任务（≤16 状态）照跑；含 17..32 状态的任务只有新固件认。 */
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
                                  * flags 一个字节装两件事，**按 bit7 分档**（两块语义互斥，低 7 位只有一个）：
                                  *   bit7 = 1 ⇒ 低 7 位 = **满足后**跳到的状态号（老功能，见 TASK_WAIT_COND_JUMP）
                                  *   bit7 = 0 ⇒ 低 7 位 = **该条件必须连续成立多少个 100ms** 才算满足
                                  *               （0 = 不要求持续；见 TASK_WAIT_COND_HOLD_MSK） */
    TASK_OP_GOTO        = 0x07,  /* p0 = 目标状态号 */
    TASK_OP_END         = 0x08,  /* 停车、任务结束 */
    TASK_OP_SET_SERVO   = 0x09,  /* p0 = 通道 1/2；p1 = 角度 0..180 */
    TASK_OP_CROSS_CLEAR = 0x0A,  /* 清横线/路口计数 */
    TASK_OP_CAL_SET     = 0x0B,  /* ★p0 = 用第几套灰度标定：0 = 普通 / 1 = 亚克力（用户 m 2026-10-07）。
                                  *   对应 trace_cal_select_set(p0)。两套标定一直存在（cal / cal_b），
                                  *   但在这块加出来之前，**跑任务时没有任何办法切换它**。 */
    TASK_OP_SET_STAGE   = 0x0C,  /* ★p0 = 段号。把"我走到第几段了"记一笔 —— 旧代码的 `black_num=M`。
                                  *   段号只有一个（**任务启动时清 0**），主页靠它做分派。 */
    TASK_OP_IF_STAGE    = 0x0D,  /* ★「**如果** 这几路压黑 ⇒ 跳到 p2」，不满足**往下走**
                                  *   —— 主页做多路分派用的那个 `if`（旧代码的 if/else-if 链）。
                                  *
                                  *   ★★**序号不用填**（用户 2026-10-10 定案）：主页循环体里
                                  *   **第 k 个本块 = 第 k+1 次压线** —— "现在是第几次"由解释器
                                  *   自己数（`s_dispatch_n` 每成功分派一次 +1，与块下标 `s_idx` 比对）。
                                  *   ⇒ 用户只填两样：**p1 哪几路压黑**、**p2 跳到哪个状态**。
                                  *   （旧代码 `if (8路全黑 && black_num==N)` 里的 `black_num==N`
                                  *    那一半就是"这是第几条判断"，不用写。）
                                  *
                                  *   p1 = 8 位掩码（bit0..bit7 = 第 1..8 路），判据与条件 10 同一份。
                                  *   ★**`p0` = 满足时间（毫秒）**（用户 2026-10-10："把这些
                                  *   如果就跳的加上滤波，就是满足时间，默认 100ms"）：
                                  *   要**连续成立 ≥ p0 毫秒**才分派，中途断一次就重新计时。
                                  *   `p0 = 0` ⇒ 不滤波（本拍成立即分派）—— **与加这个功能之前
                                  *   逐字节等价**（老镜像/老任务 p0 全是 0，固件以前也不读它）。
                                  *   用途：探头排横着装，**十字路口的横向黑线与探头排平行 ⇒
                                  *   8 路也会同时压黑**，但只持续几十毫秒（3.8cm 线 @1m/s ≈ 38ms），
                                  *   而障碍前的黑带面积大得多 ⇒ 要求持续 100ms 就能把路口滤掉。
                                  *   计时口径与 0x06 的「需持续」一致（见 task_run.c）。
                                  *
                                  *   ★与 0x06「等条件」的**根本区别**：0x06 会**停在那里等**它成立，
                                  *   本块**看一眼就走** —— 没有它，主页就写不出"判断①不行就看判断②"。
                                  *   ★本块只该放在**主页的循环体**里（进门序列的下标也从 0 开始，会撞号）。 */
    TASK_OP_CLIMB       = 0x0E,  /* ★「**爬台阶/爬坡：循环到车平**」（用户 m 2026-10-09）
                                  *   用在"上一个台阶是一串动作，但可能打滑上不去"的地方：
                                  *   **把这一段再来一遍**，直到"爬够了 且 车头真的落回水平"。
                                  *   p0 = 要爬几节（本块每被走到一次算一节，递减到 0）
                                  *   p1 = "抬起过"的判定阈值（0.1°；**0 = 不要求**，见下）
                                  *   p2 = 回落容差（0.1°；|当前俯仰角 − 基准| ≤ 它 = "车又平了"）
                                  *   p3 = 没满足时跳回的状态（**一般填本状态自己** ⇒ 把动作再来一遍）
                                  *
                                  *   基准俯仰角 = **本块第一次执行那一刻**的角度（车还在坡底/台阶前）；
                                  *   一次"爬台阶会话"按**段号**区分 ⇒ 同一个块放在大坡和台阶3 两处不会串。
                                  *   ★为什么要有 p1「抬起过」：**打滑滑下来**同样会让俯仰角"先抬头、
                                  *     再落回"，光看回落会把它误判成"爬上去了" ⇒ 要求先真的抬过 p1 那么高。
                                  *   ★不满足就**一直重试，没有超时** —— 反复爬比冲出去安全；要收手就在
                                  *     本块后面接「切换模式：停车」或「结束任务」。 */
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
 * 4 个参数位 p0..p3 已被"条件/阈值/超时/超时后跳"占满，第 5 个参数只能用那个 flags 字节。
 * flags 一共 8 位，要装**两件互不相干的事**，所以按 bit7 分档（低 7 位只有一个）：
 *
 *   bit7 = 1：低 7 位 = **满足后**跳到的状态号（与"超时后跳 p3"是两条独立出路）
 *   bit7 = 0：低 7 位 = **需持续** —— 该条件必须**连续**成立 (低7位)×100ms 才算满足；
 *             期间**一旦不成立就清零重来**；0 = 不要求持续（= 老行为）
 *
 * ★向后兼容（已核）：全仓库所有既有任务/镜像（renwu-lap.json、quancheng-v3.json 及车上
 *   那几条）的「等条件」块 flags **全是 0** ⇒ bit7 = 0 且低 7 位 = 0 ⇒ "不要求持续" ⇒
 *   **逐字节等价于加这个功能之前**。flags 非 0 的老块只有别的 op（0x02 两轮直给 bit0、
 *   0x03 相对角度 bit0），与本条无关。
 * ★为什么两件事不能同时用：低 7 位物理上只有一个。要"持续 N ms 之后再跳走"，
 *   **别用 bit7**，写成「等条件(需持续) 后面跟一块 0x07 跳到状态」—— 执行流完全等价
 *   （等待块满足后立刻进下一块 = 那块 GOTO 立刻跳）。
 * ★需持续只对**电平型**条件有意义（10/11 哪几路黑/白、2 丢线、3 找回线、1 回到中线…）。
 *   4「压到横线」是脉冲型：它满足时才会 `tracing_cross_clear()`，配"需持续"等于把清计数
 *   往后推 —— 能用但不是本意，别这么写。 */
#define TASK_WAIT_COND_JUMP     0x80u  /* bit7 = 1：条件满足后跳到 TASK_WAIT_COND_TGT_MSK 那个状态 */
#define TASK_WAIT_COND_TGT_MSK  0x7Fu  /* 低 7 位 = 目标状态号（状态最多 16 个，够用）—— 只在 bit7=1 时解释 */
#define TASK_WAIT_COND_HOLD_MSK 0x7Fu  /* 低 7 位 = 需持续的 100ms 数（0..127 ⇒ 0..12.7s）—— 只在 bit7=0 时解释 */
/* ★2026-10-24（用户要求）：单位从 100ms 改成 **10ms**（精度更高，范围 0..1270ms）。
 *   配套：上位机 taskedit.ts 的 WAIT_COND_HOLD_UNIT_MS 也要改成 10（两处必须一致）。 */
#define TASK_WAIT_COND_HOLD_UNIT_MS 10u   /* 上面那 7 位的单位：一格 = 10ms（0..127 => 0..1270ms） */

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
