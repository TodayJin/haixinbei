/**
 * @file  oled_glue.c
 * @brief KK_OLED 异步刷新与 HAL 回调的接线
 *
 * kk-oled-port 契约：应用只把属于本显示实例（I2C1）的完成/错误事件转发给驱动，
 * 不在库里定义会接管整个 SDK 的全局弱回调。
 */
#include "board.h"
#include "kk_oled_driver.h"

/** I2C Memory Write（命令或数据）完成一次。 */
volatile uint32_t g_oled_tx_done;   /* 诊断：完成回调次数 */
volatile uint32_t g_oled_tx_err;    /* 诊断：错误回调次数 */

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        g_oled_tx_done++;
        OLED_DriverHandleMemTxComplete();
    }
}

/** I2C 出错：让驱动收尾并上报，核心会走恢复流程。 */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        g_oled_tx_err++;
        OLED_DriverHandleError();
    }
}
