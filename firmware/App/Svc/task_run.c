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
#include "jy61p.h"         /* ★Pitch：「爬台阶循环到车平」靠它判断车头平不平 */
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
/* ★「如果…就跳」的满足时间（`TASK_OP_IF_STAGE` 的 p0，用户 2026-10-10："加上滤波，就是
 *   满足时间，默认 100ms"）。**刻意不复用上面的 s_hold_run**，因为 0x0D 是"看一眼就走"：
 *   成立/不成立都返回 1 ⇒ 解释器 `s_idx++` 并把 `s_started = 0`，下一遍重进这一块时
 *   TaskRun_Tick 的 `if (!s_started) s_hold_run = 0` 会把计时清零 —— 拿它计时永远到不了。
 *   这里用独立计时器，时基是 `s_now`（毫秒），所以"一拍里主页 body 被走几遍"
 *   （GOTO 自环 + TICK_MAX_BLOCKS）都不影响读数。
 *   `s_if_dn` = 正在计时的是**哪一次分派**（s_dispatch_n）；换了下一条判断就重新计时。
 *   p0 = 0 ⇒ 需持续 0ms ⇒ 本拍成立即分派，**与加这个功能之前逐字节等价**（老任务 p0 全是 0）。 */
static uint8_t  s_if_run;
static uint32_t s_if_from;
static uint8_t  s_if_dn;
static float    s_dist_mm;
static uint16_t s_cross_n;
static uint8_t  s_cross_last;
static uint8_t  s_flags;
static float    s_turn_target;
/* ★「段号」（用户 2026-10-09）：旧代码 `black_num` 的那个变量。主页用 `TASK_OP_IF_STAGE`
 *   做分派（"如果 段号==N 且 这几路压黑 ⇒ 跳状态"），各段末尾用 `TASK_OP_SET_STAGE` 记下一段，
 *   执行完「跳到状态(主页)」回来接着分派 —— 就是旧代码 `void A()` 的 if/else-if 链。
 *   任务启动时清 0（见 TaskRun_Start），所以同一份程序重跑一定从第 0 段开始。 */
static uint8_t  s_stage;

/* ★「已经分派过几次」（用户 2026-10-10：段号那一格删掉，不许让用户填）：
 *   主页循环体里**第 k 个**「如果…就跳」= 第 k+1 次压线 ⇒ 用它和块在当前序列里的下标
 *   `s_idx` 比对即可，用户不用在块里填任何序号。每成功分派一次 +1。 */
static uint8_t  s_dispatch_n;

/* ★「爬台阶」会话（TASK_OP_CLIMB，用户 m 2026-10-09）：一次爬台阶/爬坡的上下文。
 *   基准角 = 本块第一次执行那一刻的俯仰角（车还在坡底/台阶前）；会话按**段号**区分，
 *   所以同一个块放在"大坡"和"台阶3"两处不会互相串。 */
static uint8_t  s_climb_on;      /* 1 = 正在爬（每拍盯俯仰角） */
static uint8_t  s_climb_stage;   /* 这次会话属于哪一段 */
static uint8_t  s_climb_left;    /* 还剩几节要爬 */
static uint8_t  s_climb_lifted;  /* 期间有没有真的抬起来过 */
static float    s_climb_base;    /* 基准俯仰角（度） */
static float    s_climb_lift_at; /* 抬起判定线 = 基准 + 阈值；≤0 = 不要求 */

/* ------------------------------------------------------------------ */
/* 爬台阶会话                                                          */
/* ------------------------------------------------------------------ */
/* ★「爬台阶（循环到车平）」的会话必须在**进入状态那一刻**就开起来，不能等块被执行到：
 *   抬头发生在**动作执行期间** —— 等走到那个块时，车可能已经爬上去了（角度早已回落）
 *   或者已经打滑滑下来，两个方向都会判错（前者会永远判不通过、后者会误判成功）。
 *   所以 seq_begin() 一进状态就扫一遍 body 里有没有这个块：有就记基准（= 此刻的角度，
 *   车还在坡底/台阶前）、设计数、算好抬起判定线。
 *   ★幂等：同一个状态**已经在爬**时直接返回 —— "没上去就跳回自己"会再调一次 seq_begin()，
 *   重开会把计数和基准一起重置，那就永远爬不完了。 */
static void climb_begin_if_any(uint8_t st)
{
    task_state_t s;
    uint16_t i;
    task_block_t b;

    if (s_climb_on != 0u && s_climb_stage == st) {
        return;                                  /* 就是"跳回自己再来一遍" ⇒ 别重置 */
    }
    TaskProg_ReadState(s_prog, st, &s);
    for (i = 0u; i < s.n_body; i++) {
        TaskProg_ReadBlock(s_prog, (uint32_t)s.body_off + (uint32_t)i * TASK_BLOCK_LEN, &b);
        if (b.op != TASK_OP_CLIMB) {
            continue;
        }
        s_climb_base    = Pitch;
        s_climb_left    = (uint8_t)((b.p0 > 0u) ? (b.p0 & 0xFFu) : 1u);
        s_climb_lifted  = 0u;
        s_climb_lift_at = (b.p1 > 0) ? (s_climb_base + (float)b.p1 / 10.0f) : 0.0f;
        s_climb_on      = 1u;
        s_climb_stage   = st;
        return;
    }
    s_climb_on = 0u;                             /* 这个状态里没有爬台阶块 ⇒ 会话到此结束 */
}

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
    /* ★2026-10-13（用户："转换状态时响一声（50ms）就可以"）：20ms 太短，听感像"咔"一下，
     *  改成 50ms。保险丝 BEEP_MAX_MS(150ms) 离它很远，不会误掐。 */
    if (g_cfg.beep_marker) AppUi_Beep(50);
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
    climb_begin_if_any(st);                      /* ★爬台阶会话：进状态就开（幂等，见上） */
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

/* TASK_C_CH_BLACK 的"扫过即算"锁存（约 200ms，按任务 5ms/拍） */
#define CH_BLACK_LATCH_TICKS 40u
static uint16_t s_bl_latch = 0u;
/* ★2026-10-26（用户）：TURN_DEG 的“左右轮缩放”只作用于**这一块**。
 * p3 = 百分数（0 = 用全局值）；进块时临时改全局缩放，出块时恢复。 */
static uint16_t s_turn_scale_save_r = 0xFFFFu;
static uint16_t s_turn_scale_save_l = 0xFFFFu;
static uint16_t s_bl_last_m = 0u;
/* ★审计 A-02 回归修正：只有“不要求持续”（hold_n==0）时才允许锁存。
 * 否则锁存会让任何 ≤200ms 的“需持续”直接失效（等于没要求）。 */
static uint8_t  s_cond_latch_ok = 1u;

/* ★2026-10-24 审计 F-06："这一路的判断可信吗"。
 * 为什么需要：ch_black() 在"判不出来"（弱帧/没标定/这一路标定无效）时返回 0，
 *   这对「等黑」是保守的（要求"确凿是黑"才算），
 *   但对「等白」就是**反保守**：判不出来也被当成"不黑" ⇒ "全白"当场成立
 *   ⇒ 弱帧/丢线/刚上电没标定时，状态机被瞬间推过去。
 * 这里把"可信"单独暴露出来，让 CH_WHITE 要求"确凿是白"。 */
static uint8_t ch_known(uint8_t i)
{
    return (trace_cal_channel(i) >= 0) ? 1u : 0u;
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
        uint8_t allblack = 1u;

        if (m == 0u) {
            s_bl_last_m = 0u;
            s_bl_latch = 0u;
            return 0;
        }
        /* 2026-10-24（用户："等扫到指定黑总是超级不稳定，有时候有有时候没"）：
         * 根因在下游的滤波里，两条都会让"扫到了却没算到"：
         *   1) trace_cal_level() 在弱帧(TRACE_FRAME_WEAK) 时直接返回 0
         *      => 一帧被判弱帧，所有通道瞬间都"不黑" => 条件漏掉；
         *   2) 好帧的 0/1 走 5 次多数表决(s_vote_lvl) => 车快速扫过一条线时
         *      这一路只在 1~2 帧里是黑的 => 被表决滤掉 => 快扫不触发、慢扫才触发。
         * 修法（不动 trace_cal 那套滤波，避免影响循迹）：这里加"扫过即算"的锁存 ——
         *   任意一拍满足就锁存 CH_BLACK_LATCH_TICKS（约 200ms），期间一直算满足。
         *   换掩码时自动清锁存，避免上一步的残留让下一步秒过。 */
        if (m != s_bl_last_m) {
            s_bl_last_m = m;
            s_bl_latch = 0u;
        }
        for (i = 0u; i < 8u; i++) {
            if ((m & (uint16_t)(1u << i)) != 0u && ch_black(i) == 0u) {
                allblack = 0u;
                break;
            }
        }
        if (allblack != 0u) {
            if (s_cond_latch_ok != 0u) s_bl_latch = CH_BLACK_LATCH_TICKS;
            return 1;
        }
        if (s_cond_latch_ok != 0u && s_bl_latch > 0u) {
            s_bl_latch--;
            return 1;                       /* 锁存期：刚刚扫到过 => 算满足 */
        }
        return 0;
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
            if ((m & (uint16_t)(1u << i)) == 0u) {
                continue;
            }
            /* 审计 F-06：必须是"**确凿**不黑"，判不出来（弱帧/未标定）不算白 */
            if (ch_known(i) == 0u || ch_black(i) != 0u) {
                return 0;
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
            /* 两轮速度直给：走速度环，关掉循迹/角度环接管。
             *
             * ★2026-10-13（用户："两轮直给都填的负数（倒车），加了之后很容易不执行"）：
             *   这句注释一直写着"关掉循迹/角度环接管"，但代码只调了 Motion_SetMode(IDLE) ——
             *   那只管"运动模式"这一层。**后台的那两个环照样在 Motion_Task() 里每 5ms 跑**：
             *
             *     motion.c:190  if (GyroControl_IsEnabled()) GyroControl_Task();
             *
             *   而 GyroControl_Task() 最后会
             *     gyro_control.c:1025  left = base - corr; right = base + corr;
             *                          SpeedControl_SetTarget(left, right);
             *   **直接把我们刚直给的轮速盖掉**。直给负速度时尤其致命：车往后走、线离开
             *   传感器 ⇒ 8 路全丢 ⇒ PID_tracing 的丢线保护（PID_tracing.c:873）
             *   `GyroControl_Stop(); SpeedControl_Stop();` ⇒ 连速度环一起关掉 ⇒ PWM=0 ⇒
             *   **车一动不动**，可解释器还在正常往下跑（上位机显示"已启动"）。
             *   只往前开不会犯：车一直压着线，丢线保护不开火。这就是"加了倒车才容易复发、
             *   复位一次再下发就好"的由来（复位后这些环还没被打开）。
             *
             *   所以这里按注释的原意把两个后台环都停掉：直给是**开环手动**命令，
             *   不允许任何后台环来抢速度环。切回循迹/角度环由后面的 SET_MODE 分支负责开。 */
            GyroControl_Stop();          /* 停角度环：它每 5ms 会覆盖轮速目标 */
            PID_Tracing_Timer_Stop();    /* 停后台循迹环（同一类劫持，幂等） */
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
            /* ★2026-10-21 关键修复（用户：「任务里站着转角度特别慢，IMU 里正常」）：
             * 这里原来**没有把基准速度清 0** ⇒ 若任务前面 SET_SPEED 过（如 4600），
             * 转弯就变成"边走边转"：left=4600-corr, right=4600+corr，
             * 而 corr 上限 = rate_kp×rmax = 10×120 = 1200 ⇒ 差速只有 ±1200
             * ⇒ 两侧 3400/5800 ⇒ 温和地拐 ⇒ **很慢**。
             * 而上位机 IMU 页的 HOLD 是 base=0 的真原地转（left=-corr, right=+corr），
             * 全部差速都用来转 ⇒ 快。两者口径不同，所以"IMU 正常、任务慢"。
             * 修法：TURN_DEG 一开始就把基准速度清 0 ⇒ 与 IMU 页一致。 */
            /* ★2026-10-24 审计 D-06：上一版只调 GyroControl_SetBaseSpeed(0,0) **无效** ——
             * motion.c:184 每 5ms 又用 s_base 覆盖回去了，而 TaskRun_Start 把 s_base
             * 设成 HP_TASK_SPEED（默认 7000）=> 任务里的"原地转"实际仍是边走边转 => 很慢。
             * 正确修法：连**任务层的基准速度**一起清 0（Motion_SetSpeed 才是 s_base 的主人）。 */
            Motion_SetSpeed(0);
            GyroControl_SetBaseSpeed(0, 0);
        /* ★这一块专用的"左右轮缩放"（p3，%）：0 = 用全局值（不覆盖）。
         * 30..200 才认，避免写坏的值把轮子卡死。出块时（完成/超时）恢复全局值。 */
        if (b->p3 >= 30u && b->p3 <= 200u) {
            if (s_turn_scale_save_r == 0xFFFFu) {
                s_turn_scale_save_r = (uint16_t)host_param_get(HP_GYRO_RIGHT_SCALE, 70.0f);
                s_turn_scale_save_l = (uint16_t)host_param_get(HP_GYRO_LEFT_SCALE,  70.0f);
            }
            host_param_set_local(HP_GYRO_RIGHT_SCALE, (float)b->p3);
            host_param_set_local(HP_GYRO_LEFT_SCALE,  (float)b->p3);
        }
            /* 角度环自己的"当前航向" = 目标 - 误差（比直接读 IMU 全局更贴近环的真实口径） */
            heading = NormalizeAngle(GyroControl_GetTargetAngle() - GyroControl_HeadingError());
            s_turn_target = NormalizeAngle(heading + (float)b->p1);
            GyroControl_SetTargetAngle(s_turn_target);
            s_inited = 1u;
        }
        if (fabsf(GyroControl_HeadingError()) <= tol) {
            /* 出块：恢复全局的左右轮缩放 */
            if (s_turn_scale_save_r != 0xFFFFu) {
                host_param_set_local(HP_GYRO_RIGHT_SCALE, (float)s_turn_scale_save_r);
                host_param_set_local(HP_GYRO_LEFT_SCALE,  (float)s_turn_scale_save_l);
                s_turn_scale_save_r = 0xFFFFu;
            }
            return 1;
        }
        if (b->p0 != 0u && (s_now - s_t0_block) >= (uint32_t)b->p0) {
            s_flags |= TASK_RUN_F_TIMEOUT;
            if (s_turn_scale_save_r != 0xFFFFu) {   /* 超时出块也要恢复 */
                host_param_set_local(HP_GYRO_RIGHT_SCALE, (float)s_turn_scale_save_r);
                host_param_set_local(HP_GYRO_LEFT_SCALE,  (float)s_turn_scale_save_l);
                s_turn_scale_save_r = 0xFFFFu;
            }
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
        uint8_t  hold_n = ((b->flags & TASK_WAIT_COND_JUMP) != 0u)
                              ? 0u
                              : (uint8_t)(b->flags & TASK_WAIT_COND_HOLD_MSK);
        uint8_t  ok;
        /* 审计 A-02：只有"不要求持续"时才允许 ch_black 的锁存（见 s_cond_latch_ok） */
        s_cond_latch_ok = (hold_n == 0u) ? 1u : 0u;
        ok = (uint8_t)cond_ok(b->p0, b->p1);
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
        /* ★2026-10-24 审计 F-05：原来只清 tracing 那边的标志，**没清 s_cross_n**
         * （条件 5「累计 N 次」用的就是它）=> "清计数 -> 累计 N 次" 永远提前满足。
         * 现在两个一起清，语义才是用户以为的"从零开始数"。 */
        tracing_cross_clear();
        s_cross_n = 0u;
        return 1;

    case TASK_OP_CAL_SET:                        /* ★切灰度标定套：0 = 普通 / 1 = 亚克力 */
        trace_cal_select_set((uint8_t)(b->p0 & 1));
        return 1;

    case TASK_OP_SET_STAGE:                      /* ★段号 = p0（旧代码的 `black_num=M`）*/
        s_stage = (uint8_t)(b->p0 & 0xFFu);
        return 1;

    case TASK_OP_IF_STAGE: {
        /* ★主页的分派判断（旧代码 `if (8路全黑 && black_num==N) {...} else if ...`）：
         *   **这是第几条判断** = 块在主页循环体里的下标（`s_idx`）；**现在是第几次压线** =
         *   `s_dispatch_n`（每成功分派一次 +1）—— 两者相等才有资格成立。
         *   ⇒ 用户**不用填任何序号**：只填"哪几路压黑 + 跳到哪"。
         *   不成立就**往下走一块**，这就是"看一眼就走"，也是主页能并排写 N 条的原因
         *   （0x06「等条件」会停在那里等，写不了分派）。
         *   判据复用 ch_black()，与条件 10 是同一份；掩码 0 当不成立（空真陷阱）；
         *   目标状态号越界也当不成立（绝不越界跳）。
         *
         * ★★`p0` = **满足时间（毫秒）**（用户 2026-10-10："加上滤波，就是满足时间，默认 100ms"）。
         *   为什么需要它：探头排是横着装，**十字路口的横向黑线与探头排平行 ⇒ 8 路也会同时压黑**，
         *   与"骑上障碍的黑砂面"是同一个信号；路口那一下很短（3.8cm 线 @1m/s ≈ 38ms），
         *   而障碍前的黑带面积大得多 ⇒ 要求"连续成立 ≥ p0 毫秒"就能把路口滤掉。
         *   · `p0 = 0` ⇒ 不滤波（本拍成立即分派）—— **与加这个功能之前逐字节等价**（老任务 p0 全是 0）。
         *   · 中途断一次就**重新计时**（与 0x06「需持续」同一口径）。
         *   · 计时用 s_now（毫秒），不是"被走了多少次"—— GOTO 自环让一拍里 body 会被走好几遍
         *     （TICK_MAX_BLOCKS），按次数计会快好几倍。
         *   · 时间没到之前本块仍然"往下走"，所以主页不会因此卡住。 */
        uint16_t m    = (uint16_t)b->p1 & 0xFFu;
        uint32_t need = (uint32_t)b->p0;         /* 满足时间 ms（0 = 不滤波） */
        uint8_t  i;

        if (s_phase != 1u || s_idx != (uint16_t)s_dispatch_n) {
            return 1;                            /* 还没轮到这一条（或不在循环体里）⇒ 往下走 */
        }
        if (m == 0u) {
            s_if_run = 0u;
            return 1;
        }
        for (i = 0u; i < 8u; i++) {
            if ((m & (uint16_t)(1u << i)) != 0u && ch_black(i) == 0u) {
                s_if_run = 0u;                   /* 断了 ⇒ 这一轮作废，下次重记起点 */
                return 1;                        /* 勾中的某一路没压黑 ⇒ 这条不成立，往下走 */
            }
        }
        /* 条件成立 */
        if (need != 0u) {
            if (s_if_run == 0u || s_if_dn != s_dispatch_n) {
                s_if_run  = 1u;
                s_if_from = s_now;               /* 这一拍刚开始连续成立 */
                s_if_dn   = (uint8_t)s_dispatch_n;
            }
            if ((uint32_t)(s_now - s_if_from) < need) {
                return 1;                        /* 还没持续够 ⇒ 这一拍先不分派，往下走 */
            }
            s_if_run = 0u;                       /* 分派了：下一条判断重新计时 */
        }
        if (b->p2 < s_n_states) {
            if (s_dispatch_n < 0xFFu) {
                s_dispatch_n++;                  /* ★这一条用掉了 ⇒ 下一次轮到下一条 */
            }
            seq_begin((uint8_t)b->p2);
            return 2;
        }
        return 1;                                /* 目标越界：当不满足，绝不越界跳 */
    }

    case TASK_OP_CLIMB: {
        /* ★「爬台阶/爬坡：循环到车平」（用户 m 2026-10-09）—— 详见 task_prog.h 的注释。
         *   本块每被走到一次 = 刚做完一轮动作 ⇒ 记一节；**跑够节数、而且车头真的落回水平**
         *   才往下走（后面一般接「切换模式：停车」）；否则跳回 p3（一般填本状态自己）再来一遍。 */
        float tol  = (b->p2 == 0u) ? 3.0f : (float)b->p2 / 10.0f;
        float lift = (float)b->p1 / 10.0f;

        /* 会话由 seq_begin() 在**进状态那一刻**开好（见那里的注释）；这里只留兜底：
         * 万一没有（程序被改过、段号对不上），就地开一个，别让这块变成空操作。 */
        if (s_climb_on == 0u || s_climb_stage != s_stage) {
            s_climb_base    = Pitch;
            s_climb_left    = (uint8_t)((b->p0 > 0u) ? (b->p0 & 0xFFu) : 1u);
            s_climb_lifted  = 0u;
            s_climb_lift_at = (lift > 0.0f) ? (s_climb_base + lift) : 0.0f;
            s_climb_on      = 1u;
            s_climb_stage   = s_stage;
        }

        if (s_climb_left > 0u) {
            s_climb_left--;                      /* 这一轮算爬了一节 */
        }
        if (s_climb_lift_at > 0.0f && Pitch >= s_climb_lift_at) {
            s_climb_lifted = 1u;                 /* 兜底：每拍那一眼在 TaskRun_Tick 里 */
        }

        if (s_climb_left == 0u
            && (s_climb_lift_at <= 0.0f || s_climb_lifted != 0u)
            && fabsf(Pitch - s_climb_base) <= tol) {
            s_climb_on = 0u;                     /* 爬上去了：会话结束，往下走 */
            return 1;
        }

        if (b->p3 < s_n_states) {                /* 还没到位：回本状态，把动作再来一遍 */
            seq_begin((uint8_t)b->p3);
            return 2;
        }
        return 1;                                /* 目标越界：当"通过"，绝不越界跳 */
    }

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
    /* 老入口：等价于"从第 1 次分派开始"。**车上中键、老上位机都走这里** ⇒ 必须是老行为。 */
    return TaskRun_StartFrom(id, 1u);
}

int TaskRun_StartFrom(uint16_t id, uint8_t from_dispatch)
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
    /* 审计 F-02（P0）：锁存必须每进一块/每次起跑都清，
     * 否则同掩码的下一块/重跑会继承上一块的锁存 => 误判「扫到了」。 */
    s_bl_latch  = 0u;
    s_bl_last_m = 0xFFFFu;
    s_hold_from = 0u;
    s_if_run    = 0u;                            /* ★「如果…就跳」的满足时间计时：同样从零开始 */
    s_if_from   = 0u;
    s_if_dn     = 0xFFu;
    s_stage     = 0u;                            /* ★段号清 0：主页从"第 0 段"开始分派 */
    /* ★2026-10-11（用户："我输 2，第一次判断就不按第一个了，直接从第二个开始"）：
     *   主页那串「如果…就跳」是按"第几次压线"轮着来的（见 IF_STAGE 那段注释：第 k 条判断要
     *   `s_idx == s_dispatch_n` 才有资格成立）。所以"从第 k 个障碍开始测"就是把计数摆到 k-1。
     *   `from_dispatch` 1 起算：1（或 0，老上位机不发这字节）⇒ 与以前逐字节等价。
     *   ★只影响这一次运行：不写 NVRAM、不落全局 —— 车上中键启动走 TaskRun_Start() ⇒ 永远第 1 次。*/
    s_dispatch_n    = (from_dispatch > 1u) ? (uint8_t)(from_dispatch - 1u) : 0u;
    s_climb_on      = 0u;                        /* ★爬台阶会话：新任务从零开始 */
    s_climb_stage   = 0u;
    s_climb_left    = 0u;
    s_climb_lifted  = 0u;
    s_climb_base    = 0.0f;
    s_climb_lift_at = 0.0f;

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
    /* ★爬台阶会话：每拍盯一眼俯仰角。抬头可能发生在两块之间（甚至一拍之内），
     *   只在"看一步"那块被执行的时刻检查会漏判 —— 这里补上每拍那一眼。 */
    if (s_climb_on != 0u && s_climb_lift_at > 0.0f && Pitch >= s_climb_lift_at) {
        s_climb_lifted = 1u;
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
            /* ★换块了：「需持续」的连续计时必须重新开始 */
            s_hold_run = 0u;
            /* 审计 F-02（P0）：锁存也必须每进一块/每次起跑都清，
             * 否则同掩码的下一块 / 重跑会继承上一块的锁存 => 误判「扫到了」。 */
            s_bl_latch  = 0u;
            s_bl_last_m = 0xFFFFu;
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
uint8_t  TaskRun_Stage(void)       { return s_stage; }
uint8_t  TaskRun_Loaded(void)      { return s_loaded; }
uint16_t TaskRun_ProgLen(void)     { return s_len; }
uint16_t TaskRun_CrossCount(void)  { return s_cross_n; }
uint32_t TaskRun_DistMm(void)      { return (uint32_t)s_dist_mm; }
uint8_t  TaskRun_Done(void)        { return s_done; }
