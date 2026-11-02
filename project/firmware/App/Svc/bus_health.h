/**
 * @file  bus_health.h
 * @brief I2C 总线健康看门狗（纯逻辑，不碰 HAL —— 这样主机侧能单测）
 *
 * 为什么需要（2026-10-02 加）：
 *   总线被从机拉死（模块在事务中间掉电/复位、ESD 打一下 SDA）之后，STM32 这边的
 *   每一次 HAL_I2C_* 都只会超时失败，**永不自愈** —— 屏黑着、电流和灰度全读不到，
 *   只能断电重启。board.c 里其实早就有 board_i2c_bus_recover()（手动打 9 个时钟 +
 *   STOP），但**从来没人调用**（死代码）。
 *
 * 这里只做"什么时候该恢复"的决策，动作（gpio 打时钟 + 重配 I2C）留给 board.c：
 *   ① 每次 I2C 读/写用 bus_health_report() 报结果；
 *   ② 连续失败 BUS_HEALTH_FAIL_LIMIT 次 -> 置"待恢复"；
 *   ③ 调用者在**总线空闲**时调 bus_health_take(now, bus_idle)；
 *      返回 1 就请它立刻执行一次总线恢复。
 *   ④ 两次恢复之间至少隔 BUS_HEALTH_RECOVER_MIN_MS：恢复本身要阻塞十几毫秒，
 *      真"没有这个从机"的时候不能每帧都去刷总线（灰度模块没插是正常配置）。
 */
#ifndef BUS_HEALTH_H
#define BUS_HEALTH_H

#include <stdint.h>

#define BUS_HEALTH_FAIL_LIMIT      5U      /* 连续失败几次算"总线死了" */
#define BUS_HEALTH_RECOVER_MIN_MS  1000U   /* 两次恢复最小间隔 */

void     bus_health_init(void);
void     bus_health_report(uint8_t ok);                     /* 每次 I2C 操作报结果 */
uint8_t  bus_health_pending(void);                          /* 1 = 攒够失败，等空闲时机 */
uint8_t  bus_health_take(uint32_t now_ms, uint8_t bus_idle); /* 1 = 请调用者现在恢复一次 */
uint32_t bus_health_count(void);                            /* 上电以来恢复次数 */
uint8_t  bus_health_fail_streak(void);                      /* 当前连续失败数（诊断用） */

#endif /* BUS_HEALTH_H */
