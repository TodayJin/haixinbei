/**
 * @file  oled_diag.c
 * @brief OLED 通路诊断 v2 —— 定位"400kHz + DMA"这一组合
 *
 * v1 结论（人眼）：A 100K阻塞 ✅ / B 100K+DMA ✅ / C 400K阻塞 ✅ / D 400K+DMA ❌
 *   => 不是速率问题，也不是 DMA 本身，而是两者叠加。
 * v2 用来判定是"速度阈值"还是"中断+DMA 交接抢跑"：
 *   A: 400K + DMA  （复现，预期黑）
 *   B: 300K + DMA  （降速看是不是阈值）
 *   C: 200K + DMA  （更低）
 *   D: 400K + IT   （同样高速，但数据也走中断，没有 DMA 交接）
 */
#include "board.h"
#include "kk_oled.h"
#include "kk_font.h"
#include "SEGGER_RTT.h"
#include "oled_diag.h"

extern volatile uint32_t g_oled_tx_done;
extern volatile uint32_t g_oled_tx_err;

#define MODE_BLOCKING 0
#define MODE_DMA      1
#define MODE_IT       2

static void draw_phase(const char *label)
{
    OLED_Clear();
    OLED_SetDrawMode(OLED_DRAW_SET);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);

    OLED_SetFont(kk_font_home);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);
    OLED_DrawUTF8(2, 2, label);

    OLED_SetFont(kk_font_body);
    OLED_DrawUTF8(2, 20, "0123456789");

    OLED_DrawFrame(0, 34, 128, 28);
    OLED_DrawBox(3, 37, 34, 22);
    OLED_DrawDisc(96, 48, 11);
    OLED_DrawLine(40, 58, 88, 37);
}

static void run_phase(uint32_t hz, int mode, const char *label)
{
    OLED_Status st;
    uint32_t t0;
    uint32_t tx0 = g_oled_tx_done;

    (void)board_i2c_init_hz(hz);
    st = OLED_Init();
    SEGGER_RTT_printf(0, "PHASE %s: oled_init=%d\r\n", label, (int)st);

    draw_phase(label);
    if (mode == MODE_BLOCKING)      st = OLED_Update();
    else if (mode == MODE_DMA)      st = OLED_UpdateDMA();
    else                            st = OLED_UpdateIT();

    t0 = HAL_GetTick();
    while (OLED_IsBusy() && (HAL_GetTick() - t0) < 1500U) { }
    SEGGER_RTT_printf(0,
        "PHASE %s: run=%d done busy=%u last=%d tx_delta=%lu err=%lu busy_ms=%lu\r\n",
        label, (int)st, (unsigned)OLED_IsBusy(), (int)OLED_GetLastStatus(),
        (unsigned long)(g_oled_tx_done - tx0), (unsigned long)g_oled_tx_err,
        (unsigned long)(HAL_GetTick() - t0));

    HAL_Delay(5000);
}

void oled_diag_run(void)
{
    SEGGER_RTT_WriteString(0, "\r\n=== OLED 诊断 v2：400K+DMA 定位 ===\r\n");
    for (;;) {
        run_phase(400000U, MODE_DMA,      "A 400K DMA");
        run_phase(300000U, MODE_DMA,      "B 300K DMA");
        run_phase(200000U, MODE_DMA,      "C 200K DMA");
        run_phase(400000U, MODE_IT,       "D 400K IT ");
    }
}
