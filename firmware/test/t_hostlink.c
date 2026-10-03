/**
 * @file  t_hostlink.c   主机侧单元测试：上位机链路协议（App/Svc/host_link.c）
 *
 * 为什么值得测：这是唯一一条外部输入通道（LPUART1），上位机/调试脚本发来的
 * 字节是"不可信输入"。帧解析器的越界、参数表被 NaN 污染、以及"收到什么就回什么"
 * 的状态码，都能在这里用真源码跑出来。
 *
 * 用例里自己按文档实现了一份 CRC16-CCITT 和组帧，不调用被测代码的 CRC ——
 * 这样协议漂移（比如 CRC 覆盖范围改了）会被抓出来。
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "host_link.h"
#include "t_common.h"

extern void host_dl_feed(const uint8_t *data, uint16_t len);
extern void host_dl_reset(void);
extern uint16_t host_dl_tx_len(void);
extern const uint8_t *host_dl_tx(void);
extern void host_dl_tx_clear(void);

/* ---- 独立参考实现 ---- */
static uint16_t ref_crc(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void build(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *pl, uint8_t len)
{
    out[0] = 0xA5; out[1] = 0x5A; out[2] = type; out[3] = seq; out[4] = len;
    if (len && pl) memcpy(&out[5], pl, len);
    uint16_t c = ref_crc(&out[2], (uint16_t)(3u + len));
    out[5u + len] = (uint8_t)(c & 0xFFu);
    out[6u + len] = (uint8_t)(c >> 8);
}

static void feed_and_poll(const uint8_t *f, uint8_t flen)
{
    host_dl_feed(f, flen);
    host_link_poll(0u);
}

/* 在发送缓冲里找"最后一个"指定类型/序号的帧；返回帧首指针，*plen 给出负载长度 */
static const uint8_t *find_frame(uint8_t type, uint8_t seq, uint8_t *plen)
{
    const uint8_t *tx = host_dl_tx();
    uint16_t n = host_dl_tx_len();
    const uint8_t *hit = NULL;
    uint16_t i = 0;
    while (i + 7u <= n) {
        if (tx[i] == 0xA5u && tx[i + 1u] == 0x5Au) {
            uint8_t l = tx[i + 4u];
            if (i + 7u + l > n) break;
            if (tx[i + 2u] == type && tx[i + 3u] == seq) { hit = &tx[i]; if (plen) *plen = l; }
            i = (uint16_t)(i + 7u + l);
        } else {
            i++;
        }
    }
    return hit;
}

static int32_t count_frames(uint8_t type)
{
    const uint8_t *tx = host_dl_tx();
    uint16_t n = host_dl_tx_len(), i = 0;
    int32_t c = 0;
    while (i + 7u <= n) {
        if (tx[i] == 0xA5u && tx[i + 1u] == 0x5Au) {
            uint8_t l = tx[i + 4u];
            if (i + 7u + l > n) break;
            if (tx[i + 2u] == type) c++;
            i = (uint16_t)(i + 7u + l);
        } else i++;
    }
    return c;
}

/* ---- v2 用的小工具 ---- */
static void put16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* 发送缓冲里有没有出现过这段文本（T_TEXT 帧的负载原样是 ASCII） */
static int tx_has_text(const char *needle)
{
    const uint8_t *tx = host_dl_tx();
    uint16_t n = host_dl_tx_len();
    size_t l = strlen(needle);
    uint16_t i;
    if (l == 0u || n < l) return 0;
    for (i = 0; (size_t)i + l <= (size_t)n; i++) {
        if (memcmp(&tx[i], needle, l) == 0) return 1;
    }
    return 0;
}

/* host_extstub.c 的观测变量（协议层只关心"该调的调了没有"） */
extern int      stubx_drive_set_calls, stubx_last_a, stubx_last_b, stubx_drive_set_rc;
extern uint8_t  stubx_last_mode;
extern int      stubx_save_calls, stubx_center_calls, stubx_play_n;
extern int      stubx_action_set_calls, stubx_servo_set[3], stubx_servo_dev[3];
extern uint8_t  stubx_drive_mode, stubx_drive_enabled, stubx_nvram_state;
extern int      stubx_wd_trips, stubx_clear_faults;

/* host_hal.c 的假时钟（遥测有节流，要推进时间才会再发） */
extern void host_tick_add(uint32_t ms);

int main(void)
{
    uint8_t f[64], pl[8];

    /* 0) 参考 CRC 与自己的实现自检（"123456789" = 0x29B1） */
    CHECK_EQ_I(ref_crc((const uint8_t *)"123456789", 9), 0x29B1);

    host_link_init();          /* 参数表回到默认值，统计清零 */
    host_dl_reset();

    /* 1) HELLO_REQ → HELLO 帧，带名字、参数表 CRC、能力位（v2 起 24 字节） */
    memset(pl, 0, sizeof(pl));
    build(f, 0x41, 7, NULL, 0);
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x40, 7, &l);
        CHECK(r != NULL);
        CHECK_EQ_I(l, 24);
        if (r) {
            CHECK_EQ_I(r[5], 0x02);                     /* fw version 0x0002 */
            CHECK_EQ_I(r[6], 0x00);
            CHECK_EQ_I(r[7], 0x02);                     /* proto version 2 */
            CHECK_EQ_I(memcmp(&r[13], "quandi-car", 10), 0);   /* 名字在 payload+8 */
            /* 能力位在 payload+20：7 位全开（上位机据此决定页面显示什么） */
            CHECK_EQ_I(r[25] | (r[26] << 8) | (r[27] << 16) | (r[28] << 24), 0x7Fu);
        }
    }

    /* 2) 单参数 GET → REPORT（回读值与写入值一致） */
    host_param_set_local(HP_BASE_SPEED, 1234.0f);
    host_dl_tx_clear();
    pl[0] = (uint8_t)(HP_BASE_SPEED & 0xFFu);
    pl[1] = (uint8_t)(HP_BASE_SPEED >> 8);
    build(f, 0x10, 9, pl, 2);
    feed_and_poll(f, 9);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x12, 9, &l);
        float v;
        uint32_t bits;
        CHECK(r != NULL);
        if (r) {
            CHECK_EQ_I(l, 6);
            CHECK_EQ_I(r[5] | (r[6] << 8), HP_BASE_SPEED);
            bits = (uint32_t)r[7] | ((uint32_t)r[8] << 8) | ((uint32_t)r[9] << 16)
                 | ((uint32_t)r[10] << 24);
            memcpy(&v, &bits, 4);
            CHECK(v > 1233.9f && v < 1234.1f);
        }
    }

    /* 3) 全表 dump（id=0xFFFF）：分帧发，不能一次全塞进 buffer 就卡死主循环 */
    host_dl_tx_clear();
    pl[0] = 0xFF; pl[1] = 0xFF;
    build(f, 0x10, 11, pl, 2);
    feed_and_poll(f, 9);
    {
        int32_t after_one = count_frames(0x12);
        CHECK(after_one > 0);                       /* 第一拍就发了一些 */
        CHECK(after_one <= 8);                      /* 但绝不是一次性发完 */
        for (int i = 0; i < 60; i++) host_link_poll(0u);   /* 把剩下的抽干 */
        CHECK_EQ_I(count_frames(0x12), host_param_count());  /* 一项不少也不多 */
        CHECK(host_param_count() >= 43u);           /* v2 后 30 + 13 个控制环标定量 */
    }

    /* 4) 正常的 PARAM_SET → ACK(ST_OK) + 回读 REPORT */
    host_dl_tx_clear();
    pl[0] = (uint8_t)(HP_BASE_SPEED & 0xFFu);
    pl[1] = (uint8_t)(HP_BASE_SPEED >> 8);
    {
        float v = 1500.0f; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)(bits & 0xFFu); pl[3] = (uint8_t)((bits >> 8) & 0xFFu);
        pl[4] = (uint8_t)((bits >> 16) & 0xFFu); pl[5] = (uint8_t)((bits >> 24) & 0xFFu);
    }
    build(f, 0x11, 13, pl, 6);
    feed_and_poll(f, 13);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 13, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);              /* payload[1] = ST_OK */
        CHECK(host_param_get(HP_BASE_SPEED, -1.0f) > 1499.9f);
    }

    /* 5) NaN：范围检查必须挡住（NaN 与任何数比较都是 false，写成 v<min||v>max 会漏） */
    host_dl_tx_clear();
    {
        float v = NAN; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)(bits & 0xFFu); pl[3] = (uint8_t)((bits >> 8) & 0xFFu);
        pl[4] = (uint8_t)((bits >> 16) & 0xFFu); pl[5] = (uint8_t)((bits >> 24) & 0xFFu);
    }
    build(f, 0x11, 15, pl, 6);
    feed_and_poll(f, 15);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 15, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);              /* ST_OUT_OF_RANGE */
        CHECK(host_param_get(HP_BASE_SPEED, -1.0f) > 1499.9f);   /* 旧值没被污染 */
    }

    /* 6) ±Inf 同样要被挡住 */
    {
        float v = INFINITY; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)(bits & 0xFFu); pl[3] = (uint8_t)((bits >> 8) & 0xFFu);
        pl[4] = (uint8_t)((bits >> 16) & 0xFFu); pl[5] = (uint8_t)((bits >> 24) & 0xFFu);
    }
    host_dl_tx_clear();
    build(f, 0x11, 17, pl, 6);
    feed_and_poll(f, 17);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 17, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);
    }

    /* 7) 未知参数 id → ST_BAD_PARAM */
    host_dl_tx_clear();
    pl[0] = 0x77; pl[1] = 0x77;
    build(f, 0x10, 19, pl, 2);
    feed_and_poll(f, 9);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 19, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x03);
    }

    /* 8) 未知帧类型 → ST_UNKNOWN_TYPE（0x02），不能装死也不能乱回 */
    host_dl_tx_clear();
    build(f, 0x7E, 21, NULL, 0);
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 21, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x02);
    }

    /* 9) CRC 错的帧必须被丢弃（不能执行命令） */
    host_dl_tx_clear();
    build(f, 0x41, 23, NULL, 0);
    f[6] ^= 0xFF;                       /* 破坏 CRC 低字节 */
    feed_and_poll(f, 7);
    CHECK(find_frame(0x40, 23, NULL) == NULL);

    /* 10) len 超过 MAX_PAYLOAD(128) 的假头：解析器要丢头重同步，且随后还能收正常帧 */
    host_dl_tx_clear();
    {
        uint8_t junk[8];
        junk[0] = 0xA5; junk[1] = 0x5A; junk[2] = 0x41; junk[3] = 1; junk[4] = 200;   /* len=200 */
        host_dl_feed(junk, 5);
        build(f, 0x41, 25, NULL, 0);
        host_dl_feed(f, 7);
        host_link_poll(0u);
        CHECK(find_frame(0x40, 25, NULL) != NULL);
    }

    /* 11) 剥壳：一次喂进去很多帧，都要处理（不丢帧） */
    host_dl_tx_clear();
    {
        uint8_t batch[7 * 5];
        for (int i = 0; i < 5; i++) build(&batch[7 * i], 0x41, (uint8_t)(40 + i), NULL, 0);
        host_dl_feed(batch, sizeof(batch));
        host_link_poll(0u);
        for (int i = 0; i < 5; i++) CHECK(find_frame(0x40, (uint8_t)(40 + i), NULL) != NULL);
    }

    /* 12) telem_hz 边界：0 抬到 1、超大压到 200（不能让 1000/hz 变成除零） */
    {
        int32_t arg;
        host_dl_tx_clear();
        pl[0] = 0x08;   /* CMD_SET_TELEM_HZ */
        arg = 0;  memcpy(&pl[1], &arg, 4);
        build(f, 0x20, 31, pl, 5);
        feed_and_poll(f, 12);
        CHECK(host_link_stats()->telem_hz >= 1u);

        host_dl_tx_clear();
        arg = 100000; memcpy(&pl[1], &arg, 4);
        build(f, 0x20, 33, pl, 5);
        feed_and_poll(f, 12);
        CHECK(host_link_stats()->telem_hz <= 200u);
    }

    /* 13) fuzz：随机字节灌进解析器，只要求不崩（UBSan trap 会直接打死进程），
     *     而且最后仍能正常解析一帧 */
    {
        uint32_t seed = 20261002u;
        uint8_t rnd[64];
        for (int round = 0; round < 2000; round++) {
            for (int i = 0; i < 64; i++) {
                seed = seed * 1103515245u + 12345u;
                rnd[i] = (uint8_t)(seed >> 16);
            }
            host_dl_feed(rnd, 64);
            host_link_poll(0u);
            host_dl_reset();
        }
        host_dl_tx_clear();
        build(f, 0x41, 55, NULL, 0);
        feed_and_poll(f, 7);
        CHECK(find_frame(0x40, 55, NULL) != NULL);
        t_checks++;
    }

    /* 14) 统计计数在动（说明真的走到了处理路径） */
    CHECK(host_link_stats()->rx_frames > 0u);
    CHECK(host_link_stats()->tx_frames > 0u);

    /* ==================================================================
     *  以下为协议 v2（用户 m13466 的新上位机）新增的帧与元数据
     * ================================================================== */

    /* 15) 参数表元数据：id 唯一、范围能取到、未知 id 返回 0 */
    {
        uint16_t n = host_param_count(), i, j;
        int dup = 0;
        float def = 0.0f, lo = 0.0f, hi = 0.0f;
        for (i = 0; i < n; i++) {
            for (j = (uint16_t)(i + 1u); j < n; j++) {
                if (host_param_id_at(i) == host_param_id_at(j)) dup++;
            }
        }
        CHECK_EQ_I(dup, 0);
        CHECK_EQ_I(host_param_bounds(HP_PIVOT_KP, &def, &lo, &hi), 1);
        CHECK(def == 900.0f);
        CHECK(lo == 0.0f);
        CHECK(hi == 20000.0f);
        CHECK_EQ_I(host_param_bounds(HP_GYRO_RATE_KP, &def, &lo, &hi), 1);
        CHECK(def == 250.0f && lo == 10.0f && hi == 2000.0f);
        CHECK_EQ_I(host_param_bounds(0x7777u, NULL, NULL, NULL), 0);
        CHECK_EQ_I(host_param_id_at(n), 0u);         /* 越界返回 0，不崩 */
        CHECK(host_param_table_crc() != 0u);
    }

    /* 16) v2 参数项（控制环标定量）也能按协议改：HP_PIVOT_KP → 1234 */
    host_dl_tx_clear();
    pl[0] = (uint8_t)(HP_PIVOT_KP & 0xFFu);
    pl[1] = (uint8_t)(HP_PIVOT_KP >> 8);
    {
        float v = 1234.0f; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)bits; pl[3] = (uint8_t)(bits >> 8);
        pl[4] = (uint8_t)(bits >> 16); pl[5] = (uint8_t)(bits >> 24);
    }
    build(f, 0x11, 60, pl, 6);
    feed_and_poll(f, 13);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 60, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK(host_param_get(HP_PIVOT_KP, -1.0f) == 1234.0f);
    }
    /* 范围内改成 5000，再试一个超过上限 20000 的值：越界要被挡且旧值不动 */
    {
        float v = 5000.0f; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)bits; pl[3] = (uint8_t)(bits >> 8);
        pl[4] = (uint8_t)(bits >> 16); pl[5] = (uint8_t)(bits >> 24);
    }
    build(f, 0x11, 61, pl, 6);
    feed_and_poll(f, 13);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 61, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK(host_param_get(HP_PIVOT_KP, -1.0f) == 5000.0f);
    }
    {
        float v = 25000.0f; uint32_t bits; memcpy(&bits, &v, 4);
        pl[2] = (uint8_t)bits; pl[3] = (uint8_t)(bits >> 8);
        pl[4] = (uint8_t)(bits >> 16); pl[5] = (uint8_t)(bits >> 24);
    }
    build(f, 0x11, 63, pl, 6);
    feed_and_poll(f, 13);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 63, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);                             /* ST_OUT_OF_RANGE */
        CHECK(host_param_get(HP_PIVOT_KP, -1.0f) == 5000.0f);      /* 旧值没被污染 */
    }
    host_param_set_local(HP_PIVOT_KP, 900.0f);

    /* 17) 默认发 64 字节 T_TELEM2，字段逐个抽查 */
    stubx_drive_enabled = 1;
    stubx_drive_mode = 2;
    stubx_nvram_state = 0;
    stubx_servo_dev[1] = -25;               /* 两路中位偏差要能带出去（TELEM2 60/62） */
    stubx_servo_dev[2] = 40;
    host_tick_add(25u);                     /* 遥测有节流，推进时间才会发 */
    host_dl_tx_clear();
    host_link_poll(0u);
    {
        uint8_t l = 0;
        const uint8_t *t = find_frame(0x03, 0, &l);
        CHECK(t != NULL);
        CHECK_EQ_I(l, 64);
        if (t) {
            CHECK_EQ_I(t[5u + 32u], 5);                              /* gray[0] */
            CHECK_EQ_I(t[5u + 39u], 75);                             /* gray[7] */
            CHECK_EQ_I((int16_t)(t[45] | (t[46] << 8)), 425);        /* 重心 ×100 */
            CHECK_EQ_I((int16_t)(t[47] | (t[48] << 8)), -123);       /* 循迹环输出 */
            CHECK_EQ_I((int16_t)(t[49] | (t[50] << 8)), 12);         /* 角度误差 1.25×10 */
            CHECK_EQ_I((int16_t)(t[51] | (t[52] << 8)), 25);         /* 期望角速度 2.5×10 */
            CHECK_EQ_I((int16_t)(t[53] | (t[54] << 8)), -3);         /* 修正量 */
            CHECK_EQ_I(t[5u + 50u], 3);                              /* 运动模式 */
            CHECK_EQ_I(t[5u + 51u], 0);                              /* 任务 */
            CHECK_EQ_I(t[5u + 52u], 90);                             /* 舵机1 */
            CHECK_EQ_I(t[5u + 53u], 45);                             /* 舵机2 */
            CHECK_EQ_I(t[5u + 54u], 0);                              /* 当前动作 */
            CHECK_EQ_I(t[5u + 55u], 0);                              /* nvram */
            CHECK_EQ_I(t[5u + 56u], 2);                              /* 驾驶模式 */
            CHECK_EQ_I(t[5u + 57u], 1);                              /* 驾驶使能 */
            CHECK_EQ_I(t[5u + 58u], 1);                              /* 路口 */
            CHECK_EQ_I(t[5u + 59u], 0);                              /* 看门狗次数 */
            CHECK_EQ_I((int16_t)(t[65] | (t[66] << 8)), -25);        /* 舵机1 中位偏差 */
            CHECK_EQ_I((int16_t)(t[67] | (t[68] << 8)), 40);         /* 舵机2 中位偏差 */
            CHECK((t[5u + 30u] & 0x04u) != 0u);                      /* flags: FL_HOST_DRIVE */
        }
        CHECK_EQ_I(count_frames(0x01), 0);      /* 默认不发老 32B 帧 */
    }

    /* 18) CMD_SET_TELEM_MODE=0 ⇒ 只发老 32B T_TELEM */
    host_dl_tx_clear();
    pl[0] = 0x0F;
    {
        int32_t arg = 0; memcpy(&pl[1], &arg, 4);
    }
    build(f, 0x20, 62, pl, 5);
    feed_and_poll(f, 12);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 62, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
    }
    host_tick_add(25u);
    host_dl_tx_clear();
    host_link_poll(0u);
    {
        uint8_t l = 0;
        CHECK(find_frame(0x01, 0, &l) != NULL);
        CHECK_EQ_I(l, 32);
        CHECK(find_frame(0x03, 0, &l) == NULL);
    }
    /* 切回 mode=1（后面的用例都按默认帧看） */
    pl[0] = 0x0F;
    {
        int32_t arg = 1; memcpy(&pl[1], &arg, 4);
    }
    build(f, 0x20, 64, pl, 5);
    feed_and_poll(f, 12);

    /* 19) 动作表：GET → 28 字节；SET → ACK + 立刻下发 + 回读整表 */
    host_dl_tx_clear();
    build(f, 0x16, 66, NULL, 0);
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x17, 66, &l);
        CHECK(r != NULL);
        CHECK_EQ_I(l, 28);
        if (r) {
            CHECK_EQ_I(r[5u + 0u], 0);      /* 动作1 舵1 */
            CHECK_EQ_I(r[5u + 1u], 0);
            CHECK_EQ_I(r[5u + 2u], 60);     /* 动作2 舵1 */
            CHECK_EQ_I(r[5u + 3u], 120);
        }
    }

    host_dl_tx_clear();
    pl[0] = 3; pl[1] = 100; pl[2] = 20;
    build(f, 0x18, 68, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 68, &l);
        const uint8_t *r;
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_action_set_calls, 1);
        CHECK_EQ_I(stubx_play_n, 3);           /* 改完立刻掰到该动作（台架场景） */
        r = find_frame(0x17, 68, &l);
        CHECK(r != NULL);
        if (r) {
            CHECK_EQ_I(r[5u + 4u], 100);       /* 动作3 舵1 */
            CHECK_EQ_I(r[5u + 5u], 20);
        }
    }
    /* 动作号越界 / 角度超过 180：都被挡，且不掰舵机 */
    host_dl_tx_clear();
    pl[0] = 99; pl[1] = 10; pl[2] = 10;
    build(f, 0x18, 70, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 70, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);         /* ST_OUT_OF_RANGE */
    }
    host_dl_tx_clear();
    pl[0] = 1; pl[1] = 200; pl[2] = 10;
    build(f, 0x18, 72, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 72, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);
        CHECK_EQ_I(stubx_action_set_calls, 1); /* 没写进去 */
    }

    /* 20) T_ACTION_SAVE 与 T_PARAM_SAVE 都要真的排一次 Flash 保存 */
    {
        int before = stubx_save_calls;
        host_dl_tx_clear();
        build(f, 0x19, 74, NULL, 0);
        feed_and_poll(f, 7);
        {
            uint8_t l = 0;
            const uint8_t *a = find_frame(0x21, 74, &l);
            CHECK(a != NULL);
            if (a) CHECK_EQ_I(a[6], 0x00);
        }
        CHECK_EQ_I(stubx_save_calls, before + 1);
    }
    {
        int before = stubx_save_calls;
        host_dl_tx_clear();
        build(f, 0x13, 76, NULL, 0);
        feed_and_poll(f, 7);
        {
            uint8_t l = 0;
            const uint8_t *a = find_frame(0x21, 76, &l);
            CHECK(a != NULL);
            if (a) CHECK_EQ_I(a[6], 0x00);
        }
        CHECK_EQ_I(stubx_save_calls, before + 1);
        CHECK_EQ_I(tx_has_text("param save queued"), 1);
    }
    {
        int before = stubx_save_calls;
        host_dl_tx_clear();
        pl[0] = 0x06;                          /* CMD_SAVE_PARAMS */
        build(f, 0x20, 78, pl, 1);
        feed_and_poll(f, 8);
        {
            uint8_t l = 0;
            const uint8_t *a = find_frame(0x21, 78, &l);
            CHECK(a != NULL);
            if (a) CHECK_EQ_I(a[6], 0x00);
        }
        CHECK_EQ_I(stubx_save_calls, before + 1);
    }

    /* 21) 舵机：T_SERVO_SET 合法/越界/通道非法；T_SERVO_PLAY 回中与越界 */
    host_dl_tx_clear();
    pl[0] = 1; put16(&pl[1], 90);
    build(f, 0x1A, 80, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 80, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_servo_set[1], 90);
    }
    host_dl_tx_clear();
    pl[0] = 1; put16(&pl[1], 200);
    build(f, 0x1A, 82, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 82, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);         /* 角度越界 */
        CHECK_EQ_I(stubx_servo_set[1], 90);    /* 没动 */
    }
    host_dl_tx_clear();
    pl[0] = 3; put16(&pl[1], 45);
    build(f, 0x1A, 84, pl, 3);
    feed_and_poll(f, 10);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 84, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x03);         /* 通道非法 = ST_BAD_PARAM */
    }
    host_dl_tx_clear();
    pl[0] = 0;                                 /* n=0 ⇒ 回中 */
    build(f, 0x1B, 86, pl, 1);
    feed_and_poll(f, 8);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 86, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_center_calls, 1);
    }
    host_dl_tx_clear();
    pl[0] = 5;
    build(f, 0x1B, 88, pl, 1);
    feed_and_poll(f, 8);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 88, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_play_n, 5);
    }
    host_dl_tx_clear();
    pl[0] = 99;
    build(f, 0x1B, 90, pl, 1);
    feed_and_poll(f, 8);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 90, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x04);
    }

    /* 22) 驾驶：正常 → ACK + T_DRIVE_REPORT；任务在跑 → ST_NOT_READY + 文本 */
    host_dl_tx_clear();
    pl[0] = 1; put16(&pl[1], 300); put16(&pl[3], -300);
    build(f, 0x1C, 92, pl, 5);
    feed_and_poll(f, 12);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 92, &l);
        const uint8_t *r;
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_drive_set_calls, 1);
        CHECK_EQ_I(stubx_last_mode, 1);
        CHECK_EQ_I(stubx_last_a, 300);
        CHECK_EQ_I(stubx_last_b, -300);
        r = find_frame(0x1D, 92, &l);
        CHECK(r != NULL);
        CHECK_EQ_I(l, 12);
        if (r) {
            CHECK_EQ_I(r[5], 1);               /* 模式 */
            CHECK_EQ_I(r[6], 1);               /* 使能 */
            CHECK_EQ_I((int16_t)(r[7] | (r[8] << 8)), 300);
            CHECK_EQ_I((int16_t)(r[9] | (r[10] << 8)), -300);
        }
    }
    host_dl_tx_clear();
    stubx_drive_set_rc = -2;                    /* 模拟 renwu 在跑 */
    pl[0] = 1; put16(&pl[1], 100); put16(&pl[3], 100);
    build(f, 0x1C, 94, pl, 5);
    feed_and_poll(f, 12);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 94, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x05);         /* ST_NOT_READY */
        CHECK_EQ_I(tx_has_text("drive refused"), 1);
        CHECK(find_frame(0x1D, 94, &l) != NULL);   /* 仍然回报当前状态 */
    }
    stubx_drive_set_rc = -1;                    /* 模式非法 */
    host_dl_tx_clear();
    pl[0] = 9; put16(&pl[1], 0); put16(&pl[3], 0);
    build(f, 0x1C, 96, pl, 5);
    feed_and_poll(f, 12);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 96, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x03);         /* ST_BAD_PARAM */
    }
    stubx_drive_set_rc = 0;
    host_dl_tx_clear();
    build(f, 0x1C, 98, pl, 2);                 /* 长度不足 */
    feed_and_poll(f, 9);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 98, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x03);
    }
    host_dl_tx_clear();
    build(f, 0x1D, 100, NULL, 0);              /* 纯查询 */
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x1D, 100, &l);
        CHECK(r != NULL);
        if (r) CHECK_EQ_I(l, 12);
    }
    host_dl_tx_clear();
    pl[0] = 0x07;                              /* CMD_CLEAR_FAULT */
    build(f, 0x20, 102, pl, 1);
    feed_and_poll(f, 8);
    {
        uint8_t l = 0;
        const uint8_t *a = find_frame(0x21, 102, &l);
        CHECK(a != NULL);
        if (a) CHECK_EQ_I(a[6], 0x00);
        CHECK_EQ_I(stubx_clear_faults, 1);
    }

    /* 23) 标定原文回读：82 字节，一字不改地转发 trace_cal_data_t */
    host_dl_tx_clear();
    build(f, 0x14, 104, NULL, 0);
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x15, 104, &l);
        CHECK(r != NULL);
        CHECK_EQ_I(l, 82);
        if (r) {
            CHECK_EQ_I(r[5], 1);                       /* valid */
            CHECK_EQ_I(r[5u + 1u], 1);                 /* pol[0] */
            CHECK_EQ_I(r[5u + 18u] | (r[5u + 19u] << 8), 100);   /* blk[0] @18 */
            CHECK_EQ_I(r[5u + 34u] | (r[5u + 35u] << 8), 200);   /* wht[0] @34 */
            CHECK_EQ_I(r[5u + 50u] | (r[5u + 51u] << 8), 300);   /* thr[0] @50 */
            CHECK_EQ_I(r[5u + 66u] | (r[5u + 67u] << 8), 0);     /* jit[0] @66 */
        }
    }

    return T_SUMMARY();
}
