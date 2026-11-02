/**
 * @file  bus_health.c
 * @brief I2C 总线健康看门狗（实现，见 bus_health.h 的说明）
 */
#include "bus_health.h"

static uint8_t  s_fail;          /* 当前连续失败计数 */
static uint8_t  s_pending;       /* 1 = 已判定要恢复，等一个空闲时机 */
static uint32_t s_last_ms;       /* 上一次恢复的时刻 */
static uint32_t s_count;         /* 上电以来恢复次数 */

void bus_health_init(void)
{
    s_fail    = 0U;
    s_pending = 0U;
    s_last_ms = 0U;
    s_count   = 0U;
}

void bus_health_report(uint8_t ok)
{
    if (ok) {
        s_fail = 0U;
        return;
    }
    if (s_fail < 255U) s_fail++;
    if (s_fail >= BUS_HEALTH_FAIL_LIMIT) {
        s_fail    = 0U;
        s_pending = 1U;
    }
}

uint8_t bus_health_pending(void)
{
    return s_pending;
}

uint8_t bus_health_take(uint32_t now_ms, uint8_t bus_idle)
{
    if (!s_pending)  return 0U;
    if (!bus_idle)   return 0U;                     /* 屏正在搬：等下一圈 */
    if ((uint32_t)(now_ms - s_last_ms) < BUS_HEALTH_RECOVER_MIN_MS) return 0U;

    s_last_ms = now_ms;
    s_pending = 0U;
    s_count++;
    return 1U;
}

uint32_t bus_health_count(void)
{
    return s_count;
}

uint8_t bus_health_fail_streak(void)
{
    return s_fail;
}
