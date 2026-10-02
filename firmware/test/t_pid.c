/**
 * @file  t_pid.c   主机侧单元测试：循迹 PID（App/Svc/pid.c）
 *
 * 为什么值得测：这个 PID 的输出直接加到左右轮 PWM 上。它原来没有清零入口、
 * 也没有输入保护 —— 丢线找回后积分会一次性放出（甩头），NaN 输入会一路走到
 * (int) 强转（未定义行为）。这里把这几条行为钉死。
 */
#include <math.h>
#include <stdint.h>
#include "pid.h"
#include "host_link.h"   /* HP_LINE_KP/KI/KD/ILIMIT 的真实编号 */
#include "t_common.h"

/* ---- 极简参数表桩：pid.c 只要 host_param_get(HP_LINE_*) 的兜底值 ----
 * 用真实的 HP_LINE_* 编号（从 host_link.h 取）——它们是 0x0201 这种 16 位编号，
 * 以前按"小序号"建数组会越界（UBSan 立刻抓到）。没设过的 id 返回 fallback，
 * 语义与真表一致。 */
#define NID 65536u
static float   g_param[NID];
static uint8_t g_set[NID];

float host_param_get(uint16_t id, float fallback)
{
    return g_set[id] ? g_param[id] : fallback;
}
void host_param_set_local(uint16_t id, float v)
{
    g_param[id] = v;
    g_set[id]   = 1u;
}

static void set_gains(float kp, float ki, float kd, float ilim)
{
    host_param_set_local(HP_LINE_KP, kp);
    host_param_set_local(HP_LINE_KI, ki);
    host_param_set_local(HP_LINE_KD, kd);
    host_param_set_local(HP_LINE_ILIMIT, ilim);
    PID_Reset();
}

static int feq(float a, float b) { return fabsf(a - b) < 1e-3f; }

int main(void)
{
    /* 1) 只有 P：输出 = Kp × 误差 */
    set_gains(100.0f, 0.0f, 0.0f, 0.0f);
    CHECK(feq(PID_Compute(10.0f, 4.0f), 600.0f));
    CHECK(feq(PID_Compute(4.0f, 10.0f), -600.0f));

    /* 2) 积分项：误差恒为 1、Ki=1、限幅 5 ⇒ 积分只能累到 5 */
    set_gains(0.0f, 1.0f, 0.0f, 5.0f);
    for (int i = 0; i < 20; i++) {
        float out = PID_Compute(1.0f, 0.0f);
        CHECK(out <= 5.0f + 1e-3f);
        CHECK(out >= -5.0f - 1e-3f);
    }
    CHECK(feq(PID_Compute(1.0f, 0.0f), 5.0f));   /* 饱和后恒等于限幅值 */

    /* 3) 第一次调用不算微分（否则 Kd×误差 会变成起步冲击） */
    set_gains(0.0f, 0.0f, 360.0f, 0.0f);
    CHECK(feq(PID_Compute(5.0f, 0.0f), 0.0f));        /* 首拍：微分被跳过 */
    CHECK(feq(PID_Compute(6.0f, 0.0f), 360.0f));      /* 次拍：+(6-5)×360 */
    CHECK(feq(PID_Compute(6.0f, 0.0f), 0.0f));        /* 误差不变：0 */

    /* 4) PID_Reset 之后积分/微分状态都回到初始（重新上线不甩头） */
    set_gains(100.0f, 10.0f, 0.0f, 50.0f);
    for (int i = 0; i < 10; i++) (void)PID_Compute(4.0f, 0.0f);   /* 攒积分 */
    PID_Reset();
    /* 注意：integral += error 发生在算输出之前，所以这一拍是 P + Ki×1 次误差
       = 100×4 + 10×4 = 440（不是 400）—— 这里钉的是"积分不再有旧值"。 */
    CHECK(feq(PID_Compute(4.0f, 0.0f), 440.0f));
    set_gains(0.0f, 0.0f, 360.0f, 0.0f);
    (void)PID_Compute(5.0f, 0.0f);
    PID_Reset();
    CHECK(feq(PID_Compute(5.0f, 0.0f), 0.0f));        /* reset 后首拍微分又跳过 */

    /* 5) NaN / Inf 输入不能把输出变成 NaN（返回 0 = 这一拍不出力） */
    set_gains(100.0f, 10.0f, 0.0f, 50.0f);
    CHECK(PID_Compute(NAN, 0.0f) == 0.0f);
    CHECK(PID_Compute(0.0f, NAN) == 0.0f);
    CHECK(PID_Compute(INFINITY, 0.0f) == 0.0f);
    CHECK(PID_Compute(0.0f, -INFINITY) == 0.0f);
    {
        float out = PID_Compute(1.0f, 0.0f);          /* 出了坏输入还能继续用 */
        CHECK(out == out);
        CHECK(out < INFINITY && out > -INFINITY);
    }

    /* 6) 极端但有限的参数/误差：输出必须保持有限（不能溢出成 inf） */
    set_gains(1.0e6f, 1.0e6f, 1.0e6f, 1.0e6f);
    {
        float out = PID_Compute(1.0e6f, -1.0e6f);
        CHECK(out == out);
        CHECK(out < INFINITY && out > -INFINITY);
    }

    /* 7) 原地 pivot 另一组增益（2026-10-02 台架实测值 900/0/405）：
     *    换模式要清积分/微分历史，但同一模式重复调用不能清（否则每 5ms 清一次，
     *    微分项永远出不来）。 */
    set_gains(100.0f, 0.0f, 0.0f, 0.0f);
    CHECK(feq(PID_Compute(10.0f, 4.0f), 600.0f));      /* 前进：用参数表 */
    PID_SetPivot(1);
    CHECK(feq(PID_Compute(10.0f, 4.0f), 5400.0f));     /* pivot：900×6，首拍微分跳过 */
    PID_SetPivot(1);                                   /* 同模式重复调用 = 无操作 */
    CHECK(feq(PID_Compute(11.0f, 4.0f), 6705.0f));     /* 900×7 + 405×1 */
    set_gains(1234.0f, 0.0f, 0.0f, 7.0f);              /* pivot 期间参数表被忽略 */
    CHECK(feq(PID_Compute(10.0f, 4.0f), 5400.0f));
    PID_SetPivot(0);                                   /* 切回前进：清状态、回参数表 */
    CHECK(feq(PID_Compute(10.0f, 4.0f), 1234.0f * 6.0f));
    PID_Reset();

    return T_SUMMARY();
}
