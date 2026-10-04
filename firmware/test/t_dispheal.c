/**
 * @file  t_dispheal.c   （主机侧单测：屏幕故障自愈的决策逻辑）
 *
 * 被测的是固件里**真的** App/Svc/disp_heal.c（纯逻辑、不碰 HAL / kk_ui）。
 * 覆盖：好的一拍不动 / 发现故障立刻试第一次 / 300ms 节拍 / 重试到阈值顺手复位总线 /
 *       恢复正常就结束这一轮 / 时间戳回绕 / 成功计数只增。
 *
 * 为什么值得单测：这段逻辑的失效模式是"要么一次也不去自愈（屏永久冻住，只能断电 ——
 * 用户 m26333 看到的卡死），要么每 2ms 就去戳一次屏 / 反复复位 I2C 把总线搅乱"，
 * 两种都只能靠时序断言钉住。
 */
#include "t_common.h"
#include "disp_heal.h"

int main(void)
{
    disp_heal_t h;
    disp_heal_action_t a;

    /* 1. 初值：什么都没有 */
    disp_heal_init(&h);
    CHECK_EQ_I(disp_heal_count(&h), 0);
    a = disp_heal_step(&h, 0U, 1000U);
    CHECK_EQ_I(a.try_recover, 0);
    CHECK_EQ_I(a.bus_recover, 0);
    CHECK_EQ_I(h.active, 0);

    /* 2. 发现故障的那一拍立刻试第一次（不让用户多等 300ms） */
    a = disp_heal_step(&h, 1U, 1000U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 0);
    CHECK_EQ_I(h.active, 1);
    CHECK_EQ_I(h.retry, 0);      /* 发现的那一拍不算重试 */

    /* 3. 300ms 以内不再动（主循环 2ms 一拍，别每拍都去戳屏） */
    CHECK_EQ_I(disp_heal_step(&h, 1U, 1001U).try_recover, 0);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 1200U).try_recover, 0);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 1299U).try_recover, 0);
    CHECK_EQ_I(h.retry, 0);

    /* 4. 满 300ms：重试第 1 次（还不够阈值，只自愈、不复位总线） */
    a = disp_heal_step(&h, 1U, 1300U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 0);
    CHECK_EQ_I(h.retry, 1);

    /* 5. 再两拍：重试第 2、3 次；第 3 次重试（≈发现故障后 0.9s）顺手复位总线 */
    CHECK_EQ_I(disp_heal_step(&h, 1U, 1600U).try_recover, 1);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 1600U).try_recover, 0);   /* 同一毫秒不重复 */
    CHECK_EQ_I(h.retry, 2);
    a = disp_heal_step(&h, 1U, 1900U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 1);
    CHECK_EQ_I(h.retry, 0);      /* 复位过总线，重试计数从头数 */

    /* 6. 复位后还没好：接着 300ms 一拍，不会再连续复位 */
    a = disp_heal_step(&h, 1U, 2200U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 0);
    CHECK_EQ_I(h.retry, 1);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 2300U).try_recover, 0);
    a = disp_heal_step(&h, 1U, 2500U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 0);

    /* 8. 自愈真的成功了：调用者通报一次，计数 +1（这一轮还没结束：下一条消息才算好） */
    disp_heal_recovered(&h);
    CHECK_EQ_I(disp_heal_count(&h), 1);

    /* 9. 好的一拍 => 这一轮结束：重试计数清掉、状态回到"没故障" */
    a = disp_heal_step(&h, 0U, 2600U);
    CHECK_EQ_I(a.try_recover, 0);
    CHECK_EQ_I(h.active, 0);
    CHECK_EQ_I(h.retry, 0);
    CHECK_EQ_I(disp_heal_count(&h), 1);      /* 计数是"上电以来"，不清 */

    /* 10. 下一次故障又是立刻试第一次（不用等 300ms 的旧时间戳） */
    a = disp_heal_step(&h, 1U, 3000U);
    CHECK_EQ_I(a.try_recover, 1);
    CHECK_EQ_I(a.bus_recover, 0);
    disp_heal_recovered(&h);
    CHECK_EQ_I(disp_heal_count(&h), 2);

    /* 11. 时间戳回绕（32 位无符号减法）：跨 0 点的间隔要算对 */
    disp_heal_init(&h);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 0xFFFFFF00U).try_recover, 1);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 0xFFFFFF10U).try_recover, 0);  /* 只过 16ms */
    CHECK_EQ_I(disp_heal_step(&h, 1U, 0x00000000U).try_recover, 0);  /* 回绕后 240ms，仍不够 */
    CHECK_EQ_I(disp_heal_step(&h, 1U, 0x00000040U).try_recover, 1);  /* 距上次 0x140 = 320ms */

    /* 12. 一直不好、无限重试：不会溢出，也不会每拍都动手 */
    disp_heal_init(&h);
    CHECK_EQ_I(disp_heal_step(&h, 1U, 0U).try_recover, 1);   /* now_ms=0 也能发现故障 */
    for (uint32_t t = 1U; t < 200000U; t++) {
        a = disp_heal_step(&h, 1U, t);
        CHECK_EQ_I(a.try_recover, ((t % 300U) == 0U) ? 1 : 0);
    }

    return T_SUMMARY();
}
