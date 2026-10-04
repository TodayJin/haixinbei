/**
 * @file  disp_heal.c
 * @brief 屏幕故障自愈的决策逻辑（见 disp_heal.h 的来龙去脉）
 */
#include "disp_heal.h"

void disp_heal_init(disp_heal_t *h)
{
    if (h == 0) return;
    h->active  = 0U;
    h->retry   = 0U;
    h->last_ms = 0U;
    h->count   = 0U;
}

disp_heal_action_t disp_heal_step(disp_heal_t *h, uint8_t display_error, uint32_t now_ms)
{
    disp_heal_action_t a = { 0U, 0U };

    if (h == 0) return a;

    if (!display_error) {
        /* 这一拍 KK_UI_Update() 是好的：不管之前试到哪一步，这一轮都结束了。
           count 不清 —— 它是"上电以来自愈成功了几次"的诊断量。 */
        h->active = 0U;
        h->retry  = 0U;
        return a;
    }

    if (h->active) {
        /* 同一轮里：没到间隔就不动（300ms 一拍，别每 2ms 去戳一次屏） */
        if ((uint32_t)(now_ms - h->last_ms) < DISP_HEAL_RETRY_MS) return a;
        if (h->retry < 255U) h->retry++;
    } else {
        /* 刚发现故障：这一拍就试第一次，让用户少看一会儿"卡住" */
        h->active = 1U;
        h->retry  = 0U;
    }

    h->last_ms = now_ms;

    /* 连续试了 DISP_HEAL_BUS_AFTER 次还不成 ⇒ 顺手复位一次 I2C 总线
       （速率和 DMA 一起回来），然后重新从一开始数。 */
    if (h->retry >= DISP_HEAL_BUS_AFTER) {
        a.bus_recover = 1U;
        h->retry      = 0U;
    }

    a.try_recover = 1U;
    return a;
}

void disp_heal_recovered(disp_heal_t *h)
{
    if (h == 0) return;
    h->count++;
}

uint32_t disp_heal_count(const disp_heal_t *h)
{
    return (h == 0) ? 0U : h->count;
}
