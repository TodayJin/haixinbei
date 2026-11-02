/**
 * @file  t_tracing.c   主机侧单元测试：灰度读数归一化（App/Svc/tracing.c）
 *
 * 为什么值得测：trace_read_channel() 是循迹环唯一的输入，它有三个来源
 * （标定过的模拟量 / 数字口 / 反相开关），而且索引来自循环变量和外部配置。
 * 这里把边界（idx≥8）、回落、反相、以及旧板开关式循迹函数的 256 种输入
 * 组合全部过一遍（UBSan trap 模式下越界会直接打死进程）。
 */
#include <stdint.h>
#include "tracing.h"
#include "motor.h"
#include "trace_cal.h"
#include "bsp_board.h"
#include "t_common.h"

/* ---- 可控桩：模拟量是否就绪（数字口电平用 stub 里的 host_trace_bits_set） ---- */
static int     g_analog_ok;     /* 0 = 没模拟量（trace_cal_channel 返回 -1） */
static int     g_analog_val;    /* 模拟量归一化结果 */
static int     g_pwm_calls;
static int     g_pwm_last[3];

extern void host_trace_bits_set(uint8_t bits);   /* host_board.c 的注入口 */

int trace_cal_channel(uint8_t idx)
{
    (void)idx;
    return g_analog_ok ? g_analog_val : -1;
}

void Set_Pwm(int mode, uint16_t left, uint16_t right)
{
    g_pwm_calls++;
    g_pwm_last[0] = mode;
    g_pwm_last[1] = (int)left;
    g_pwm_last[2] = (int)right;
}

int main(void)
{
    /* 1) 索引越界：8..255 一律返回 0，不能读数组外（UBSan 会抓） */
    g_analog_ok = 0;
    for (int i = 8; i < 256; i++) {
        CHECK_EQ_I(trace_read_channel((uint8_t)i), 0);
    }

    /* 2) 没有模拟量时回落数字口 */
    host_trace_bits_set(0x0);
    CHECK_EQ_I(trace_read_channel(0), 0);
    host_trace_bits_set(0x1);
    CHECK_EQ_I(trace_read_channel(0), 1);
    host_trace_bits_set(0x80);
    CHECK_EQ_I(trace_read_channel(7), 1);
    CHECK_EQ_I(trace_read_channel(0), 0);

    /* 3) trace_invert 反相 */
    trace_invert = 1;
    host_trace_bits_set(0x5);
    CHECK_EQ_I(trace_read_channel(0), 0);
    CHECK_EQ_I(trace_read_channel(1), 1);
    CHECK_EQ_I(trace_read_channel(2), 0);
    trace_invert = 0;

    /* 4) 有模拟量时优先用模拟量（且模拟量也被反相开关归一化） */
    g_analog_ok = 1;
    g_analog_val = 1;
    host_trace_bits_set(0x0);                 /* 数字口说没有，模拟量说有 ⇒ 听模拟量 */
    CHECK_EQ_I(trace_read_channel(3), 1);
    g_analog_val = 0;
    host_trace_bits_set(0xFF);
    CHECK_EQ_I(trace_read_channel(3), 0);
    g_analog_ok = 0;

    /* 5) read_tracing_data 填满 8 路且与逐路读一致 */
    host_trace_bits_set(0xA5);                 /* 1010 0101 */
    read_tracing_data();
    for (int i = 0; i < 8; i++) {
        CHECK_EQ_I(tracing_data[i], (0xA5 >> i) & 1);
    }
    CHECK_EQ_I(trace_on_line_count(), 4);

    /* 6) 旧板开关式循迹：256 种输入组合全跑一遍，不能越界、不能除零 */
    for (int pat = 0; pat < 256; pat++) {
        for (int i = 0; i < 8; i++) tracing_data[i] = (pat >> i) & 1;
        motor_tracing(1500, 900);
    }
    CHECK(g_pwm_calls >= 256);

    /* 7) 全部丢线时必须走减速/停那一支（旧的兜底行为） */
    g_pwm_calls = 0;
    for (int i = 0; i < 8; i++) tracing_data[i] = 0;
    motor_tracing(1500, 900);
    CHECK_EQ_I(g_pwm_calls, 1);
    CHECK_EQ_I(g_pwm_last[0], 2);
    CHECK_EQ_I(g_pwm_last[1], 290);
    CHECK_EQ_I(g_pwm_last[2], 290);

    return T_SUMMARY();
}
