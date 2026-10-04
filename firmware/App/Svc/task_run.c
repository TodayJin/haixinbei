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
        Motion_SetSpeed((int)b->p0);
        return 1;

    case TASK_OP_SET_MODE:
        if ((b->flags & 0x01u) || b->p0 == (uint16_t)TASK_MODE_WHEELS) {
            /* 两轮速度直给：走速度环，关掉循迹/角度环接管 */
            Motion_SetMode(MOTION_IDLE);
            SpeedControl_SetTarget((int)b->p1, (int)b->p2);
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

    case TASK_OP_WAIT_COND:
        if (cond_ok(b->p0, b->p1)) {
            if (b->p0 == (uint16_t)TASK_C_CROSS) {
                tracing_cross_clear();
            }
            return 1;
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
