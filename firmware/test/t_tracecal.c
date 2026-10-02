/**
 * @file  t_tracecal.c   （主机侧单测：8 路灰度模块的采集/通道判定/标定搬运）
 *
 * 被测的是固件里**真的** App/Svc/trace_cal.c（板上跑的是同一份代码），
 * 硬件相关的东西由 stub/host_board.c 的假 IIC / 假 UART4 提供。
 *
 * 覆盖（都是"没有硬件也能证伪"的部分）：
 *   1. 通讯方式 -> 通道 的映射（自动=串口优先）
 *   2. IIC 读成功：原值按**大端**解出来 + 喂总线看门狗 ok
 *   3. IIC 读失败：喂看门狗 fail；固定通道连续失败 5 帧 -> analog_ok=0 但通道不偷偷换
 *   4. 屏正忙时不许插 IIC 读（会花屏）：什么都不读，但看门狗拿到"忙"
 *   5. 自动模式：串口 5 帧不通 -> 换 IIC；IIC 也不通 -> 回落数字口 + 2s 退避后重头试
 *   6. 串口 18 字节读取帧的解析（帧头/帧尾/大端）
 *   7. 标定 -> 柱高/阈值/极性的映射（含没压线/压线两侧）
 *   8. 两套标定 export/import/select_set 的搬运语义（换套不许丢数据）
 *   9. 标定向导：没有模拟量 -> FAILED；再按一下 -> 回 IDLE
 *
 * 不覆盖（需要上板）：真 IIC 时序、真串口波特率、标定时手压黑白的物理过程。
 */
#include "t_common.h"
#include "trace_cal.h"
#include "tracing.h"

/* host_board.c 的注入口/观测口 */
extern void host_uart4_feed(const uint8_t *data, uint16_t len);
extern void host_uart4_reset(void);
extern void host_uart4_reply_set(const uint8_t *frame, uint16_t len);
extern int  host_uart4_reply_wobble;
extern uint16_t host_uart4_tx_len(void);
extern const uint8_t *host_uart4_tx(void);
extern void host_i2c_reg_set(uint8_t reg, uint8_t value);
extern void host_i2c_fail_set(int fail);
extern void host_oled_busy_set(int busy);
extern void host_i2c_stats_reset(void);
extern int  host_i2c_report_ok(void);
extern int  host_i2c_report_fail(void);
extern int  host_i2c_wd_idle(void);
extern int  host_i2c_wd_busy(void);
extern void host_tick_set(uint32_t ms);

static uint32_t t_now;

/* tracing.c（真代码，为了 trace_read_channel 的回落行为）依赖的最小桩：
 * 本测试只关心灰度模块，电机动作只记个数。 */
static int s_pwm_calls;

void Set_Pwm(int mode, uint16_t left, uint16_t right)
{
    (void)mode; (void)left; (void)right;
    s_pwm_calls++;
}

static void tick_to(uint32_t ms) { t_now = ms; host_tick_set(t_now); }
static void poll_in(uint32_t dms) { t_now += dms; host_tick_set(t_now); trace_cal_poll(); }

/* 造一份"标定好了"的数据：blk=100(黑), wht=900(白), thr=500, pol=0(黑原值更小) */
static void make_cal(trace_cal_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->valid = 1U;
    for (uint8_t i = 0U; i < TRACE_CAL_CH; i++) {
        d->blk[i] = 100U;
        d->wht[i] = 900U;
        d->thr[i] = 500U;
        d->pol[i] = 0U;
        d->bad[i] = 0U;
        d->jit[i] = 2U;
    }
}

static void feed_i2c_ch(uint8_t i, uint16_t v)
{
    host_i2c_reg_set((uint8_t)(TRACE_REG_ANALOG + 2U * i), (uint8_t)(v >> 8));
    host_i2c_reg_set((uint8_t)(TRACE_REG_ANALOG + 2U * i + 1U), (uint8_t)(v & 0xFFU));
}

/* 18 字节串口读取帧：{0x75, 8×BE16, 0x11}（每路 = base + i*step） */
static uint8_t s_frame[18];

static void make_uart_frame(uint16_t base, uint16_t step)
{
    s_frame[0] = 0x75U;
    for (uint8_t i = 0U; i < TRACE_CAL_CH; i++) {
        uint16_t v = (uint16_t)(base + (uint16_t)(i * step));
        s_frame[1U + 2U * i]      = (uint8_t)(v >> 8);
        s_frame[1U + 2U * i + 1U] = (uint8_t)(v & 0xFFU);
    }
    s_frame[17] = 0x11U;
}

int main(void)
{
    trace_cal_data_t d;

    /* ---------- 1. 通讯方式 -> 通道 ---------- */
    trace_cal_init();                       /* 自动模式：串口优先 */
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_UART);
    CHECK_EQ_I(trace_cal_analog_ok, 0);
    CHECK_EQ_I(trace_cal_mode, TRACE_MODE_AUTO);

    trace_cal_set_mode(TRACE_MODE_I2C);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_I2C);
    trace_cal_set_mode(TRACE_MODE_DIGITAL);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_NONE);
    trace_cal_set_mode(TRACE_MODE_AUTO);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_UART);
    trace_cal_set_mode(99U);                /* 越界值 -> 自动 */
    CHECK_EQ_I(trace_cal_mode, TRACE_MODE_AUTO);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_UART);

    /* ---------- 2. IIC 读成功：大端原值 + 看门狗 ok ---------- */
    trace_cal_set_mode(TRACE_MODE_I2C);
    host_i2c_fail_set(0);
    for (uint8_t i = 0U; i < TRACE_CAL_CH; i++) feed_i2c_ch(i, (uint16_t)(1000U + i));
    host_i2c_stats_reset();
    tick_to(1000U);
    trace_cal_poll();
    CHECK_EQ_I(trace_cal_analog_ok, 1);
    CHECK_EQ_I(trace_cal_raw[0], 1000);
    CHECK_EQ_I(trace_cal_raw[7], 1007);
    CHECK_EQ_I(host_i2c_report_ok(), 1);      /* 成功要报给看门狗 */
    CHECK_EQ_I(host_i2c_report_fail(), 0);
    CHECK_EQ_I(host_i2c_wd_idle(), 1);        /* 屏空闲：允许它执行恢复 */
    CHECK_EQ_I(host_i2c_wd_busy(), 0);

    /* ---------- 3. 标定 -> 柱高/阈值/极性（走固定阈值那条，先关自适应） ---------- */
    make_cal(&d);
    trace_cal_set_data(&d);
    trace_cal_set_adapt(0U);
    CHECK_EQ_I(trace_cal_valid, 1);
    CHECK_EQ_I(trace_cal_quality(), 8);
    CHECK_EQ_I(trace_cal_ch_ok(0), 1);
    CHECK_EQ_I(trace_cal_span(0), -800);      /* 黑原值 - 白原值 = 100 - 900 */
    CHECK_EQ_I(trace_cal_threshold(0), 500);

    feed_i2c_ch(0U, 100U);                    /* 第 0 路压黑线 */
    feed_i2c_ch(1U, 900U);                    /* 第 1 路压白底 */
    poll_in(20U);
    CHECK_EQ_I(trace_cal_raw[0], 100);
    CHECK_EQ_I(trace_cal_level(0), 255);      /* 黑 -> 满格 */
    CHECK_EQ_I(trace_cal_level(1), 0);        /* 白 -> 空 */
    CHECK_EQ_I(trace_cal_channel(0), 1);      /* 压线 */
    CHECK_EQ_I(trace_cal_channel(1), 0);      /* 没压 */
    CHECK_EQ_I(trace_cal_thr_level(0), 127);  /* 500 映射到半高附近 */

    d.bad[2] = 1U;                            /* 把第 2 路标成无效 */
    trace_cal_set_data(&d);
    CHECK_EQ_I(trace_cal_quality(), 7);
    CHECK_EQ_I(trace_cal_ch_ok(2), 0);
    CHECK_EQ_I(trace_cal_channel(2), -1);     /* 无效路 -> 请回落数字口 */
    CHECK(trace_cal_bad_reason(2) != 0U);
    CHECK_EQ_I(trace_cal_channel(TRACE_CAL_CH), -1);   /* 越界 */
    CHECK_EQ_I(trace_cal_threshold(TRACE_CAL_CH), 0);

    /* ---------- 4. 屏正忙：不许插 IIC 读 ---------- */
    trace_cal_set_mode(TRACE_MODE_I2C);
    host_oled_busy_set(1);
    host_i2c_stats_reset();
    poll_in(20U);
    CHECK_EQ_I(host_i2c_report_ok(), 0);      /* 一次都没读 */
    CHECK_EQ_I(host_i2c_report_fail(), 0);
    CHECK_EQ_I(host_i2c_wd_idle(), 0);
    CHECK_EQ_I(host_i2c_wd_busy(), 1);        /* 报"忙"，看门狗自己会等 */
    host_oled_busy_set(0);

    /* ---------- 5. 固定 IIC 通道：连失败 5 帧 -> analog_ok=0，通道不换 ---------- */
    trace_cal_set_mode(TRACE_MODE_I2C);
    host_i2c_fail_set(1);
    host_i2c_stats_reset();
    poll_in(20U);
    CHECK_EQ_I(host_i2c_report_fail(), 1);
    CHECK_EQ_I(host_i2c_report_ok(), 0);
    for (int i = 0; i < 4; i++) poll_in(20U);
    CHECK_EQ_I(host_i2c_report_fail(), 5);    /* 5 帧都报失败 */
    CHECK_EQ_I(trace_cal_analog_ok, 0);       /* 判"没有模拟量" */
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_I2C); /* 用户定死的通道要留着（界面要显示） */

    /* ---------- 6. 自动模式：串口不通 -> IIC -> 回落数字口 + 2s 退避 ---------- */
    trace_cal_set_mode(TRACE_MODE_AUTO);
    host_i2c_fail_set(1);
    host_uart4_reset();
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_UART);
    for (int i = 0; i < 5; i++) poll_in(20U);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_I2C); /* 串口判死，换 IIC */
    for (int i = 0; i < 5; i++) poll_in(20U);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_NONE);/* 两条都试过 -> 回落数字口 */
    CHECK_EQ_I(trace_cal_analog_ok, 0);
    poll_in(100U);                            /* 退避期内：什么都不做 */
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_NONE);
    poll_in(2001U);                           /* 退避满 -> 按设置从头试 */
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_UART);

    /* ---------- 7. 串口 18 字节读取帧的解析（模块"应答"由桩提供） ---------- */
    trace_cal_set_mode(TRACE_MODE_UART);
    make_uart_frame(300U, 100U);
    host_uart4_reply_set(s_frame, sizeof(s_frame));
    host_uart4_reset();
    poll_in(20U);
    CHECK_EQ_I(trace_cal_analog_ok, 1);
    CHECK_EQ_I(trace_cal_raw[0], 300);
    CHECK_EQ_I(trace_cal_raw[7], 1000);       /* 300 + 7*100 */
    CHECK_EQ_I(host_uart4_tx_len(), 2);       /* 请求是 2 字节：0x57 + 地址 */
    CHECK_EQ_I(host_uart4_tx()[0], 0x57);
    CHECK_EQ_I(host_uart4_tx()[1], TRACE_SENSOR_ADDR);

    /* ---------- 7b. 22 字节"全部"帧也要能解（模拟量在 [5..20]） ---------- */
    {
        uint8_t f22[22];
        memset(f22, 0, sizeof(f22));
        f22[0] = 0x75U;
        f22[1] = 0x00U;
        f22[2] = 0x05U;
        for (uint8_t i = 0U; i < 9U; i++) f22[3U + 2U * i] = 0x00U;   /* 偏移量 9×BE16 先都填 0 */
        for (uint8_t i = 0U; i < 9U; i++) f22[4U + 2U * i] = 0x00U;
        for (uint8_t i = 0U; i < TRACE_CAL_CH; i++) {                 /* 模拟量从第 5 字节起 */
            uint16_t v = (uint16_t)(700U + i);
            f22[5U + 2U * i]      = (uint8_t)(v >> 8);
            f22[5U + 2U * i + 1U] = (uint8_t)(v & 0xFFU);
        }
        f22[21] = 0x15U;
        host_uart4_reply_set(f22, sizeof(f22));
        host_uart4_reset();
        poll_in(20U);
        CHECK_EQ_I(trace_cal_analog_ok, 1);
        CHECK_EQ_I(trace_cal_raw[0], 700);
        CHECK_EQ_I(trace_cal_raw[7], 707);
    }

    /* ---------- 8. 数字口模式：压根不碰模拟量 ---------- */
    trace_cal_set_mode(TRACE_MODE_DIGITAL);
    host_i2c_stats_reset();
    poll_in(20U);
    CHECK_EQ_I(host_i2c_report_ok(), 0);
    CHECK_EQ_I(host_i2c_report_fail(), 0);
    CHECK_EQ_I(host_i2c_wd_idle(), 0);
    CHECK_EQ_I(trace_cal_analog_ok, 0);
    CHECK_EQ_I(trace_cal_src, TRACE_SRC_NONE);
    CHECK_EQ_I(trace_cal_channel(0), -1);     /* 没有模拟量 -> 上层回落数字口 */
    CHECK_EQ_I(trace_cal_level(0), trace_read_channel(0) ? 255U : 0U);

    /* ---------- 9. 两套标定的搬运：换套不许把某套弄丢 ---------- */
    {
        trace_cal_data_t s0, s1, o0, o1;
        uint8_t act = 0xAAU;

        make_cal(&s0);
        make_cal(&s1);
        s0.blk[0] = 111U;                     /* 两套用可区分的值 */
        s1.blk[0] = 222U;
        s0.jit[0] = 3U;
        s1.jit[0] = 4U;

        trace_cal_import(&s0, &s1, 1U);
        CHECK_EQ_I(trace_cal_active_set(), 1);
        trace_cal_get_data(&d);
        CHECK_EQ_I(d.blk[0], 222);            /* 当前应是第 1 套 */
        CHECK_EQ_I(d.jit[0], 4);

        trace_cal_export(&o0, &o1, &act);
        CHECK_EQ_I(o0.blk[0], 111);           /* 导出永远 0 -> set0、1 -> set1 */
        CHECK_EQ_I(o1.blk[0], 222);
        CHECK_EQ_I(act, 1);

        trace_cal_select_set(0U);
        CHECK_EQ_I(trace_cal_active_set(), 0);
        trace_cal_get_data(&d);
        CHECK_EQ_I(d.blk[0], 111);
        trace_cal_select_set(1U);
        CHECK_EQ_I(trace_cal_active_set(), 1);
        trace_cal_get_data(&d);
        CHECK_EQ_I(d.blk[0], 222);
        trace_cal_select_set(9U);             /* 越界：不许把 active 弄成非法值 */
        CHECK(trace_cal_active_set() < TRACE_CAL_SET_COUNT);
    }

    /* ---------- 10. 标定向导：没有模拟量 -> FAILED；再按 -> IDLE ---------- */
    trace_cal_set_mode(TRACE_MODE_DIGITAL);   /* analog_ok = 0 */
    trace_cal_wizard_start();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_BLACK);
    CHECK_EQ_I(trace_cal_valid, 0);           /* 重标期间先不吃旧阈值 */
    trace_cal_wizard_press();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_FAILED);
    trace_cal_wizard_press();                 /* 失败态再按一下 = 退出 */
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_IDLE);

    /* ---------- 11. 完整标定：黑点 100 -> 白点 900 -> 阈值/极性/质量 ---------- */
    trace_cal_set_mode(TRACE_MODE_UART);
    make_uart_frame(100U, 0U);
    host_uart4_reply_set(s_frame, sizeof(s_frame));
    host_uart4_reply_wobble = 0;
    host_uart4_reset();
    poll_in(20U);                             /* 先有一帧成功的采集：analog_ok 才会置 1 */
    CHECK_EQ_I(trace_cal_analog_ok, 1);

    trace_cal_wizard_start();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_BLACK);
    trace_cal_wizard_press();                 /* 采黑点 */
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_WHITE);
    CHECK_EQ_I(trace_cal_valid, 0);           /* 两点都收下之前不算有效 */

    make_uart_frame(900U, 0U);                /* 换成"全压白底" */
    trace_cal_wizard_press();                 /* 采白点 */
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_DONE);

    trace_cal_get_data(&d);
    CHECK_EQ_I(d.valid, 1);
    CHECK_EQ_I(d.blk[0], 100);
    CHECK_EQ_I(d.wht[0], 900);
    CHECK_EQ_I(d.thr[0], 500);                /* 中点阈值 */
    CHECK_EQ_I(d.pol[0], 0);                  /* 黑原值比白小 */
    CHECK_EQ_I(d.bad[0], 0);
    CHECK_EQ_I(d.jit[0], 0);                  /* 数据是常数帧，抖动 0 */
    CHECK_EQ_I(trace_cal_quality(), 8);
    CHECK_EQ_I(trace_cal_ch_ok(7), 1);
    CHECK_EQ_I(trace_cal_span(7), -800);
    CHECK_EQ_I(trace_cal_threshold(7), 500);
    CHECK_EQ_I(trace_cal_bad_reason(0), 0);

    /* 完成态 3s 后自己退回 IDLE（用户 m03536 的"不用再按键"） */
    poll_in(1500U);
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_DONE);
    poll_in(1600U);
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_IDLE);
    trace_cal_wizard_press();                 /* IDLE 下按下不做事 */
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_IDLE);

    /* ---------- 12. 采到的点整体在抖 -> 整点作废、留在原档等重按 ---------- */
    host_uart4_reply_wobble = 6000;           /* 每次应答给第 0 路 ±6000 */
    make_uart_frame(100U, 0U);                /* 其余 7 路仍是常数 */
    trace_cal_wizard_start();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_BLACK);
    CHECK_EQ_I(trace_cal_quality(), 0);       /* 重标期间旧标定先不吃 */
    trace_cal_wizard_press();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_BLACK);   /* 没前进 */
    CHECK_EQ_I(trace_cal_retry_hint(), 1);    /* 界面提示"重按一下" */
    host_uart4_reply_wobble = 0;

    /* 抖完再按一次（数据稳了）就正常推进 */
    trace_cal_wizard_press();
    CHECK_EQ_I(trace_cal_state(), TRACE_CAL_WAIT_WHITE);
    trace_cal_wizard_cancel();

    return T_SUMMARY();
}
