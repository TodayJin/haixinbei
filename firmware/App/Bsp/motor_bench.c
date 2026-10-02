/**
 * @file  motor_bench.c
 * @brief 电机 + 编码器台架自测：上电自动跑一遍，结果全走 SEGGER RTT 通道 0
 *
 * 只在 `make MOTOR_BENCH=1`（-> build-bench/haixin-bench.*）里编进去；
 * 正式构建没定义 -DMOTOR_BENCH，整个文件是空的，不占 flash。
 *
 * 为什么必须插在 main_app.c 的 Encoder_Init() 之后、Motion_Init() 之前：
 *   TIM6 每 5ms 触发 Motion_Task() -> SpeedControl_Task()，而它无条件调用
 *   Encoder_ReadDelta()，那函数读完就把 CNT 清零。台架要是在周期任务已经
 *   跑起来之后才动编码器，计数会被它偷走，看到的数只有真实的一半甚至更少。
 *   所以在周期任务起来之前把台架跑完，是这里唯一正确的插入点。
 *
 * 判定标尺（电机 JGB37-3865-520：11PPR 霍尔 AB 相、45:1 减速）：
 *   11 * 4（双边沿 4 倍频）* 45 = 1980 计数 / 输出轴圈（= HP_COUNTS_PER_REV）。
 *   12V 空载 150 rpm -> 150/60 * 1980 = 4950 计数/秒。
 *   所以 2500/7200 占空比跑 400ms，应该拿到几百个计数；拿到 0 就是没动。
 *
 * 编码器极性约定（见 encoder.h）：计数正 = 车前进方向。
 *   于是 Load(+,+) 那一步左右两侧的计数应当同号且都为正；
 *   出现一正一负，说明有一侧的电机线序或编码器 A/B 相接反了，必须报出来。
 *
 * 末尾会把前面所有行回放一遍：台架在 RTT 记录器挂上之前就开跑了，
 * 最前面几行在 NO_BLOCK_SKIP 模式下会被丢掉，回放保证一条不漏。
 */
/* ---- 台架模式选择（改这一行 + `make MOTOR_BENCH=1 flash` 即可切换）----
 *   0 = 完整台架：分级正转/反转/单轮/速度环闭环+目标阶跃，自动跑完约 40s，出量化数据
 *   1 = 转向目视 + 起步脉宽：慢爬坡找到起转阈值，正反各保持 2.5s，循环
 *   2 = 手推方向标定：电机全程不上电，只让轮子被推动，看两路编码器各自的符号
 * 为什么要人参与：编码器只知道"转了多少、往哪个符号走"，它不知道车轮相对
 * 车头是往前还是往后 —— 这一条物理上只有眼睛/手能定（规格书 §3.1 也这么写）。
 * 注意：这里必须用 `#if BENCH_MODE == n` 而不是 `#ifdef` —— 定义成 0 也成立。 */
#define BENCH_MODE      0

#ifdef MOTOR_BENCH

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "motor_bench.h"
#include "motor.h"
#include "encoder.h"
#include "speed_control.h"
#include "host_link.h"      /* host_param_set_local / HP_SPEED_* */
#include "bsp_board.h"      /* MOTOR_PWM_MAX */
#include "board.h"          /* INA226 */
#include "SEGGER_RTT.h"

#define BENCH_MAX    64
#define BENCH_LINE   240

static char    s_log[BENCH_MAX][BENCH_LINE];
static uint8_t s_log_n;

/* ---- 输出：实时打一条 + 存一份给末尾回放 ---- */
static void bench_emit(const char *fmt, ...)
{
    char line[BENCH_LINE];
    va_list ap;
    int i;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    SEGGER_RTT_WriteString(0, line);
    SEGGER_RTT_WriteString(0, "\r\n");

    if (s_log_n < BENCH_MAX) {
        for (i = 0; i < BENCH_LINE - 1 && line[i] != '\0'; i++) {
            s_log[s_log_n][i] = line[i];
        }
        s_log[s_log_n][i] = '\0';
        s_log_n++;
    }
}

#if (BENCH_MODE == 0)
/* 统计：拿不到计数的步数、左右不同号的步数 */
static uint8_t s_zero_steps;
static uint8_t s_mismatch_steps;

/* ---- 单步：给一段 PWM，量这段时间里两侧编码器的累计增量 ----
 * 注意 Load(0,0) 是真停：Limit() 夹的是 [-7200, 7200]，0 原样保留。
 *
 * 除了增量，还顺手抓两样东西，用来分辨「电机没转」和「电机转了但计数没进去」：
 *   ① PB4..PB7 的电平跳变次数 —— 这是编码器脉冲到底有没有到 MCU 引脚；
 *   ② 步中途的主电源电流 —— 空转 / 堵转 / 完全没通电，电流完全不同。
 * 采样用死循环连续读、不带延时：满速时单相脉冲约 1.2kHz，加 1ms 延时
 * 会漏掉一大半边沿，统计就没意义了。 */
static void bench_drive(const char *tag, int l, int r, int ms)
{
    int dl = 0, dr = 0;
    uint32_t t0, half;
    int32_t ma = -1;
    uint32_t el = 0, er = 0;
    uint8_t last_l, last_r;

    Load(0, 0);
    HAL_Delay(120);                 /* 先停稳再清零，别把上一步的余转算进来 */
    Encoder_Reset();

    /* GPIOB->IDR 第 4/5 位 = PB4/PB5 = 编码器A(左) 的 A/B 相；第 6/7 位 = 编码器B(右) */
    last_l = (uint8_t)((GPIOB->IDR >> 4) & 0x03U);
    last_r = (uint8_t)((GPIOB->IDR >> 6) & 0x03U);

    Load(l, r);
    t0   = HAL_GetTick();
    half = (uint32_t)ms / 2U;

    while ((int32_t)(HAL_GetTick() - t0) < ms) {
        uint8_t nl = (uint8_t)((GPIOB->IDR >> 4) & 0x03U);
        uint8_t nr = (uint8_t)((GPIOB->IDR >> 6) & 0x03U);

        if (nl != last_l) { el++; last_l = nl; }
        if (nr != last_r) { er++; last_r = nr; }

        if (ma < 0 && (int32_t)(HAL_GetTick() - t0) >= (int32_t)half) {
            if (ina226_read_current_ma(&ma) != HAL_OK) ma = -100000;
        }
    }

    Encoder_ReadDelta(&dl, &dr);
    Load(0, 0);                     /* 量完立刻停 */

    if (dl == 0 && dr == 0) {
        s_zero_steps++;
        bench_emit("STEP %-11s pwm %6d,%6d %4dms | L %7d R %7d | 边沿 %6lu %6lu | %6ldmA | 无计数",
                   tag, l, r, ms, dl, dr, (unsigned long)el, (unsigned long)er, (long)ma);
    } else if (((dl > 0) != (dr > 0)) && (l != 0) && (r != 0)) {
        s_mismatch_steps++;
        bench_emit("STEP %-11s pwm %6d,%6d %4dms | L %7d R %7d | 边沿 %6lu %6lu | %6ldmA | 不同号!",
                   tag, l, r, ms, dl, dr, (unsigned long)el, (unsigned long)er, (long)ma);
    } else {
        bench_emit("STEP %-11s pwm %6d,%6d %4dms | L %7d R %7d | 边沿 %6lu %6lu | %6ldmA",
                   tag, l, r, ms, dl, dr, (unsigned long)el, (unsigned long)er, (long)ma);
    }

    HAL_Delay(220);                 /* 步间留白：减少正反转对驱动器的冲击 */
}

/* ---- 开环静特性：恒定 PWM 下稳态转速是多少 ----
 * 这才是前馈要拟合的那张表。bench_drive 的 400ms 短脉冲只能回答"能不能起步"，
 * 加速过程会把转速算低；这里跑够长、只看后半段，得到 pwm -> 计数/秒 的真值。
 * st = 后半段里增量为 0 的拍数：这个数大就说明该 PWM 落在静摩擦区里（粘滑）。 */
static void bench_open(int pwm, int ms)
{
    int64_t sum_l = 0, sum_r = 0;
    int     n = 0, stl = 0, str = 0;
    uint32_t t0, next;
    int dl, dr;

    SpeedControl_Stop();            /* 确保速度环不插手 */
    Load(0, 0);
    HAL_Delay(150);                 /* 先停稳再清零 */
    Encoder_Reset();

    Load(pwm, pwm);
    t0 = next = HAL_GetTick();
    while ((int32_t)(HAL_GetTick() - t0) < ms) {
        if ((int32_t)(HAL_GetTick() - next) >= 0) {
            next += 5U;
            Encoder_ReadDelta(&dl, &dr);
            if ((int32_t)(HAL_GetTick() - t0) >= ms / 2) {
                sum_l += dl;  sum_r += dr;  n++;
                if (dl == 0) stl++;
                if (dr == 0) str++;
            }
        }
    }
    Load(0, 0);

    if (n > 0) {
        bench_emit("OPEN pwm %5d | L m %6ld st %3d | R m %6ld st %3d | n=%d",
                   pwm, (long)(sum_l * 200 / n), stl,
                   (long)(sum_r * 200 / n), str, n);
    }
    HAL_Delay(250);
}


/* 本轮窗口用的前馈参数，只为了打在同一行里方便对号入座 */
static float s_ff_lbl_s, s_ff_lbl_f, s_ff_lbl_ki;
static int   s_ff_lbl_kp, s_ff_lbl_il;      /* STAT 行标签：本窗口用的是哪组前馈 + 增益 */

/* ---- 速度环闭环：人工按 5ms 喂 SpeedControl_Task()，量实际转速对不对 ----
 * 期望计数/秒 = 目标( PWM 刻度) * (SPEED_FULL_COUNTS / 7200) * 200
 *             = 目标 * 0.6944   （SPEED_FULL_COUNTS = 25，每 5ms 一次 -> 200 次/秒） */
static void bench_cl_window(int l, int r, int ms)
{
    uint32_t t0, next, seg_t0;
    long seg_l = 0, seg_r = 0;
    long b_l[6], b_r[6];
    int  nb = 0, i, expected;

    /* 稳态统计：只看后半段（前 1000ms 是爬坡/收敛过程，不参与） */
    int64_t sum_l = 0, sum_r = 0;
    int  mn_l = 0, mx_l = 0, mn_r = 0, mx_r = 0;
    int  rev_l = 0, rev_r = 0, n_st = 0;
    int  stl_l = 0, stl_r = 0;
    int  om_l = 0, ox_l = 0, om_r = 0, ox_r = 0;
    int64_t osum_l = 0, osum_r = 0;     /* 输出均值：用来验证前馈是不是直接落在了工作点上 */
    uint32_t edge_l = 0, edge_r = 0;    /* PB4/5、PB6/7 的相位跳变数：分辨「轮子没转」和「编码器没计数」 */
    uint8_t  last_el, last_er;

    for (i = 0; i < 6; i++) { b_l[i] = 0; b_r[i] = 0; }

    SpeedControl_SetTarget(l, r);

    last_el = (uint8_t)((GPIOB->IDR >> 4) & 0x03U);
    last_er = (uint8_t)((GPIOB->IDR >> 6) & 0x03U);

    t0 = next = seg_t0 = HAL_GetTick();
    while ((int32_t)(HAL_GetTick() - t0) < ms) {
        uint32_t now = HAL_GetTick();
        /* 每一轮（约 1ms）都采一次电平，跟 5ms 的计数无关：
         * 计数为 0 但这里跳变很多 -> 是编码器没进计数器，不是轮子没转。 */
        {   uint8_t nl = (uint8_t)((GPIOB->IDR >> 4) & 0x03U);
            uint8_t nr = (uint8_t)((GPIOB->IDR >> 6) & 0x03U);
            if (nl != last_el) { edge_l++; last_el = nl; }
            if (nr != last_er) { edge_r++; last_er = nr; } }
        if ((int32_t)(now - next) >= 0) {
            int cl, cr, ol, orr;
            next += 5U;
            SpeedControl_Task();
            cl = SpeedControl_Counts(0);
            cr = SpeedControl_Counts(1);
            ol = SpeedControl_Output(0);
            orr = SpeedControl_Output(1);
            seg_l += (long)cl;  seg_r += (long)cr;
            /* 每 500ms 切一段，看出"有没有收敛、稳态误差多大" */
            if ((int32_t)(now - seg_t0) >= 500 && nb < 6) {
                long span = (long)(now - seg_t0);
                b_l[nb] = seg_l * 1000L / span;
                b_r[nb] = seg_r * 1000L / span;
                nb++;
                seg_l = 0; seg_r = 0;
                seg_t0 = now;
            }
            /* 后半段：累计平均 / 本拍增量的峰峰 / 反号拍数 / 输出峰峰 */
            if ((int32_t)(now - t0) >= 1000) {
                sum_l += (int64_t)cl;  sum_r += (int64_t)cr;
                if (n_st == 0) {
                    mn_l = mx_l = cl;      mn_r = mx_r = cr;
                    om_l = ox_l = ol;      om_r = ox_r = orr;
                } else {
                    if (cl  < mn_l) mn_l = cl;
                    if (cl  > mx_l) mx_l = cl;
                    if (cr  < mn_r) mn_r = cr;
                    if (cr  > mx_r) mx_r = cr;
                    if (ol  < om_l) om_l = ol;
                    if (ol  > ox_l) ox_l = ol;
                    if (orr < om_r) om_r = orr;
                    if (orr > ox_r) ox_r = orr;
                }
                if ((long)cl * (long)l < 0) rev_l++;
                if ((long)cr * (long)r < 0) rev_r++;
                if (cl == 0) stl_l++;
                if (cr == 0) stl_r++;
                osum_l += ol;  osum_r += orr;
                n_st++;
            }
        }
        HAL_Delay(1);
    }

    /* 收尾：while 的退出条件会吃掉最后不足 500ms 的那一段（上一版第 4 列恒为 0 就是这原因） */
    if (nb < 6) {
        uint32_t span = HAL_GetTick() - seg_t0;
        if (span >= 50U) {
            b_l[nb] = seg_l * 1000L / (long)span;
            b_r[nb] = seg_r * 1000L / (long)span;
            nb++;
        }
    }

    expected = (int)(((long)l) * 25L * 200L / 7200L);

    /* 分段列从左到右就是时间先后；靠右的列 ≈ 稳态 */
    bench_emit("CLOSE tgt %5d,%5d out %6d,%6d exp %5d | seg500 L %5ld %5ld %5ld %5ld %5ld %5ld R %5ld %5ld %5ld %5ld %5ld %5ld",
               l, r,
               SpeedControl_Output(0), SpeedControl_Output(1), expected,
               b_l[0], b_l[1], b_l[2], b_l[3], b_l[4], b_l[5],
               b_r[0], b_r[1], b_r[2], b_r[3], b_r[4], b_r[5]);

    /* 稳态三件套（都是整数，不用 libm/浮点打印）：
     *   mean 平均计数/秒
     *   p2p  本拍增量(5ms)的峰峰，换算成计数/秒 —— 粘滑/抖动会把它顶得很高
     *   rev  增量与目标反号的拍数 —— 粘滑极限环里轮子真在倒转，这里必然 > 0
     *   outp 控制输出的峰峰 —— 看控制器自己是不是在来回抽
     * 目标 1000 附近如果 mean 乱跳、rev 一堆，就是静摩擦粘滑；平滑改善了没有，看这三列。 */
    if (n_st > 0) {
        /* 前馈参数用定点打（本工程的 printf 没开浮点）；
         * o = 后半段实际输出的平均值，跟 ff 列一对比就知道前馈偏了多少。
         * rev（增量与目标反号的拍数）在这一版实测里恒为 0，已经从行里省掉印位。 */
        int ffs = (int)(s_ff_lbl_s * 100.0f + 0.5f);
        int fff = (int)(s_ff_lbl_f + 0.5f);
        int kk  = (int)(s_ff_lbl_ki * 100.0f + 0.5f);
        (void)rev_l; (void)rev_r;      /* 仍然统计，只是实测恒 0，不再占打印位 */
        bench_emit("STAT tgt %4d ff %d.%02d/%4d kp %2d ki %d.%02d/%4d | L m %6ld p2p %6ld st %3d o %6ld e %6lu | R m %6ld p2p %6ld st %3d o %6ld e %6lu | outp %5d,%5d n=%d",
                   l, ffs / 100, (ffs < 0 ? -ffs : ffs) % 100, fff, s_ff_lbl_kp,
                   kk / 100, (kk < 0 ? -kk : kk) % 100, s_ff_lbl_il,
                   (long)(sum_l * 200 / n_st), (long)(mx_l - mn_l) * 200L, stl_l, (long)(osum_l / n_st), (unsigned long)edge_l,
                   (long)(sum_r * 200 / n_st), (long)(mx_r - mn_r) * 200L, stl_r, (long)(osum_r / n_st), (unsigned long)edge_r,
                   ox_l - om_l, ox_r - om_r, n_st);
    }
}

/* ---- 扫表用：**先彻底复位环路状态**再跑一个窗口 ----
 * 这一条是必须的：改前馈而不清积分，等于把上一窗口为旧前馈攒下的积分
 * 直接叠到新前馈上（实测能把输出顶到 7200 饱和，然后整窗都在退积分），
 * 看到的数据全是假的。Stop() 会清目标/积分/滑动窗并 Load(0,0)，
 * Enable() 的 0->1 边沿再清一次，于是每个窗口都从"静止 + 零积分"起步。 */
static void bench_cl_tune(int tgt, float slope, float fric,
                          int kp, float ki, float ilim, int ms)
{
    SpeedControl_Stop();
    host_param_set_local(HP_SPEED_KP, (float)kp);
    host_param_set_local(HP_SPEED_KI, ki);
    host_param_set_local(HP_SPEED_ILIMIT, ilim);
    SpeedControl_SetFeedforward(slope, fric);
    s_ff_lbl_s  = slope;
    s_ff_lbl_f  = fric;
    s_ff_lbl_kp = kp;
    s_ff_lbl_ki = ki;
    s_ff_lbl_il = (int)ilim;
    SpeedControl_SetTarget(tgt, tgt);
    SpeedControl_Enable(1U);
    bench_cl_window(tgt, tgt, ms);
}

/* ---- 验收用：拿**出厂默认前馈**跑一个窗口 ----
 * 与 bench_cl_tune 只差一件事：不调用 SpeedControl_SetFeedforward，用的是
 * speed_control.c 里 SPEED_FF_SLOPE / SPEED_FF_FRIC 的编译期默认值，再从
 * GetFeedforward 读回来打在标签上。这样验收数字证明的就是实际会出厂的配置，
 * 不会出现"台架上扫出来的常数和发货代码里的常数写岔了"。 */
static void bench_cl_default(int tgt, int ms)
{
    float s = 0.0f, f = 0.0f;

    SpeedControl_Stop();
    host_param_set_local(HP_SPEED_KP, 20.0f);
    host_param_set_local(HP_SPEED_KI, 2.0f);
    host_param_set_local(HP_SPEED_ILIMIT, 3000.0f);
    SpeedControl_GetFeedforward(&s, &f);
    s_ff_lbl_s  = s;
    s_ff_lbl_f  = f;
    s_ff_lbl_kp = 20;
    s_ff_lbl_ki = 2.0f;
    s_ff_lbl_il = 3000;
    SpeedControl_SetTarget(tgt, tgt);
    SpeedControl_Enable(1U);
    bench_cl_window(tgt, tgt, ms);
}

/* ---- 目标阶跃响应：先稳在 a，再一步跳到 b ----
 * 每 20ms 出一个速度样本（4 拍 5ms 计数求和 ×50 = 计数/秒）。采样窗口对两种构建
 * 完全一致，所以"加了 20ms 平滑之后是不是变迟钝了"可以直接比。
 * 只看左轮：两轮驱动信号相同，够用了。 */
static void bench_cl_step(int a, int b)
{
    int  sb[96], n = 0, i, peak = 0, rise = -1;
    long blk = 0, tail = 0;
    uint32_t t0, next, blk0;

    /* 1) 先稳到 a */
    SpeedControl_SetTarget(a, a);
    t0 = next = HAL_GetTick();
    while ((int32_t)(HAL_GetTick() - t0) < 1500) {
        uint32_t now = HAL_GetTick();
        if ((int32_t)(now - next) >= 0) { next += 5U; SpeedControl_Task(); }
        HAL_Delay(1);
    }

    /* 2) 跳到 b，采 1600ms */
    SpeedControl_SetTarget(b, b);
    blk = 0; n = 0;
    t0 = next = blk0 = HAL_GetTick();
    while ((int32_t)(HAL_GetTick() - t0) < 1600 && n < 96) {
        uint32_t now = HAL_GetTick();
        if ((int32_t)(now - next) >= 0) {
            next += 5U;
            SpeedControl_Task();
            blk += (long)SpeedControl_Counts(0);
            if ((int32_t)(now - blk0) >= 20) {
                sb[n++] = (int)(blk * 50L);     /* 20ms 窗口 -> 计数/秒 */
                blk = 0;
                blk0 = now;
            }
        }
        HAL_Delay(1);
    }

    /* 3) 末值 = 最后 5 个样本的均值；上升时间 = 首次达到末值 90% 的采样点 */
    { int m = (n >= 5) ? 5 : n;
      for (i = n - m; i < n; i++) tail += sb[i];
      if (m > 0) tail /= m; }
    for (i = 0; i < n; i++) {
        if (rise < 0 && tail > 0 && (long)sb[i] * 10L >= tail * 9L) rise = i;
        if (sb[i] > peak) peak = sb[i];
    }

    bench_emit("STEPR tgt %5d->%5d | 末值 L %6ld | rise90 %5dms | peak %6ld (+%ld.%ld%%)",
               a, b, (long)tail,
               (rise < 0) ? -1 : rise * 20 + 20,
               (long)peak,
               tail > 0 ? (long)(peak - tail) * 10L / tail : 0L,
               tail > 0 ? ((long)(peak - tail) * 100L / tail) % 10L : 0L);
    if (n >= 10) {
        bench_emit("      前 10 个 20ms 速度样本: %d %d %d %d %d %d %d %d %d %d",
                   sb[0], sb[1], sb[2], sb[3], sb[4],
                   sb[5], sb[6], sb[7], sb[8], sb[9]);
    }
}
#endif /* BENCH_MODE == 0 */

#if (BENCH_MODE == 1)
/* ===================================================================
 *  目视转向测试 + 起步脉宽标定
 *  台架实测：PWM 3200 时两轮【完全不动】（边沿计数 = 0），5000 才起。
 *  所以本模式改做三件事：
 *    ① 慢爬坡 0 -> DIR_PWM，记录两轮"第一次稳定出计数"的脉宽 = 起步阈值
 *    ② 保持 DIR_PWM 正转，量正转速度
 *    ③ 停稳后 -DIR_PWM 反转，量反转速度
 *  无限循环，让人看清方向，同时把起步阈值喂给标定。
 * =================================================================== */
#define DIR_PWM        6000
#define DIR_RAMP_MS    3000            /* 慢到能看清方向：2.5s 爬到满 */
#define DIR_SOFT_MS     400            /* 正/反转起转都先软启动 */
#define DIR_HOLD_MS    2500
#define DIR_GAP_MS     1800

/* 慢爬坡：返回两轮各自的起步脉宽（起不来就是 -1） */
static void dir_ramp(int *th_l, int *th_r)
{
    int t, dl, dr;
    int32_t cum_l = 0, cum_r = 0;

    *th_l = -1;
    *th_r = -1;

    Load(0, 0);
    HAL_Delay(200);
    Encoder_Reset();

    for (t = 0; t <= DIR_RAMP_MS; t += 10) {
        int p = DIR_PWM * t / DIR_RAMP_MS;
        Load(p, p);
        HAL_Delay(10);
        Encoder_ReadDelta(&dl, &dr);
        cum_l += dl;                    /* ReadDelta 会把 CNT 清零，所以自己累加 */
        cum_r += dr;
        /* 阈值取"累计 5 个计数"而不是 1：低速时相位抖动会蹦出 ±1 的假计数 */
        if (*th_l < 0 && (cum_l >= 5 || cum_l <= -5)) *th_l = p;
        if (*th_r < 0 && (cum_r >= 5 || cum_r <= -5)) *th_r = p;
    }
}

/* 软启动到 (l,r) -> 保持 hold_ms -> 停。返回中段电流与两轮净计数 */
static int32_t dir_hold(int l, int r, int hold_ms, int32_t *cnt_l, int32_t *cnt_r)
{
    int t = 0, dl, dr;
    int32_t al = 0, ar = 0, ma = 0;

    for (t = 0; t < DIR_SOFT_MS; t += 10) {     /* 软启动，别硬砸齿轮箱 */
        Load(l * t / DIR_SOFT_MS, r * t / DIR_SOFT_MS);
        HAL_Delay(10);
    }
    Load(l, r);
    Encoder_Reset();

    t = 0;
    while (t < hold_ms) {
        HAL_Delay(10);
        t += 10;
        Encoder_ReadDelta(&dl, &dr);
        al += dl; ar += dr;
        if (t == hold_ms / 2) {                 /* 转的中途采一次主电源电流 */
            if (ina226_read_current_ma(&ma) != HAL_OK) ma = -100000;
        }
    }
    Load(0, 0);

    *cnt_l = al;
    *cnt_r = ar;
    return ma;
}

static void bench_dirtest(void)
{
    int th_l, th_r, cyc;
    int32_t mv = 0, ma0 = 0;
    int32_t fl, fr, rl, rr, maf, mar;

    if (ina226_read_bus_voltage_mv(&mv) != HAL_OK) mv = -1;
    if (ina226_read_current_ma(&ma0) != HAL_OK)    ma0 = -100000;

    bench_emit("");
    bench_emit("============ 转向目视测试 + 起步脉宽 ============");
    bench_emit("[PWR] 静止 vbus %ld mV, 电流 %ld mA%s",
               (long)mv, (long)ma0,
               (mv >= 0 && mv < 6000) ? "   !! 12V 没上电或太低 -> 电机不会动" : "");
    bench_emit("固件把 +PWM 当【前进】。每轮：慢爬坡 %dms 到 +%d -> 保持 %dms -> 停 -> 反向 -%d 保持 %dms",
               DIR_RAMP_MS, DIR_PWM, DIR_HOLD_MS, DIR_PWM, DIR_HOLD_MS);
    bench_emit("请看：① 轮子朝【车头】还是【车尾】转  ② 左右两轮是不是同一个方向");
    bench_emit("看完回答后主机会立刻停下来、重烧正式固件。");
    bench_emit("");

    for (cyc = 1; ; cyc++) {
        Load(0, 0);
        HAL_Delay(DIR_GAP_MS);

        dir_ramp(&th_l, &th_r);
        HAL_Delay(150);
        maf = dir_hold(DIR_PWM, DIR_PWM, DIR_HOLD_MS, &fl, &fr);

        Load(0, 0);
        HAL_Delay(1500);                        /* 停稳，别让惯性混进反转数 */
        mar = dir_hold(-DIR_PWM, -DIR_PWM, DIR_HOLD_MS, &rl, &rr);

        bench_emit("[第%d轮] 起步脉宽 左 %5d 右 %5d | 转时电流 +%ld / -%ld mA",
                   cyc, th_l, th_r, (long)maf, (long)mar);
        bench_emit("           +%d 计数/秒: 左 %6ld 右 %6ld   |   -%d 计数/秒: 左 %6ld 右 %6ld",
                   DIR_PWM, (long)(fl * 1000 / DIR_HOLD_MS), (long)(fr * 1000 / DIR_HOLD_MS),
                   DIR_PWM, (long)(rl * 1000 / DIR_HOLD_MS), (long)(rr * 1000 / DIR_HOLD_MS));
    }
}

void MotorBench_Run(void)
{
    bench_dirtest();                /* 永不返回：正式流程要等重烧才恢复 */
}

#elif (BENCH_MODE == 2)
/* ===================================================================
 *  手推方向标定（零风险：全程不给电机上电）
 *  台架已知：+PWM 时两路编码器读数【都是负】，但两轮物理转向【相反】。
 *  这只有在"两条编码器的正方向约定互不相同"时才可能。缺的最后一块拼图是
 *  "车轮朝车头滚时，编码器算正还是算负" —— 编码器自己不知道车头在哪，
 *  所以只能让轮子朝前进方向滚一次，看符号。
 *
 *  用法：车在地上就直接往前推 10cm；架空的就用手把两轮朝车头方向转几圈。
 *  只想动一个轮也行。转完不用管，日志会一路累计下去。
 * =================================================================== */
static void bench_spintest(void)
{
    int dl, dr;
    uint32_t t0;
    int32_t acc_l = 0, acc_r = 0;
    int32_t mv = 0;

    Load(0, 0);                     /* 电机全程不上电 */
    Encoder_Reset();

    if (ina226_read_bus_voltage_mv(&mv) != HAL_OK) mv = -1;

    bench_emit("");
    bench_emit("======== 手推方向标定（电机全程不上电，转轮子不危险）========");
    bench_emit("vbus %ld mV   请把轮子朝【车头 / 前进】方向慢慢转几圈：", (long)mv);
    bench_emit("  * 车在地上 -> 直接往前推 10cm；架空 -> 用手把两轮朝车头方向转");
    bench_emit("  * 左右一起转最好；不方便就只转一边。转完就不用管了。");
    bench_emit("下面每 250ms 一行：本拍 = 这 250ms 里的增量，累计 = 从开机到现在");
    bench_emit("");

    t0 = HAL_GetTick();
    for (;;) {
        HAL_Delay(250);
        Encoder_ReadDelta(&dl, &dr);
        acc_l += dl;
        acc_r += dr;
        bench_emit("SPIN t=%6lu ms | 本拍 L %6d R %6d | 累计 L %8ld R %8ld",
                   (unsigned long)(HAL_GetTick() - t0), dl, dr,
                   (long)acc_l, (long)acc_r);
    }
}

void MotorBench_Run(void)
{
    bench_spintest();               /* 永不返回：正式流程要等重烧才恢复 */
}

#else  /* BENCH_MODE == 0：完整台架 */

void MotorBench_Run(void)
{
    int32_t mv = 0, ma = 0;

    bench_emit("=== MOTOR BENCH ===  build %s %s", __DATE__, __TIME__);

    if (ina226_read_bus_voltage_mv(&mv) != HAL_OK) mv = -1;
    if (ina226_read_current_ma(&ma) != HAL_OK)     ma = -100000;
    bench_emit("[PWR]  INA226 主电源 vbus %ld mV, 电流 %ld mA", (long)mv, (long)ma);
    if (mv >= 0 && mv < 6000) {
        bench_emit("!! 12V 没上电 (vbus < 6V)：电机转不动，下面每步大概都是 0");
    }

    /* --- 0. 静止基线：不驱动时计数器必须纹丝不动 --- */
    Load(0, 0);
    Encoder_Reset();
    HAL_Delay(300);
    {
        int dl = 0, dr = 0;
        Encoder_ReadDelta(&dl, &dr);
        bench_emit("[IDLE] 300ms 无驱动 | L %d  R %d | %s",
                   dl, dr,
                   (dl == 0 && dr == 0) ? "OK 无噪声" : "有计数 -> 噪声/串扰/引脚悬空");
    }

    /* --- 1. 分级正转：看有没有计数、符号对不对、随 PWM 单调不单调 --- */
    bench_drive("fwd",         500,    500, 400);
    bench_drive("fwd",        1000,   1000, 400);
    bench_drive("fwd",        2000,   2000, 400);
    bench_drive("fwd",        3500,   3500, 400);
    bench_drive("fwd",        5000,   5000, 400);

    /* --- 2. 反转：计数应当变号，幅值大致相当 --- */
    bench_drive("rev",       -2000,  -2000, 500);
    bench_drive("rev",       -4000,  -4000, 500);

    /* --- 3. 单轮：确认「通道 <-> 轮子」的对应关系，顺带查串扰 --- */
    bench_drive("L-only fwd", 2500,      0, 600);
    bench_drive("R-only fwd",    0,   2500, 600);
    bench_drive("L-only rev",-2500,      0, 600);
    bench_drive("R-only rev",    0,  -2500, 600);

    /* --- 3b. 开环静特性：前馈该拟合的那张表 ---
     * ff4 那轮暴露的真问题：开环恒定 3500 只走 3 个计数（L 轮），恒定 5000 才到 2170 计数/秒
     * —— 这台减速电机的**起转阈值比闭环稳态工作点还高**。闭环在目标 1000/1250 上的平均输出
     * 只有 3000~3300，也就是整窗都在靠积分的瞬时尖峰"顶"过去，静摩擦区里粘滑。
     * 所以先把 pwm -> 稳态计数/秒 量出来，前馈才有依据。 */
    bench_emit("[OPEN] 开环静特性：恒定 PWM 下的稳态转速（前馈拟合依据）");
    bench_open(3000, 2600);
    bench_open(3500, 2600);
    bench_open(4000, 2600);
    bench_open(5000, 2600);
    bench_open(6000, 2600);

    /* --- 4. 速度环闭环：出厂默认前馈验收 ---
     * ff2：fric=1680 把 1000/1250 的稳态均值从 -33%/-12% 拉进 ±5%，前后馈 0.79/1870。
     * ff3：收紧 ilim 到 300（权限 ki×ilim=600）想压极限环，结果 R 轮整窗 st=500、outp=0。
     * ff4：改用拟合出来的 0.662/2327，1250/1500/3000/5000 全落到 ±2.2% 以内；
     *      并用 PB 电平跳变数（e 列）证实那次 R 停住是"轮子真没转"（e=393，正常 1300~1700）
     *      而不是编码器坏了 —— 轮子停在起转阈值以下。
     * ff5：量出了开环静特性（见 bench_open 那几行），确认 3500 以上两轮都平滑单调，
     *      真正的病灶是**起转阈值**（L ~2900、R ~3100~3450）左右不对称，以及
     *      **之前的 2.5s 窗口太短**：同一配置在目标 1000 上能跑出 -9% 也能跑出 +18%。
     * 这一轮就是最终验收：不改任何参数，直接用发货那份 SPEED_FF_SLOPE/FRIC，
     * 七个目标点各跑 6~10s 长窗口。 */
    bench_emit("[CLOSE] 速度环闭环验收：出厂默认前馈 (speed_control.c) + 长窗口");
    SpeedControl_Init();
    SpeedControl_SetRamp(20000.0f);
    SpeedControl_SetTarget(0, 0);
    SpeedControl_Enable(1U);

    /* 验收口（用户 2026 定的新口径）：高速段必须 ≤2%，低速段不追求精度。
     * 这里跑的就是生产默认前馈（speed_control.c 的 SPEED_FF_SLOPE/FRIC），
     * 不是 bench_cl_tune 的临时参数，所以这张表可以直接当交付依据。 */
    bench_cl_default(1000, 8000);
    bench_cl_default(1250, 8000);
    bench_cl_default(1500, 8000);
    bench_cl_default(2000, 8000);
    bench_cl_default(3000, 8000);
    bench_cl_default(4000, 8000);
    bench_cl_default(5000, 8000);
    /* 高速段复跑：≤2% 必须是可复现的，不是一次性运气 */
    bench_cl_default(2000, 8000);
    bench_cl_default(3000, 8000);
    bench_cl_default(4000, 8000);
    bench_cl_default(5000, 8000);

    /* 扫表把 kp/ki 留在最后一组，阶跃段要的是出厂参数，先还原 */
    host_param_set_local(HP_SPEED_KP, 20.0f);
    host_param_set_local(HP_SPEED_KI, 2.0f);
    host_param_set_local(HP_SPEED_ILIMIT, 3000.0f);
    SpeedControl_SetFeedforward(0.662f, 2327.0f);

    /* --- 4b. 目标阶跃响应：加了 20ms 平滑后响应会不会变迟钝，看这里 --- */
    bench_emit("[STEP] 目标阶跃响应（只看左轮，上升时间/超调）");
    bench_cl_step(2000, 3000);
    HAL_Delay(800);
    bench_cl_step(3000, 2000);
    SpeedControl_Stop();            /* 里面会 Load(0,0) 并关使能 */

    /* --- 5. 收尾：电机彻底停，交回正常启动流程 --- */
    Load(0, 0);

    bench_emit("[SUM]  无计数步数 %d/11, 左右不同号步数 %d", s_zero_steps, s_mismatch_steps);
    bench_emit("=== MOTOR BENCH DONE === 下面回放，防 RTT 记录器挂晚了丢行");

    {
        uint8_t i;
        for (i = 0; i < s_log_n; i++) {
            SEGGER_RTT_WriteString(0, s_log[i]);
            SEGGER_RTT_WriteString(0, "\r\n");
        }
    }
    SEGGER_RTT_WriteString(0, "=== MOTOR BENCH REPLAY END ===\r\n");
}

#endif /* BENCH_MODE */

#endif /* MOTOR_BENCH */
