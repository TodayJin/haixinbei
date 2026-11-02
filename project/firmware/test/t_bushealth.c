/**
 * @file  t_bushealth.c   （主机侧单测：I2C 总线健康看门狗）
 *
 * 被测的是固件里**真的** App/Svc/bus_health.c（纯逻辑、不碰 HAL）。
 * 覆盖：连续失败计数 / 攒够才请求恢复 / 空闲才让恢复 / 两次恢复最小间隔 /
 *       成功一次就清零 / 恢复次数只增。
 *
 * 为什么值得单测：这段逻辑的失效模式是"要么永远不恢复（屏黑着不亮）、
 * 要么没插灰度模块时每帧都去刷总线（恢复一次阻塞 12ms）"，两者都要靠时序断言。
 */
#include "t_common.h"
#include "bus_health.h"

int main(void)
{
    /* 1. 初值：什么都没有 */
    bus_health_init();
    CHECK_EQ_I(bus_health_pending(), 0);
    CHECK_EQ_I(bus_health_count(), 0);
    CHECK_EQ_I(bus_health_fail_streak(), 0);

    /* 2. 连失败 4 次还不够（阈值 5） */
    for (int i = 0; i < 4; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_fail_streak(), 4);
    CHECK_EQ_I(bus_health_pending(), 0);

    /* 3. 第 5 次失败 -> 请求恢复，连败计数归零（下一个阈值重新数） */
    bus_health_report(0);
    CHECK_EQ_I(bus_health_pending(), 1);
    CHECK_EQ_I(bus_health_fail_streak(), 0);

    /* 4. 总线忙的时候不能恢复（屏正在搬 DMA，插进去会花屏） */
    CHECK_EQ_I(bus_health_take(1000U, 0U), 0);
    CHECK_EQ_I(bus_health_pending(), 1);        /* 请求还在，等下一圈 */
    CHECK_EQ_I(bus_health_count(), 0);

    /* 5. 空闲了 -> 放行一次，计数 +1，请求清掉 */
    CHECK_EQ_I(bus_health_take(1000U, 1U), 1);
    CHECK_EQ_I(bus_health_pending(), 0);
    CHECK_EQ_I(bus_health_count(), 1);
    CHECK_EQ_I(bus_health_take(1000U, 1U), 0);  /* 已经没请求了 */

    /* 6. 两次恢复之间至少 1000ms：再攒够一次也不许 1000ms 内再恢复 */
    for (int i = 0; i < 5; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_pending(), 1);
    CHECK_EQ_I(bus_health_take(1500U, 1U), 0);  /* 才过 500ms */
    CHECK_EQ_I(bus_health_pending(), 1);        /* 请求没被吃掉 */
    CHECK_EQ_I(bus_health_count(), 1);
    CHECK_EQ_I(bus_health_take(2000U, 1U), 1);  /* 整 1000ms，放行 */
    CHECK_EQ_I(bus_health_count(), 2);

    /* 7. 成功一次立刻把连败清零（偶尔的 NACK 不该攒成"总线死了"） */
    bus_health_init();
    bus_health_report(0);
    bus_health_report(0);
    bus_health_report(0);
    CHECK_EQ_I(bus_health_fail_streak(), 3);
    bus_health_report(1);
    CHECK_EQ_I(bus_health_fail_streak(), 0);
    for (int i = 0; i < 4; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_pending(), 0);        /* 攒不到 5 次 */

    /* 8. 时间戳回绕（32 位无符号减法）：跨 0 点的间隔要算对 */
    bus_health_init();
    for (int i = 0; i < 5; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_take(0xFFFFFF00U, 1U), 1);   /* 首次（s_last_ms=0 -> 距离巨大） */
    for (int i = 0; i < 5; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_take(0xFFFFFF10U, 1U), 0);   /* 只过 16ms */
    for (int i = 0; i < 5; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_take(0x00000000U, 1U), 0);   /* 回绕后 240ms，仍不够 */
    for (int i = 0; i < 5; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_take(0x00000400U, 1U), 1);   /* 距上次 0x4F0 = 1264ms，放行 */
    CHECK_EQ_I(bus_health_count(), 2);

    /* 9. 长时间一直失败、但没给空闲时机：只会攒一个请求，不会无限长大 */
    bus_health_init();
    for (int i = 0; i < 200; i++) bus_health_report(0);
    CHECK_EQ_I(bus_health_pending(), 1);
    CHECK_EQ_I(bus_health_fail_streak(), 0);    /* 每次到阈值就归零 */
    CHECK_EQ_I(bus_health_take(5000U, 0U), 0);
    CHECK_EQ_I(bus_health_take(5000U, 1U), 1);
    CHECK_EQ_I(bus_health_count(), 1);

    return T_SUMMARY();
}
