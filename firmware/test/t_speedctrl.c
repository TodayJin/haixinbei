/**
 * @file  t_speedctrl.c   主机侧单元测试：速度环目标斜坡 + 测速窗口（App/Svc/speed_control.c）
 *
 * 为什么值得测：速度环是三层串级的最内层，它的目标斜坡直接决定"起步会不会打滑"
 * 和"角度环反打时车身会不会先倒转一下"。这里把斜坡语义钉死成回归测试：
 *
 *   同号且 |目标| 变大（起步/加速）  -> 慢斜坡 SPEED_RAMP_DEFAULT（20000/s）
 *   反号 或 |目标| 变小（刹车/换向） -> 快斜坡 s_ramp_brake（300000/s）
 *
 * 这条判据 2026-10-02 修过一次：原来写成 `cur * req <= 0`，停车时 cur == 0
 * 让 `0 * req == 0` 也成立，于是"从静止起步"被当成换向走了快斜坡 —— 软启动
 * 整个失效（目标一拍跳到满值，起步打滑/电流冲击）。本文件的用例 1 就是它的
 * 回归测试：谁再把判据改回 `<=`，这里立刻红。
 *
 * 另外覆盖：斜坡不会冲过目标、ramp=0 时直通、未使能不碰 PWM、
 * Stop() 清零、测速滑动平均的量纲（计数/5ms -> mm/s），以及随机目标下的不变量。
 */
#include <stdint.h>
#include <math.h>
#include "speed_control.h"
#include "bsp_board.h"
#include "host_link.h"
#include "t_common.h"

/* ---- 可控桩 ---- */
static int g_dl, g_dr;                  /* 本拍编码器增量（计数/5ms） */
static int g_delta_calls;

void Encoder_ReadDelta(int *dl, int *dr)
{
    g_delta_calls++;
    *dl = g_dl;
    *dr = g_dr;
}

extern int host_load_left(void);        /* host_board.c 里的 Load() 记录 */
extern int host_load_right(void);

/* host_link.c 的参数表在主机侧由测试顶替：这里只要"永远返回调用者给的默认值"，
 * 速度环就会用出厂参数（kp=20/ki=2/kd=0/ilim=3000/1980 计数每圈/65mm 轮径）。 */
float host_param_get(uint16_t id, float fallback) { (void)id; return fallback; }
void  host_param_set_local(uint16_t id, float v) { (void)id; (void)v; }

/* 最近一次 Load 的内容（host_board.c 的 Load 会记下来） */
static void reset_world(void)
{
    g_dl = 0;
    g_dr = 0;
    SpeedControl_Init();
}

/* 走 n 拍 */
static void ticks(int n)
{
    int i;
    for (i = 0; i < n; i++) SpeedControl_Task();
}

int main(void)
{
    /* ---------------------------------------------------------------
     * 1) 从静止起步必须走慢斜坡（<= 的回归测试）
     * --------------------------------------------------------------- */
    reset_world();
    SpeedControl_Enable(1U);
    SpeedControl_SetTarget(2000, 2000);
    ticks(1);
    CHECK_EQ_I(SpeedControl_Target(0), 100);     /* 20000 * 0.005 = 每拍 100 */
    CHECK_EQ_I(SpeedControl_Target(1), 100);
    CHECK(SpeedControl_Target(0) < 2000);        /* 绝不允许一拍跳到满值 */
    ticks(19);                                   /* 再 19 拍：20 拍 * 100 = 2000 */
    CHECK_EQ_I(SpeedControl_Target(0), 2000);
    CHECK_EQ_I(SpeedControl_Target(1), 2000);
    ticks(1);                                    /* 到点后不再长 */
    CHECK_EQ_I(SpeedControl_Target(0), 2000);

    /* ---------------------------------------------------------------
     * 2) 刹车（|目标| 变小）走快斜坡：一拍就到位，且不冲过
     * --------------------------------------------------------------- */
    SpeedControl_SetTarget(500, 500);
    ticks(1);
    CHECK_EQ_I(SpeedControl_Target(0), 500);     /* 2000 -> 500：快斜坡 1500/拍，正好一拍 */
    CHECK_EQ_I(SpeedControl_Target(1), 500);

    /* ---------------------------------------------------------------
     * 3) 换向（反号）走快斜坡：第一阶段跳过零点，之后按慢斜坡爬出反向
     *    —— 这是设计：过零后是"反向加速"，慢一点防止反向冲过头。
     * --------------------------------------------------------------- */
    SpeedControl_SetTarget(-1500, -1500);
    ticks(1);
    CHECK_EQ_I(SpeedControl_Target(0), -1000);   /* 500 - 1500 = -1000，直接跨过 0 */
    CHECK(SpeedControl_Target(0) < 0);           /* 已经反号 */
    ticks(1);
    CHECK_EQ_I(SpeedControl_Target(0), -1100);   /* 同号且 |req|>|cur| -> 慢斜坡 100/拍 */
    ticks(10);
    CHECK_EQ_I(SpeedControl_Target(0), -1500);   /* 到点停住，不冲过 */

    /* ---------------------------------------------------------------
     * 4) 斜坡不会冲过目标（单调逼近，任意起始/目标组合）
     * --------------------------------------------------------------- */
    {
        const int reqs[] = { 7200, 3600, 1, -1, -3600, -7200, 0 };
        int a, b, k;
        for (a = 0; a < 7; a++) {
            for (b = 0; b < 7; b++) {
                reset_world();
                SpeedControl_Enable(1U);
                SpeedControl_SetTarget(reqs[a], reqs[a]);
                ticks(100);                           /* 慢斜坡 100/拍：7200 要 72 拍才到位 */
                CHECK_EQ_I(SpeedControl_Target(0), reqs[a]);
                SpeedControl_SetTarget(reqs[b], reqs[b]);
                for (k = 0; k < 100; k++) {
                    int prev = SpeedControl_Target(0);
                    ticks(1);
                    {
                        int cur = SpeedControl_Target(0);
                        /* 单调逼近且绝不越过目标：从上面下来不许跌破 req，
                         * 从下面上去不许冲过 req，已经在 req 上就原地不动。 */
                        if (prev > reqs[b])      CHECK(cur >= reqs[b] && cur <= prev);
                        else if (prev < reqs[b]) CHECK(cur <= reqs[b] && cur >= prev);
                        else                     CHECK_EQ_I(cur, reqs[b]);
                        CHECK(host_load_left()  >= MOTOR_PWM_MIN && host_load_left()  <= MOTOR_PWM_MAX);
                        CHECK(host_load_right() >= MOTOR_PWM_MIN && host_load_right() <= MOTOR_PWM_MAX);
                    }
                }
                CHECK_EQ_I(SpeedControl_Target(0), reqs[b]);
            }
        }
    }

    /* ---------------------------------------------------------------
     * 5) ramp = 0 表示不限速（直通）
     * --------------------------------------------------------------- */
    reset_world();
    SpeedControl_Enable(1U);
    SpeedControl_SetRamp(0.0f);
    SpeedControl_SetTarget(4321, -4321);
    ticks(1);
    CHECK_EQ_I(SpeedControl_Target(0), 4321);
    CHECK_EQ_I(SpeedControl_Target(1), -4321);

    /* ---------------------------------------------------------------
     * 6) 未使能：只测速不碰 PWM（模块测试页/标定会自己调 Load）
     * --------------------------------------------------------------- */
    reset_world();
    SpeedControl_Stop();                     /* Load(0,0) */
    SpeedControl_SetTarget(3000, 3000);
    g_dl = 12; g_dr = 12;
    SpeedControl_Enable(0U);
    ticks(3);
    CHECK_EQ_I(host_load_left(), 0);         /* 一拍都没写过 PWM */
    CHECK_EQ_I(host_load_right(), 0);
    CHECK(g_delta_calls >= 3);               /* 但是测速照读 */

    /* ---------------------------------------------------------------
     * 7) Stop()：目标/输出清零、并 Load(0,0)
     * --------------------------------------------------------------- */
    SpeedControl_Enable(1U);
    SpeedControl_SetTarget(3000, 3000);
    g_dl = 30; g_dr = 30;
    ticks(10);
    CHECK(host_load_left() != 0);
    SpeedControl_Stop();
    CHECK_EQ_I(host_load_left(), 0);
    CHECK_EQ_I(host_load_right(), 0);
    CHECK_EQ_I(SpeedControl_Target(0), 0);
    CHECK_EQ_I(SpeedControl_Target(1), 0);
    CHECK_EQ_I(SpeedControl_Output(0), 0);
    CHECK_EQ_I(SpeedControl_IsEnabled(), 0);

    /* ---------------------------------------------------------------
     * 8) 测速量纲：窗口平均后 mm/s = 计数/5ms * 200 / 每圈计数 * π * 直径
     *    （默认 1980 计数/圈、65mm 轮径 -> 每 (计数/5ms) 约 20.63 mm/s）
     * --------------------------------------------------------------- */
    reset_world();
    SpeedControl_Enable(1U);
    g_dl = 100; g_dr = 100;
    ticks(4);                                /* SPEED_AVG_TICKS = 4：窗口填满 */
    {
        float expect = 100.0f * (1000.0f / 5.0f) / 1980.0f * 3.14159265f * 65.0f;
        int got = SpeedControl_SpeedMmps(0);
        CHECK(fabsf((float)got - expect) <= 2.0f);
        CHECK_EQ_I(SpeedControl_SpeedMmps(0), SpeedControl_SpeedMmps(1));
        CHECK_EQ_I(SpeedControl_WindowCounts(0), 400);   /* 4 拍 × 100 */
    }
    /* 窗口滚动：第 5 拍起把最老的 100 挤出去（增量改 0 之后总和要降下来） */
    g_dl = 0; g_dr = 0;
    ticks(4);
    CHECK_EQ_I(SpeedControl_WindowCounts(0), 0);

    /* ---------------------------------------------------------------
     * 9) 随机目标/随机编码器下的不变量（UBSan trap 也在起作用）
     * --------------------------------------------------------------- */
    {
        uint32_t seed = 12345U;
        int k;
        reset_world();
        SpeedControl_Enable(1U);
        for (k = 0; k < 20000; k++) {
            int prev = SpeedControl_Target(0);
            int req;
            seed = seed * 1103515245U + 12345U;
            req = (int)((seed >> 16) & 0x3FFFu) - 8192;      /* -8192..8191 */
            seed = seed * 1103515245U + 12345U;
            g_dl = (int)((seed >> 16) & 0x1FFu) - 256;       /* -256..255 */
            g_dr = -g_dl;
            SpeedControl_SetTarget(req, req);
            SpeedControl_Task();
            {
                int cur = SpeedControl_Target(0);
                int step = cur - prev;
                if (step < 0) step = -step;
                CHECK(step <= 1500 + 2);                     /* 一拍最多走快斜坡的量 */
                CHECK(host_load_left()  >= MOTOR_PWM_MIN && host_load_left()  <= MOTOR_PWM_MAX);
                CHECK(host_load_right() >= MOTOR_PWM_MIN && host_load_right() <= MOTOR_PWM_MAX);
            }
        }
    }

    return T_SUMMARY();
}
