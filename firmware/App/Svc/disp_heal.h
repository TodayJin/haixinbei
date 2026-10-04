/**
 * @file  disp_heal.h
 * @brief 屏幕故障自愈的决策逻辑（纯逻辑，不碰 HAL，主机侧能单测）
 *
 * 背景（用户 m26333：「车上的这个屏幕有时候就会变得特别卡，帧率特别低」）：
 * `KK_UI_Update()` 一旦 latch 了显示错误（OLED 传输报错，或一次传输拖过
 * `KK_UI_DISPLAY_TIMEOUT_MS`=250ms），kk_ui.c:731 之后**每一拍都直接 return** ——
 * 不画、不收按键。而它的恢复入口 `KK_UI_RecoverDisplay()` 在原工程里
 * **没有任何调用者**（死代码）⇒ 一次传输错误 = 屏永久冻住，只能断电。
 *
 * 这里只做"什么时候该去自愈"的决策，动作（调 KK_UI_RecoverDisplay /
 * board_i2c_bus_recover）留给 app_ui.c：
 *   ① 发现故障的那一拍立刻试一次（用户少看一会儿"卡住"）；
 *   ② 之后每 DISP_HEAL_RETRY_MS 试一次；
 *   ③ 同一轮里连续 DISP_HEAL_BUS_AFTER 次重试还不成 ⇒ 顺手复位一次 I2C 总线
 *      （速率 + DMA 一起回来，免得"屏能画了但总线还歪着"时无限打转）；
 *   ④ 只要 `KK_UI_Update()` 恢复正常，这一轮就结束、计数归零重来。
 */
#ifndef DISP_HEAL_H
#define DISP_HEAL_H

#include <stdint.h>

#define DISP_HEAL_RETRY_MS   300U   /* 两次自愈尝试之间至少隔这么久 */
#define DISP_HEAL_BUS_AFTER  3U     /* 同一轮里**重试**到第几次时顺手复位总线
                                       （发现故障的那一拍不算重试 => 大约 0.9s 后） */

/** 每拍要执行的动作（都是 0/1 标志，调用者自己决定做不做得了）。 */
typedef struct {
    uint8_t try_recover;   /* 1 = 现在调 KK_UI_RecoverDisplay() */
    uint8_t bus_recover;   /* 1 = 现在顺手调 board_i2c_bus_recover() */
} disp_heal_action_t;

/** 自愈状态（调用者持有，别放静态量：主机测试要能建多份）。 */
typedef struct {
    uint8_t  active;     /* 1 = 这一轮故障还没清 */
    uint8_t  retry;      /* 这一轮已经试了几次 */
    uint32_t last_ms;    /* 上一次尝试的时刻（只在 active 时有意义） */
    uint32_t count;      /* 上电以来"真的自愈成功"了几次（诊断用） */
} disp_heal_t;

void disp_heal_init(disp_heal_t *h);

/**
 * 每拍喂一次 `KK_UI_Update()` 的结果。
 * @param display_error  1 = 这次返回的是 KK_UI_DISPLAY_ERROR/TIMEOUT，0 = 其它
 * @param now_ms         HAL_GetTick()
 * @return 这一拍要不要去自愈、要不要顺手复位总线
 */
disp_heal_action_t disp_heal_step(disp_heal_t *h, uint8_t display_error, uint32_t now_ms);

/** 调用者真的调成了 KK_UI_RecoverDisplay()（返回 KK_UI_OK）时通知一次：计数 +1。 */
void disp_heal_recovered(disp_heal_t *h);

/** 上电以来成功自愈次数。 */
uint32_t disp_heal_count(const disp_heal_t *h);

#endif /* DISP_HEAL_H */
