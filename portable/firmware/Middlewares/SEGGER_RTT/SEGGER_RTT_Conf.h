/**
 * @file  SEGGER_RTT_Conf.h
 * @brief RTT 用户配置（其余项由 SEGGER_RTT_ConfDefaults.h 填默认值）
 */
#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

#ifdef __ICCARM__
  #include <intrinsics.h>
#endif

/* 上行缓冲：探针帧比较多，给大一点；下行只要够收单条命令 */
#ifndef   BUFFER_SIZE_UP
  #define BUFFER_SIZE_UP                            (4096)
#endif
#ifndef   BUFFER_SIZE_DOWN
  #define BUFFER_SIZE_DOWN                          (64)
#endif

/* 不阻塞打印，满了就丢，绝不卡住业务 */
#ifndef   SEGGER_RTT_MODE_DEFAULT
  #define SEGGER_RTT_MODE_DEFAULT                   SEGGER_RTT_MODE_NO_BLOCK_SKIP
#endif

#ifndef   SEGGER_RTT_PRINTF_BUFFER_SIZE
  #define SEGGER_RTT_PRINTF_BUFFER_SIZE             (128u)
#endif

/* Cortex-M4 单核：临界区用 PRIMASK 关中断 */
#define SEGGER_RTT_MAX_INTERRUPT_PRIORITY         (0x20)

#endif /* SEGGER_RTT_CONF_H */
