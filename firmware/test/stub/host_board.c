/**
 * @file  host_board.c   （主机侧假板级外设 —— 只给 firmware/test 用）
 *
 * 被测模块（trace_cal.c / host_link.c / pid.c / tracing.c）对外部世界的全部依赖
 * 都集中在这里，并且都带"测试注入口"：
 *   host_trace_bits_set()   模拟 8 路灰度电平
 *   host_i2c_reg_set()      模拟灰度模块的寄存器读数
 *   host_uart4_feed()       模拟 UART4 收到的字节
 *   host_ina226_set()       模拟 INA226 电压/电流
 *   host_oled_busy_set()    模拟 OLED 忙
 *   host_dl_feed()/host_dl_tx()  上位机链路的收/发
 */
#include "stm32g4xx_hal.h"
#include "board.h"
#include "tracing.h"
#include "motor.h"
#include "host_link.h"
#include <stdbool.h>
#include <string.h>

/* ---------------- 灰度电平 ---------------- */
static uint8_t s_trace_bits;

GPIO_PinState bsp_trace_bit(uint8_t idx)
{
    uint8_t b = (s_trace_bits >> (idx & 7U)) & 1U;
    return b ? GPIO_PIN_SET : GPIO_PIN_RESET;
}
void host_trace_bits_set(uint8_t bits) { s_trace_bits = bits; }
uint8_t host_trace_bits_get(void) { return s_trace_bits; }

/* ---------------- I2C 假寄存器 ---------------- */
static uint8_t s_i2c_regs[256];
static int     s_i2c_fail;      /* >0 时 i2c_read_reg 一律返回 HAL_ERROR */

void host_i2c_reg_set(uint8_t reg, uint8_t value) { s_i2c_regs[reg] = value; }
void host_i2c_fail_set(int fail) { s_i2c_fail = fail; }

HAL_StatusTypeDef i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
    (void)addr7;
    if (buf == NULL) return HAL_ERROR;
    if (s_i2c_fail) return HAL_ERROR;
    for (uint16_t i = 0U; i < len; i++) buf[i] = s_i2c_regs[(uint8_t)(reg + i)];
    return HAL_OK;
}

HAL_StatusTypeDef i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    (void)addr7;
    if (buf == NULL) return HAL_ERROR;
    for (uint16_t i = 0U; i < len; i++) s_i2c_regs[(uint8_t)(reg + i)] = buf[i];
    return HAL_OK;
}

/* ---------------- 循迹模块的 UART4 ---------------- */
static uint8_t  s_u4_rx[512];
static uint16_t s_u4_rx_len, s_u4_rx_pos;
static uint8_t  s_u4_tx[512];
static uint16_t s_u4_tx_len;

void host_uart4_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len && s_u4_rx_len < sizeof(s_u4_rx); i++) s_u4_rx[s_u4_rx_len++] = data[i];
}
void host_uart4_reset(void) { s_u4_rx_len = s_u4_rx_pos = s_u4_tx_len = 0U; }
uint16_t host_uart4_tx_len(void) { return s_u4_tx_len; }
const uint8_t *host_uart4_tx(void) { return s_u4_tx; }

/* 假装模块会应答：固件发 0x57 + 地址 时，把这一段"回复"塞进接收缓冲。
 * 这样 trace_cal 的串口读取路径（发请求 -> 等 18/22 字节帧）在主机上也能测。
 * 注意 trace_cal 每次读之前会先冲刷残留字节，所以这里直接从头覆盖。 */
static const uint8_t *s_u4_reply;
static uint16_t       s_u4_reply_len;

/* >0 时每次应答给第 0 路加减这个值（正负交替），用来造"连接时模块在抖"的帧 */
int host_uart4_reply_wobble;
static int s_wobble_sign = 1;

void host_uart4_reply_set(const uint8_t *frame, uint16_t len)
{
    s_u4_reply     = frame;
    s_u4_reply_len = len;
}

void board_uart4_write(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len && s_u4_tx_len < sizeof(s_u4_tx); i++) s_u4_tx[s_u4_tx_len++] = data[i];

    if (data != NULL && len >= 2U && data[0] == 0x57U && s_u4_reply != NULL) {
        s_u4_rx_len = 0U;
        s_u4_rx_pos = 0U;
        for (uint16_t i = 0U; i < s_u4_reply_len && s_u4_rx_len < sizeof(s_u4_rx); i++) {
            s_u4_rx[s_u4_rx_len++] = s_u4_reply[i];
        }
        /* 造抖动：只改第 0 路（帧里偏移 1/2 两个字节） */
        if (host_uart4_reply_wobble != 0 && s_u4_rx_len >= 3U && s_u4_rx[0] == 0x75U) {
            int v = ((int)s_u4_rx[1] << 8) | (int)s_u4_rx[2];
            v += host_uart4_reply_wobble * s_wobble_sign;
            s_wobble_sign = -s_wobble_sign;
            s_u4_rx[1] = (uint8_t)((v >> 8) & 0xFF);
            s_u4_rx[2] = (uint8_t)(v & 0xFF);
        }
    }
}

int board_uart4_read_byte(uint8_t *b, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (b == NULL || s_u4_rx_pos >= s_u4_rx_len) return 0;
    *b = s_u4_rx[s_u4_rx_pos++];
    return 1;
}

/* ---------------- 上位机链路（LPUART1） ---------------- */
static uint8_t  s_dl_rx[4096];
static uint16_t s_dl_rx_len, s_dl_rx_pos;
static uint8_t  s_dl_tx[8192];
static uint16_t s_dl_tx_len;

void host_dl_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len && s_dl_rx_len < sizeof(s_dl_rx); i++) s_dl_rx[s_dl_rx_len++] = data[i];
}
void host_dl_reset(void) { s_dl_rx_len = s_dl_rx_pos = s_dl_tx_len = 0U; }
uint16_t host_dl_tx_len(void) { return s_dl_tx_len; }
const uint8_t *host_dl_tx(void) { return s_dl_tx; }
void host_dl_tx_clear(void) { s_dl_tx_len = 0U; }

int board_dl_get_byte(uint8_t *b)
{
    if (b == NULL || s_dl_rx_pos >= s_dl_rx_len) return 0;
    *b = s_dl_rx[s_dl_rx_pos++];
    return 1;
}

HAL_StatusTypeDef board_dl_send(const uint8_t *data, uint16_t len)
{
    if (data == NULL) return HAL_ERROR;
    for (uint16_t i = 0U; i < len && s_dl_tx_len < sizeof(s_dl_tx); i++) s_dl_tx[s_dl_tx_len++] = data[i];
    return HAL_OK;
}

void board_dl_start_rx(void) { }
HAL_StatusTypeDef board_dl_init(void) { return HAL_OK; }

/* ---------------- INA226 ---------------- */
static int32_t s_ina_mv = 12000, s_ina_ma = 0;

void host_ina226_set(int32_t mv, int32_t ma) { s_ina_mv = mv; s_ina_ma = ma; }

HAL_StatusTypeDef ina226_read_bus_voltage_mv(int32_t *mv) { if (mv) *mv = s_ina_mv; return HAL_OK; }
HAL_StatusTypeDef ina226_read_current_ma(int32_t *ma) { if (ma) *ma = s_ina_ma; return HAL_OK; }
HAL_StatusTypeDef ina226_read_shunt_uv(int32_t *uv) { if (uv) *uv = 0; return HAL_OK; }
HAL_StatusTypeDef ina226_read_power_mw(int32_t *mw) { if (mw) *mw = 0; return HAL_OK; }
HAL_StatusTypeDef ina226_read_id(uint16_t *mfg, uint16_t *die) { if (mfg) *mfg = 0x5449; if (die) *die = 0x2260; return HAL_OK; }
HAL_StatusTypeDef ina226_init(void) { return HAL_OK; }

/* ---------------- OLED 忙标志 ---------------- */
static bool s_oled_busy;
bool OLED_IsBusy(void) { return s_oled_busy; }
void host_oled_busy_set(int busy) { s_oled_busy = (busy != 0); }

/* ---------------- 电机/按键/周期任务 ---------------- */
static int s_load_left, s_load_right;
static uint8_t s_keys;

void Load(int moto1, int moto2) { s_load_left = moto1; s_load_right = moto2; }
int host_load_left(void) { return s_load_left; }
int host_load_right(void) { return s_load_right; }

uint8_t keys_raw(void) { return s_keys; }
void host_keys_set(uint8_t bits) { s_keys = bits; }

/* renwu.c 里的任务全局（host_link.c 会读它上报状态） */
uint8_t renwu_flag = 0u;
uint8_t black_num  = 0u;

void Servo_Task(void) { }
void bsp_periodic_start(void) { }

/* ---------------- I2C 总线看门狗（board.c 的真实现碰 HAL，这里只记数） ----------------
 * 被测模块（trace_cal.c）调 board_i2c_report()/board_i2c_watchdog() 时，
 * 测试可以从这里查"报了几次、空闲时要求了几次恢复"，
 * 用来验证"失败上报 -> 请求恢复"这条接线没断。
 * （决策逻辑本身由 tools/../App/Svc/bus_health.c 单独测，见 t_bushealth.c） */
static int s_i2c_report_ok, s_i2c_report_fail, s_i2c_wd_idle, s_i2c_wd_busy;

void host_i2c_stats_reset(void) { s_i2c_report_ok = s_i2c_report_fail = s_i2c_wd_idle = s_i2c_wd_busy = 0; }
int  host_i2c_report_ok(void)   { return s_i2c_report_ok; }
int  host_i2c_report_fail(void) { return s_i2c_report_fail; }
int  host_i2c_wd_idle(void)     { return s_i2c_wd_idle; }
int  host_i2c_wd_busy(void)     { return s_i2c_wd_busy; }

void board_i2c_report(uint8_t ok)
{
    if (ok) s_i2c_report_ok++;
    else    s_i2c_report_fail++;
}

uint8_t board_i2c_watchdog(uint8_t bus_idle)
{
    if (bus_idle) s_i2c_wd_idle++;
    else          s_i2c_wd_busy++;
    return 0U;
}

uint32_t board_i2c_recover_count(void) { return 0U; }
