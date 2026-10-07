/**
 * @file  task_run.c
 * @brief 任务程序解释器（状态机执行器）—— 全非阻塞，5ms 一拍
 *
 * 三段式结构：
 *   1) 载入：TaskRun_Load() 校验格式 + 把程序拷进 RAM（运行期不碰 Flash）
 *   2) 启动：TaskRun_Start() 从主页状态开始，先跑 enter 序列再跑 body 序列
 *   3) 推进：TaskRun_Tick() 每拍执行/轮询当前块；"等待类"块不满足就返回 0
 *
 * 安全约定（都能在主机测试 t_task.c 里看到）：
 *   - 单拍最多推进 TICK_MAX_BLOCKS 个块 ⇒ GOTO 自环不会把主循环卡死
 *   - 未知 op / 越界状态一律"跳过"或"忽略"，绝不越界读
 *   - END 与 TaskRun_Stop() 都会 Motion_SetMode(IDLE) + SpeedControl_Stop()，车一定停
 */
#include <math.h>
#include <string.h>

#include "task_run.h"
#include "task_prog.h"
#include "app_ui.h"        /* ★g_cfg.beep_marker：「过点蜂鸣」（切状态时响一下）*/
#include "buzzer.h"        /* ★beep_ms()：蜂鸣器 */
#include "gyro_control.h"
#include "host_link.h"     /* HP_TASK_SPEED + host_param_get：启动时的默认基准速度 */
#include "motion.h"
#include "PID_tracing.h"
#include "servo.h"
#include "speed_control.h"
#include "trace_cal.h"

#define TICK_MAX_BLOCKS   32u    /* 单拍最多推进的块数（防自环） */
#define DIST_DT_MAX_MS    200u   /* 里程积分的单拍上限（主循环被拖长时的钳位） */
#define GRAY_LOST_THR     60u    /* "丢线"默认灰度阈值（0..255，越大越宽松） */
#define ANGLE_TOL_DEFAULT 10u    /* 0.1° 单位 → 1.0° */

static uint8_t  s_prog[TASK_RUN_MAX_PROG];
static uint16_t s_len;
static uint8_t  s_n_states;
static uint8_t  s_loaded;

static uint16_t s_id;
static uint8_t  s_running;
static uint8_t  s_done;
static uint8_t  s_state;
static uint8_t  s_phase;      /* 0 = enter 序列，1 = body 序列 */
static uint16_t s_idx;        /* 序列内块下标 */
static uint16_t s_off;        /* 序列起始偏移 */
static uint16_t s_cnt;        /* 序列块数 */
static uint8_t  s_started;    /* 当前块是否已开始（决定"起始时刻"） */
static uint8_t  s_inited;     /* 当前块的"首拍动作"是否做过 */

static uint32_t s_now;
static uint32_t s_tick_last;
static uint32_t s_t0_block;
static uint32_t s_elapsed;
/* ★「需持续」的连续计时（TASK_WAIT_COND 的 flags 低 7 位，见 task_prog.h）：
 *   s_hold_run = 0 表示"条件当前不成立（或还没开始计时）"，= 1 表示正在累计。
 *   s_hold_from = 这一轮**连续成立**的起始时刻；条件断一次就 s_hold_run = 0，
 *   下次成立时重新取 s_now ⇒ **断一次重新计时**（不是累计、不是打折扣）。
 *   进入每一块时清零（见 TaskRun_Tick 的 `if (!s_started)`）：否则连着两块都成立的
 *   「等条件」会继承上一块的起始时刻、第二块当场就"持续够久"了。 */
static uint8_t  s_hold_run;
static uint32_t s_hold_from;
static float    s_dist_mm;
static uint16_t s_cross_n;
static uint8_t  s_cross_last;
static uint8_t  s_flags;
static float    s_turn_target;

/* ------------------------------------------------------------------ */
/* 状态切换                                                            */
/* ------------------------------------------------------------------ */
static void seq_begin(uint8_t st)
{
    task_state_t s;

    TaskProg_ReadState(s_prog, st, &s);
    s_state = st;
    /* ★「过点蜂鸣」（用户 m 2026-10-07）：**每次切换状态响一下**。
     *   这个开关原来属于旧内置流程 `renwu.c`（2026-10-03 转成"上位机编排 + 解释器"那套时，
     *   蜂鸣器/RGB 那两下被丢了）⇒ 菜单里的开关一直是**孤儿**（声明/默认/NVRAM 都在，没人读）。
     *   现在接回：切状态就"嘀"一声，按键蜂鸣（beep_key）不受影响。 */
    if (g_cfg.beep_marker) AppUi_Beep(20);
    if (s.n_enter > 0u) {
        s_phase = 0u;
        s_off   = s.enter_off;
        s_cnt   = s.n_enter;
    } else {
        s_phase = 1u;
        s_off   = s.body_off;
        s_cnt   = s.n_body;
    }
    s_idx     = 0u;
    s_started = 0u;
}

/* ------------------------------------------------------------------ */
/* 条件求值（TASK_OP_WAIT_COND）                                        */
/* ------------------------------------------------------------------ */
static uint8_t gray_thr(int16_t p1)
{
    if (p1 <= 0 || p1 > 255) {
        return GRAY_LOST_THR;
    }
    return (uint8_t)p1;
}

static float angle_tol(int16_t p2)
{
    return ((p2 == 0) ? (float)ANGLE_TOL_DEFAULT : (float)p2) / 10.0f;
}

/* 「这一路压黑线吗」—— 给 TASK_C_CH_BLACK 逐路取位。
 * 真值优先用 trace_cal_channel()（1 = 压黑线 / 0 = 没压）；它回 **-1** 表示"这一路现在
 * 给不出真值"（没标定 / 这一路标定无效 / 用户显式选了数字口模式要调用者回落），
 * 这时退回同一份柱高的半高判定（= 界面上那个 0/1 的口径，数字口模式下是 0/255 真值）。
 * 仍判不出来就当**没压黑**：方向必须是保守的 —— 判不出来若当成"黑"，标定一坏这个条件
 * 会立刻满足、车提前动作；当成"不黑"最多是卡在等待里，还有超时兜底。 */
static uint8_t ch_black(uint8_t i)
{
    int r = trace_cal_channel(i);

    if (r >= 0) {
        return (uint8_t)r;
    }
    return (trace_cal_level(i) >= 128u) ? 1u : 0u;
}

static int cond_ok(uint16_t c, int16_t thr)
{
    uint8_t i;

    switch (c) {
    case TASK_C_LINE_CENTER: {
        float tol = ((thr == 0) ? 0.5f : (float)thr / 10.0f);
        return (fabsf(PID_Tracing_TracePos() - 4.5f) <= tol) ? 1 : 0;
    }
    case TASK_C_LINE_LOST:
        for (i = 0u; i < 8u; i++) {
            if (trace_cal_level(i) >= gray_thr(thr)) {
                return 0;                       /* 有一路压线就不算丢 */
            }
        }
        return 1;
    case TASK_C_LINE_FOUND:
        for (i = 0u; i < 8u; i++) {
            if (trace_cal_level(i) >= gray_thr(thr)) {
                return 1;
            }
        }
        return 0;
    case TASK_C_CROSS:
        return (tracing_cross() != 0u) ? 1 : 0;
    case TASK_C_CROSS_COUNT:
        return (thr > 0 && s_cross_n >= (uint16_t)thr) ? 1 : 0;
    case TASK_C_ANGLE_DONE:
        return (fabsf(GyroControl_HeadingError()) <= angle_tol(thr)) ? 1 : 0;
    case TASK_C_DIST_MM:
        return (thr > 0 && s_dist_mm >= (float)thr) ? 1 : 0;
    case TASK_C_FAULT:
        return (Motion_GetFault() != 0u) ? 1 : 0;
    case TASK_C_ALWAYS:
        return 1;
    case TASK_C_CH_BLACK: {
        /* ★「指定哪几路是黑的」（用户 m 2026-10-07）：thr = **8 位掩码**（bit0..bit7 = 第 1..8 路），
         *   判据 = **勾中的那几路当前全是黑的**（没勾的通道一律不管）。
         *   掩码 = 0 ⇒ **恒不满足**（显式定的语义）：0 路"全黑"是空真，会让这个条件永远立刻
         *   通过 —— 那是陷阱不是本意。掩码只取低 8 位（界面只给 0..255）。 */
        uint16_t m = (uint16_t)thr & 0xFFu;

        if (m == 0u) {
            return 0;
        }
        for (i = 0u; i < 8u; i++) {
            if ((m & (uint16_t)(1u << i)) != 0u && ch_black(i) == 0u) {
                return 0;                       /* 勾中的某一路没压黑线 ⇒ 不满足 */
            }
        }
        return 1;
    }
    case TASK_C_CH_WHITE: {
        /* ★「指定哪几路是白的」= 上面那条件的镜像：同一个掩码，判据 = **勾中的那几路一路都不黑**
         *   （没勾的通道一律不管）。旧内置流程 renwu.c 有 5 段入口就是这个意思（data[0..4]==0 等）。
         *   掩码 = 0 ⇒ **恒不满足**，与条件 10 一字不差：0 路的"全白"同样是空真，会让这个条件
         *   永远立刻通过（把状态机瞬间推过去），那是陷阱不是本意。
         *   复用的是同一个 ch_black()（含它的回落与"判不出来当没压黑"），所以两个条件对同一份
         *   灰度数据永远给出一致的判断，不会出现"一路既算黑又算白"的中间态。 */
        uint16_t m = (uint16_t)thr & 0xFFu;

        if (m == 0u) {
            return 0;
        }
        for (i = 0u; i < 8u; i++) {
            if ((m & (uint16_t)(1u << i)) != 0u && ch_black(i) != 0u) {
                return 0;                       /* 勾中的某一路压着黑线 ⇒ 不满足 */
            }
        }
        return 1;
    }
    default:
        return 0;                               /* 不认识的条件：永不满足（靠超时脱身） */
    }
}

/* ------------------------------------------------------------------ */
/* 单块推进                                                            */
/*   返回 0 = 还在等（本拍到此为止）                                    */
/*   返回 1 = 本块完成，进下一块                                        */
/*   返回 2 = 执行流已经改变（跳状态/结束），不要再 ++ 下标              */
/* ------------------------------------------------------------------ */
static int block_step(const task_block_t *b)
{
    switch (b->op) {
    case TASK_OP_SET_SPEED:
        /* ★p0 是 u16 ⇒ 按 i16 符号扩展（否则负基准速度表达不出来；
         *   旧全程「超级大坡」段要 RW(-900) 那种倒车段。向后兼容：现网速度都 <32768）。 */
        Motion_SetSpeed((int)(int16_t)b->p0);
        return 1;

    case TASK_OP_SET_MODE:
        if ((b->flags & 0x01u) || b->p0 == (uint16_t)TASK_MODE_WHEELS) {
            /* 两轮速度直给：走速度环，关掉循迹/角度环接管 */
            Motion_SetMode(MOTION_IDLE);
            /* ★p2 是 u16 ⇒ 按 i16 符号扩展：否则「两轮直给 −3600」编码成 61936，
             *   车上变成"左轮倒转 + 右轮顶到 +7200" = **原地打转，不是倒车**（已实锤）。
             *   向后兼容：现网所有任务速度都 <32768。 */
            SpeedControl_SetTarget((int)b->p1, (int)(int16_t)b->p2);
            SpeedControl_Enable(1u);
        } else {
            /* **只有真的要换模式才先停速度环**（用户 m28949：跑循迹"根本就不动"）。
             * 状态机的 enter 序列每次进状态都会重放，而 Motion_SetMode() 对"同模式"
             * 是空操作（motion.c:80 `if (m == s_mode) return;`）—— 于是
             * "SET_MODE 3 + GOTO 自己"这种任务每拍都会回来重跑 enter 块：
             *   SpeedControl_Stop() 把 PWM 清 0、把速度环关掉，紧接着的
             *   Motion_SetMode(3) 又因为是同模式直接返回 ⇒ 速度环再没人打开，
             *   车"站着不动"（还把 5ms 环刚写好的 PWM 反复清掉）。
             * 用"当前模式"做判据：同模式就当它已经切好了，什么都不做。 */
            Motion_Mode want = (Motion_Mode)b->p0;

            if (Motion_GetMode() != want) {
                SpeedControl_Stop();
                Motion_SetMode(want);
            }
        }
        return 1;

    case TASK_OP_TURN_DEG: {
        float tol = angle_tol(b->p2);

        if (!s_inited) {
            float heading;
            Motion_SetMode(MOTION_GYRO);
            /* 角度环自己的"当前航向" = 目标 - 误差（比直接读 IMU 全局更贴近环的真实口径） */
            heading = NormalizeAngle(GyroControl_GetTargetAngle() - GyroControl_HeadingError());
            s_turn_target = NormalizeAngle(heading + (float)b->p1);
            GyroControl_SetTargetAngle(s_turn_target);
            s_inited = 1u;
        }
        if (fabsf(GyroControl_HeadingError()) <= tol) {
            return 1;
        }
        if (b->p0 != 0u && (s_now - s_t0_block) >= (uint32_t)b->p0) {
            s_flags |= TASK_RUN_F_TIMEOUT;
            return 1;
        }
        return 0;
    }

    case TASK_OP_ACTION:
        if (!s_inited) {
            Servo_ApplyAction((int)b->p0);
            s_inited = 1u;
        }
        if (b->p2 != 0u && (s_now - s_t0_block) < (uint32_t)b->p2) {
            return 0;
        }
        return 1;

    case TASK_OP_WAIT_MS:
        return ((s_now - s_t0_block) >= (uint32_t)b->p0) ? 1 : 0;

    case TASK_OP_WAIT_COND: {
        /* ★「需持续」（用户 m 2026-10-07：两段减速带 = "扫到两段超过 1s 的全黑"）。
         *   flags 按 bit7 分档：bit7=1 时低 7 位是"满足后跳到的状态号"，那种块**不支持**
         *   需持续（要两者都有，就写成"等条件(需持续) + 下一块 GOTO"）；bit7=0 时低 7 位
         *   才是"需要连续成立多少个 100ms"。低 7 位 = 0 ⇒ 需持续 0ms ⇒ **本拍成立即满足**，
         *   与加这个功能之前逐字节等价（老任务 flags 全是 0）。
         *   计时口径：条件首次成立的那一拍记起始时刻（那一拍算已持续 0ms），此后每 5ms 一拍
         *   检查 now - from >= 需持续；**中途假一次就把这一轮作废**，下次成立重新记起点。
         *   全程不阻塞（本函数每拍被 TaskRun_Tick 调一次），也不影响"超时后跳 p3"那条出路。 */
        uint8_t  ok     = (uint8_t)cond_ok(b->p0, b->p1);
        uint8_t  hold_n = ((b->flags & TASK_WAIT_COND_JUMP) != 0u)
                              ? 0u
                              : (uint8_t)(b->flags & TASK_WAIT_COND_HOLD_MSK);
        uint32_t need   = (uint32_t)hold_n * TASK_WAIT_COND_HOLD_UNIT_MS;

        if (ok == 0u) {
            s_hold_run = 0u;                     /* 条件断了 ⇒ 这一轮作废，计时清零重来 */
        } else {
            if (s_hold_run == 0u) {
                s_hold_run  = 1u;
                s_hold_from = s_now;             /* 这一拍刚开始连续成立 */
            }
            if ((s_now - s_hold_from) >= need) { /* need = 0 ⇒ 恒真 ⇒ 老行为 */
                if (b->p0 == (uint16_t)TASK_C_CROSS) {
                    tracing_cross_clear();
                }
                /* ★flags.bit7（用户 m 2026-10-07）：「满足后」去哪。
                 *   bit7 = 1 ⇒ 跳到低 7 位的状态号（和 p3 的"超时后跳"是两条独立出路）；
                 *   bit7 = 0 ⇒ **满足后继续往下走一块**（老任务 flags 全是 0 ⇒ 行为一字不变）。 */
                if ((b->flags & TASK_WAIT_COND_JUMP) != 0u) {
                    uint8_t tgt = (uint8_t)(b->flags & TASK_WAIT_COND_TGT_MSK);

                    if (tgt < s_n_states) {
                        seq_begin(tgt);
                        return 2;
                    }
                    /* 目标状态越界（程序被改过）：当"继续往下"处理 —— 绝不越界跳 */
                }
                return 1;
            }
        }
        if (b->p2 != 0u && (s_now - s_t0_block) >= (uint32_t)b->p2) {
            s_flags |= TASK_RUN_F_TIMEOUT;       /* 超时（无论改不改道都要留下痕迹） */
            if (b->p3 != 0xFFFFu && b->p3 < s_n_states) {
                seq_begin((uint8_t)b->p3);       /* 超时改道 */
                return 2;
            }
            return 1;
        }
        return 0;
    }

    case TASK_OP_GOTO:
        if (b->p0 < s_n_states) {
            seq_begin((uint8_t)b->p0);
            return 2;
        }
        return 1;

    case TASK_OP_END:
        TaskRun_Stop();
        s_done = 1u;
        s_flags |= TASK_RUN_F_DONE;
        return 2;

    case TASK_OP_SET_SERVO: {
        int16_t a = b->p1;
        if (a < 0)     a = 0;
        if (a > 180)   a = 180;
        Servo_Control((int)b->p0, (uint8_t)a);
        return 1;
    }

    case TASK_OP_CROSS_CLEAR:
        tracing_cross_clear();
        return 1;

    case TASK_OP_CAL_SET:                        /* ★切灰度标定套：0 = 普通 / 1 = 亚克力 */
        trace_cal_select_set((uint8_t)(b->p0 & 1));
        return 1;

    default:
        return 1;                                /* 未知块：跳过（格式校验已拦，这里兜底） */
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
int TaskRun_Load(const uint8_t *prog, uint32_t len)
{
    int e;

    if (s_running) {
        return -1;                               /* 跑着不换程序 */
    }
    if (prog == 0 || len < TASK_HDR_LEN || len > TASK_RUN_MAX_PROG) {
        return -(int)TASK_E_LEN;
    }
    e = TaskProg_Validate(prog, len);
    if (e != TASK_OK) {
        return -e;
    }
    memcpy(s_prog, prog, len);
    s_len      = (uint16_t)len;
    s_n_states = s_prog[3];
    s_loaded   = 1u;
    return 0;
}

int TaskRun_Start(uint16_t id)
{
    if (!s_loaded) {
        return -1;
    }
    TaskRun_Stop();                              /* 幂等：先把上一次停干净 */

    s_id        = id;
    s_running   = 1u;
    s_done      = 0u;
    s_flags     = 0u;
    s_dist_mm   = 0.0f;
    s_cross_n   = 0u;
    s_cross_last = 0u;
    s_elapsed   = 0u;
    s_tick_last = 0u;
    s_t0_block  = 0u;
    s_inited    = 0u;
    s_turn_target = 0.0f;
    s_hold_run  = 0u;                            /* 「需持续」计时：新任务从零开始 */
    s_hold_from = 0u;

    Motion_ClearFault();
    tracing_cross_clear();
    /* 默认基准速度：任务块里没有显式给速度时按 HP_TASK_SPEED 跑（默认 6480，即旧板
     * "900 刻度"在本板 PWM 满量程 7200 下的等价量）。这个参数以前只有旧的内置比赛流程
     * 在用，旧流程删掉后由解释器接手 —— 参数值/范围/上位机页面都保持不变。 */
    Motion_SetSpeed((int)host_param_get(HP_TASK_SPEED, 6480.0f));
    seq_begin(s_prog[4]);                        /* 从主页状态开始 */
    return 0;
}

void TaskRun_Stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = 0u;
    Motion_SetMode(MOTION_IDLE);
    SpeedControl_Stop();
    s_started = 0u;                              /* 下次启动重新开始 */
}

void TaskRun_Tick(uint32_t now_ms)
{
    uint32_t dt;
    uint32_t guard = 0u;
    task_block_t b;

    if (!s_running) {
        return;
    }

    dt = (s_tick_last == 0u || now_ms < s_tick_last) ? 0u : (now_ms - s_tick_last);
    if (dt > DIST_DT_MAX_MS) {
        dt = DIST_DT_MAX_MS;
    }
    s_tick_last = now_ms;
    s_now       = now_ms;
    s_elapsed  += dt;

    {   /* 里程：用平滑后的轮速积分（对 dt 不敏感，主循环抖动不会累积成大偏差） */
        int l = SpeedControl_SpeedMmps(0);
        int r = SpeedControl_SpeedMmps(1);
        float al = (float)((l < 0) ? -l : l);
        float ar = (float)((r < 0) ? -r : r);
        s_dist_mm += 0.5f * (al + ar) * ((float)dt / 1000.0f);
    }
    {   /* 横线计数：抓上升沿 */
        uint8_t c = tracing_cross();
        if (c != 0u && s_cross_last == 0u) {
            s_cross_n++;
        }
        s_cross_last = c;
    }

    while (s_running && guard++ < TICK_MAX_BLOCKS) {
        if (s_idx >= s_cnt) {
            if (s_phase == 0u) {                 /* enter 序列跑完 → 转 body */
                task_state_t st;
                TaskProg_ReadState(s_prog, s_state, &st);
                s_phase   = 1u;
                s_off     = st.body_off;
                s_cnt     = st.n_body;
                s_idx     = 0u;
                s_started = 0u;
                if (s_cnt == 0u) {
                    break;                       /* 空 body：状态停住 */
                }
                continue;
            }
            break;                               /* body 跑完：状态停住，等跳转/停止 */
        }

        if (!s_started) {
            s_t0_block = now_ms;
            s_started  = 1u;
            s_inited   = 0u;
            s_hold_run = 0u;                     /* ★换块了：「需持续」的连续计时必须重新开始
                                                  *   （否则连着两块都成立的「等条件」会继承
                                                  *    上一块的起点、第二块当场就算"持续够久"） */
        }
        TaskProg_ReadBlock(s_prog, (uint32_t)s_off + (uint32_t)s_idx * TASK_BLOCK_LEN, &b);

        {
            int r = block_step(&b);
            if (r == 0) {
                break;                           /* 还在等 */
            }
            if (r == 2) {
                continue;                        /* 执行流已换，别再 ++ */
            }
            s_idx++;
            s_started = 0u;
        }
    }
}

uint8_t  TaskRun_IsRunning(void)   { return s_running; }
uint16_t TaskRun_Id(void)          { return s_id; }
uint8_t  TaskRun_State(void)       { return (s_loaded ? s_state : (uint8_t)TASK_RUN_OFF); }
uint8_t  TaskRun_Phase(void)       { return s_phase; }
uint16_t TaskRun_Block(void)       { return s_idx; }
uint16_t TaskRun_BlockCount(void)  { return s_cnt; }
uint32_t TaskRun_ElapsedMs(void)   { return s_elapsed; }
uint32_t TaskRun_BlockMs(void)     { return (s_started ? (s_now - s_t0_block) : 0u); }
uint8_t  TaskRun_Flags(void)       { return s_flags; }
uint8_t  TaskRun_Loaded(void)      { return s_loaded; }
uint16_t TaskRun_ProgLen(void)     { return s_len; }
uint16_t TaskRun_CrossCount(void)  { return s_cross_n; }
uint32_t TaskRun_DistMm(void)      { return (uint32_t)s_dist_mm; }
uint8_t  TaskRun_Done(void)        { return s_done; }
