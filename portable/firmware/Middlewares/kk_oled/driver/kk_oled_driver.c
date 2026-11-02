#include "kk_oled_driver.h"

#include "board.h"
#include "kk_oled_internal.h"

#include <string.h>

/* ============================================================================
 * 本文件是"固定硬件适配边界"：STM32G474 HAL、I2C1 与 SSD1315 参数都集中在这里。
 *
 * 与上游参考实现（CH1116）的差异：
 *   - 控制器：CH1116 -> SSD1315（命令集与 SSD1306 兼容）
 *   - I2C 从地址：0x3C（本板 A0=SA0 接 GND）
 *   - 列偏移：0（SSD1315 的 128x64 显存就是 128 列，无 SH1106 那种偏移）
 *   - 初始化序列取自屏厂 GME12864-41 手册，已在本板实测点亮
 *     ★ 8D 14 = 开内部电荷泵（本板只有 3.3V，必须开）
 *     ★ AD 30 = 内部 IREF（所以 IREF 脚 26 悬空是对的）
 * ============================================================================ */

/** HAL 使用左移一位后的 8 位形式设备地址。 */
#define OLED_I2C_ADDRESS ((uint16_t)(0x3CU << 1U))
/** I2C Memory Address 字节：后续内容为命令。 */
#define OLED_CONTROL_COMMAND 0x00U
/** I2C Memory Address 字节：后续内容为显示数据。 */
#define OLED_CONTROL_DATA 0x40U
/** 本板可见第 0 列相对控制器内部显存的列偏移（无偏移）。 */
#define OLED_COLUMN_OFFSET 0U
/** 所有阻塞 I2C 调用的最长等待时间。 */
#define OLED_BLOCKING_TIMEOUT_MS 100U

typedef enum {
    OLED_DRIVER_MODE_NONE = 0,
    OLED_DRIVER_MODE_IT,
    OLED_DRIVER_MODE_DMA
} OLED_DriverMode;

typedef enum {
    OLED_DRIVER_PHASE_COMMAND = 0,
    OLED_DRIVER_PHASE_DATA
} OLED_DriverPhase;

static volatile bool oled_driver_busy;
static volatile uint8_t oled_driver_page;
static volatile OLED_DriverPhase oled_driver_phase;
static volatile OLED_DriverMode oled_driver_mode;
static uint8_t oled_driver_command[3];

static OLED_Status oled_hal_status(HAL_StatusTypeDef status)
{
    if (status == HAL_OK) {
        return OLED_OK;
    }
    if (status == HAL_BUSY) {
        return OLED_BUSY;
    }
    return OLED_ERROR;
}

/** 通过控制字节 0x00 阻塞发送一组命令。 */
static HAL_StatusTypeDef oled_send_command_blocking(const uint8_t *command,
                                                     uint16_t length)
{
    return HAL_I2C_Mem_Write(board_i2c(), OLED_I2C_ADDRESS, OLED_CONTROL_COMMAND,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)command, length,
                             OLED_BLOCKING_TIMEOUT_MS);
}

/** 通过控制字节 0x40 阻塞发送连续显存数据。 */
static HAL_StatusTypeDef oled_send_data_blocking(const uint8_t *data,
                                                  uint16_t length)
{
    return HAL_I2C_Mem_Write(board_i2c(), OLED_I2C_ADDRESS, OLED_CONTROL_DATA,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)data, length,
                             OLED_BLOCKING_TIMEOUT_MS);
}

/** 按当前页的首个差异列生成三字节页寻址命令（0xB0|page, 列低, 0x10|列高）。 */
static void oled_prepare_page_command(uint8_t page)
{
    uint8_t visible_column = (uint8_t)(OLED_InternalGetTransferMinX(page) +
                                       OLED_COLUMN_OFFSET);

    oled_driver_command[0] = (uint8_t)(0xB0U | page);
    oled_driver_command[1] = (uint8_t)(visible_column & 0x0FU);
    oled_driver_command[2] = (uint8_t)(0x10U | (visible_column >> 4U));
}

static bool oled_find_next_page(uint8_t first, uint8_t *page)
{
    uint8_t candidate;

    for (candidate = first; candidate < OLED_PHYSICAL_PAGES; ++candidate) {
        if (OLED_InternalGetTransferMinX(candidate) < OLED_PHYSICAL_WIDTH) {
            *page = candidate;
            return true;
        }
    }
    return false;
}

/**
 * 等待上电稳定、发送 SSD1315 初始化序列、逐页清零后再点亮。
 * 初始化阶段故意用阻塞调用，保证返回时屏幕处于确定状态。
 */
OLED_Status OLED_DriverInit(void)
{
    /* 屏厂 GME12864-41 手册的初始化序列；最后一条 AF（开显示）在清屏之后单独发。
     * 页寻址模式 20 02，所以后面每页都要重发 B0|page + 列地址。 */
    static const uint8_t init_commands[] = {
        0xAEU,          /* 关显示 */
        0x81U, 0xCFU,   /* 对比度 0xCF */
        0xA4U,          /* 按显存显示 */
        0xA6U,          /* 正常显示（非反白） */
        0x00U, 0x10U,   /* 列地址低/高 4 位归零 */
        0x20U, 0x02U,   /* ★ 寻址模式 = 页寻址 */
        0xB0U,          /* 起始页 0 */
        0x40U,          /* 显示起始行 0 */
        0xA0U,          /* 列地址映射：左->右 */
        0xA8U, 0x3FU,   /* multiplex = 63（1/64 duty） */
        0xC0U,          /* COM 扫描方向 */
        0xD3U, 0x00U,   /* COM 引脚配置 */
        0xDAU, 0x12U,   /* COM 引脚：交替 + 不左右反相 */
        0xD5U, 0x80U,   /* 时钟分频 */
        0xD9U, 0x22U,   /* 预充电周期 */
        0xDBU, 0x40U,   /* VCOMH 反压 */
        0x8DU, 0x14U,   /* ★ 启用内部电荷泵（VCC = 7.5V） */
        0xADU, 0x30U    /* ★ 内部 IREF，ISEG = 30uA */
    };
    static const uint8_t zeros[OLED_PHYSICAL_WIDTH] = {0};
    uint8_t page;

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    oled_driver_busy = true;
    oled_driver_mode = OLED_DRIVER_MODE_NONE;
    HAL_Delay(20U);

    if (oled_send_command_blocking(init_commands, sizeof(init_commands)) != HAL_OK) {
        oled_driver_busy = false;
        return OLED_ERROR;
    }
    for (page = 0U; page < OLED_PHYSICAL_PAGES; ++page) {
        uint8_t command[3] = {
            (uint8_t)(0xB0U | page),
            (uint8_t)(OLED_COLUMN_OFFSET & 0x0FU),
            (uint8_t)(0x10U | (OLED_COLUMN_OFFSET >> 4U))
        };
        if (oled_send_command_blocking(command, sizeof(command)) != HAL_OK ||
            oled_send_data_blocking(zeros, sizeof(zeros)) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    {
        const uint8_t display_on = 0xAFU; /* 全部页清零成功后才开显示 */
        if (oled_send_command_blocking(&display_on, 1U) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    oled_driver_busy = false;
    return OLED_OK;
}

OLED_Status OLED_DriverWriteBlocking(void)
{
    const uint8_t *buffer = OLED_InternalGetTransferBuffer();
    uint8_t page;

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    oled_driver_busy = true;
    oled_driver_mode = OLED_DRIVER_MODE_NONE;
    for (page = 0U; page < OLED_PHYSICAL_PAGES; ++page) {
        uint8_t min_x = OLED_InternalGetTransferMinX(page);
        uint8_t max_x = OLED_InternalGetTransferMaxX(page);
        uint16_t length;

        if (min_x >= OLED_PHYSICAL_WIDTH) {
            continue;
        }
        oled_prepare_page_command(page);
        length = (uint16_t)max_x - min_x + 1U;
        if (oled_send_command_blocking(oled_driver_command,
                                       sizeof(oled_driver_command)) != HAL_OK ||
            oled_send_data_blocking(buffer + (uint16_t)page * OLED_PHYSICAL_WIDTH + min_x,
                                    length) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    oled_driver_busy = false;
    return OLED_OK;
}

static HAL_StatusTypeDef oled_start_async_command(void)
{
    oled_prepare_page_command(oled_driver_page);
    oled_driver_phase = OLED_DRIVER_PHASE_COMMAND;
    return HAL_I2C_Mem_Write_IT(board_i2c(), OLED_I2C_ADDRESS, OLED_CONTROL_COMMAND,
                                I2C_MEMADD_SIZE_8BIT, oled_driver_command,
                                sizeof(oled_driver_command));
}

static OLED_Status oled_start_async(OLED_DriverMode mode)
{
    HAL_StatusTypeDef hal_status;
    uint8_t first_page;

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    if (!oled_find_next_page(0U, &first_page)) {
        return OLED_OK;
    }
    oled_driver_busy = true;
    oled_driver_page = first_page;
    oled_driver_mode = mode;
    hal_status = oled_start_async_command();
    if (hal_status != HAL_OK) {
        oled_driver_busy = false;
        oled_driver_mode = OLED_DRIVER_MODE_NONE;
        return oled_hal_status(hal_status);
    }
    return OLED_OK;
}

OLED_Status OLED_DriverWriteIT(void)
{
    return oled_start_async(OLED_DRIVER_MODE_IT);
}

OLED_Status OLED_DriverWriteDMA(void)
{
    return oled_start_async(OLED_DRIVER_MODE_DMA);
}

bool OLED_DriverIsBusy(void)
{
    return oled_driver_busy;
}

OLED_Status OLED_DriverSetContrast(uint8_t value)
{
    const uint8_t command[2] = {0x81U, value};

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    return oled_hal_status(oled_send_command_blocking(command, sizeof(command)));
}

OLED_Status OLED_DriverSetPowerSave(bool enable)
{
    uint8_t command = enable ? 0xAEU : 0xAFU;

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    return oled_hal_status(oled_send_command_blocking(&command, 1U));
}

/* ---- 异步状态机：由应用把 HAL 的 MemTx 完成/错误回调转发进来 ---- */
void OLED_DriverHandleMemTxComplete(void)
{
    if (!oled_driver_busy) {
        return;
    }
    if (oled_driver_phase == OLED_DRIVER_PHASE_COMMAND) {
        const uint8_t *buffer = OLED_InternalGetTransferBuffer();
        uint8_t min_x = OLED_InternalGetTransferMinX(oled_driver_page);
        uint8_t max_x = OLED_InternalGetTransferMaxX(oled_driver_page);
        uint16_t length = (uint16_t)max_x - min_x + 1U;

        oled_driver_phase = OLED_DRIVER_PHASE_DATA;
        {
            /* 数据阶段按当前异步模式选 IT 或 DMA（页命令始终走 IT） */
            HAL_StatusTypeDef st;
            uint8_t *src = (uint8_t *)(buffer +
                             (uint16_t)oled_driver_page * OLED_PHYSICAL_WIDTH + min_x);

            if (oled_driver_mode == OLED_DRIVER_MODE_DMA) {
                st = HAL_I2C_Mem_Write_DMA(board_i2c(), OLED_I2C_ADDRESS, OLED_CONTROL_DATA,
                                           I2C_MEMADD_SIZE_8BIT, src, length);
            } else {
                st = HAL_I2C_Mem_Write_IT(board_i2c(), OLED_I2C_ADDRESS, OLED_CONTROL_DATA,
                                          I2C_MEMADD_SIZE_8BIT, src, length);
            }
            if (st != HAL_OK) {
                oled_driver_busy = false;
                oled_driver_mode = OLED_DRIVER_MODE_NONE;
                OLED_InternalTransferFinished(OLED_ERROR);
            }
        }
        return;
    }

    /* 一页数据发完：找下一页 */
    {
        uint8_t next_page;
        if (!oled_find_next_page((uint8_t)(oled_driver_page + 1U), &next_page)) {
            oled_driver_busy = false;
            oled_driver_mode = OLED_DRIVER_MODE_NONE;
            OLED_InternalTransferFinished(OLED_OK);
            return;
        }
        oled_driver_page = next_page;
        if (oled_start_async_command() != HAL_OK) {
            oled_driver_busy = false;
            oled_driver_mode = OLED_DRIVER_MODE_NONE;
            OLED_InternalTransferFinished(OLED_ERROR);
        }
    }
}

void OLED_DriverHandleError(void)
{
    if (!oled_driver_busy) {
        return;
    }
    oled_driver_busy = false;
    oled_driver_mode = OLED_DRIVER_MODE_NONE;
    OLED_InternalTransferFinished(OLED_ERROR);
}
