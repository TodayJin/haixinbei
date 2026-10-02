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

int main(void)
{
    uint8_t f[64], pl[8];

    /* 0) 参考 CRC 与自己的实现自检（"123456789" = 0x29B1） */
    CHECK_EQ_I(ref_crc((const uint8_t *)"123456789", 9), 0x29B1);

    host_link_init();          /* 参数表回到默认值，统计清零 */
    host_dl_reset();

    /* 1) HELLO_REQ → HELLO 帧，带名字和参数表 CRC */
    memset(pl, 0, sizeof(pl));
    build(f, 0x41, 7, NULL, 0);
    feed_and_poll(f, 7);
    {
        uint8_t l = 0;
        const uint8_t *r = find_frame(0x40, 7, &l);
        CHECK(r != NULL);
        CHECK_EQ_I(l, 20);
        if (r) {
            CHECK_EQ_I(r[5], 0x01);                     /* fw version 1 */
            CHECK_EQ_I(memcmp(&r[13], "quandi-car", 10), 0);   /* 名字在 payload+8 */
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
        CHECK(after_one <= 8);                      /* 但绝不是一次性 31 帧 */
        for (int i = 0; i < 40; i++) host_link_poll(0u);   /* 把剩下的抽干 */
        CHECK_EQ_I(count_frames(0x12), 30);         /* 参数表 30 项，一项不少也不多 */
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

    return T_SUMMARY();
}
