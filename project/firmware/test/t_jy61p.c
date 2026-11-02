/**
 * @file  t_jy61p.c   主机侧单元测试：JY61P 角度/角速度帧解析（App/Svc/jy61p.c）
 *
 * 为什么值得测：这一份解析是所有控制环的角度来源，帧错一位 = 车按错的角度转。
 * 用例覆盖：正常帧、坏校验、逐字节拆包、垃圾流里重同步、以及随机字节流 fuzz
 * （UBSan trap 模式会把任何越界/溢出直接打死 ⇒ 纯逻辑错误立刻暴露）。
 */
#include <stdint.h>
#include <string.h>
#include "jy61p.h"
#include "t_common.h"

static void feed(const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) jy61p_ReceiveData(d[i]);
}

/* 按 JY61P 规则造一帧：55 | type | 8 字节数据 | 前 10 字节和的低字节 */
static void make_frame(uint8_t *f, uint8_t type, const uint8_t *data8)
{
    uint8_t sum = 0;
    f[0] = 0x55;
    f[1] = type;
    memcpy(&f[2], data8, 8);
    for (int i = 0; i < 10; i++) sum = (uint8_t)(sum + f[i]);
    f[10] = sum;
}

int main(void)
{
    uint8_t f[11], d[8];
    float yaw0;

    /* 1) 角度帧 0x53：yaw = raw/32768*180 */
    memset(d, 0, sizeof(d));
    d[1] = 0x10;   /* roll  = 0x1000 = 4096  -> 22.5° */
    d[3] = 0x20;   /* pitch = 0x2000 = 8192  -> 45°   */
    d[5] = 0x7F; d[4] = 0xFF;  /* yaw = 0x7FFF = 32767 -> ≈179.994° */
    make_frame(f, 0x53, d);
    feed(f, 11);
    CHECK(Roll > 22.49f && Roll < 22.51f);
    CHECK(Pitch > 44.99f && Pitch < 45.01f);
    CHECK(Yaw > 179.98f && Yaw <= 180.0f);

    /* 2) 角速度帧 0x52：wz 在 byte6/7，量程 ±2000°/s */
    memset(d, 0, sizeof(d));
    d[5] = 0x10; d[4] = 0x00;  /* 0x1000 = 4096 -> 250°/s */
    make_frame(f, 0x52, d);
    Gyro_Z = 0.0f;
    feed(f, 11);
    CHECK(Gyro_Z > 249.9f && Gyro_Z < 250.1f);

    /* 3) 负值（补码）也要对：0xF000 = -4096 -> -250°/s */
    d[5] = 0xF0; d[4] = 0x00;
    make_frame(f, 0x52, d);
    Gyro_Z = 0.0f;
    feed(f, 11);
    CHECK(Gyro_Z > -250.1f && Gyro_Z < -249.9f);

    /* 4) 坏校验：一个字都不能动 */
    make_frame(f, 0x53, d);
    f[10] ^= 0xFF;
    yaw0 = Yaw;
    feed(f, 11);
    CHECK(Yaw == yaw0);

    /* 5) 逐字节拆包（两次喂之间的字节数任意），值要对 */
    memset(d, 0, sizeof(d));
    d[5] = 0x40; d[4] = 0x00;  /* yaw = 0x4000 = 16384 -> 90° */
    make_frame(f, 0x53, d);
    for (int i = 0; i < 11; i++) {
        jy61p_ReceiveData(f[i]);
        if (i == 4 || i == 8) { /* 中途插一点垃圾，状态机应能活下来 */ }
    }
    CHECK(Yaw > 89.99f && Yaw < 90.01f);

    /* 6) 垃圾流里重同步：前面塞 200 个随机字节，最后一帧必须解析出来 */
    {
        uint32_t seed = 12345u;
        uint8_t junk[256];
        for (int i = 0; i < 200; i++) {
            seed = seed * 1103515245u + 12345u;
            junk[i] = (uint8_t)(seed >> 16);
        }
        feed(junk, 200);
        d[5] = 0x40; d[4] = 0x00;
        make_frame(f, 0x53, d);
        Yaw = -1.0f;
        feed(f, 11);
        CHECK(Yaw > 89.99f && Yaw < 90.01f);
    }

    /* 7) 非 0x52/0x53 的类型不该污染状态：喂一帧"未知类型"，紧接着喂真帧 */
    {
        uint8_t g[11];
        make_frame(g, 0x51, d);          /* 0x51 = 加速度，未使用 */
        feed(g, 11);
        d[5] = 0x40; d[4] = 0x00;
        make_frame(f, 0x53, d);
        Yaw = -1.0f;
        feed(f, 11);
        CHECK(Yaw > 89.99f && Yaw < 90.01f);
    }

    /* 8) fuzz：30 万随机字节，只要求"不崩、不越界"（UBSan trap 会直接打死进程） */
    {
        uint32_t seed = 987654321u;
        for (int i = 0; i < 300000; i++) {
            seed = seed * 1103515245u + 12345u;
            jy61p_ReceiveData((uint8_t)(seed >> 19));
        }
        t_checks++;
    }

    /* 9) fuzz 之后解析仍然正常（状态机能回到 Idle） */
    {
        uint8_t tail[32];
        d[5] = 0x40; d[4] = 0x00;
        make_frame(f, 0x53, d);
        memset(tail, 0, sizeof(tail));
        feed(tail, 32);
        Yaw = -1.0f;
        feed(f, 11);
        CHECK(Yaw > 89.99f && Yaw < 90.01f);
    }

    return T_SUMMARY();
}
