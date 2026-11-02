/**
 * @file  host_rtt.c   （主机侧 RTT 桩 —— 只给 firmware/test 用）
 * 被测模块里 RTT 只用来打日志，主机上既不打印也不阻塞，只记调用次数。
 */
#include <stdarg.h>
#include <stdio.h>
#include "SEGGER_RTT.h"

int host_rtt_calls;

int SEGGER_RTT_printf(unsigned BufferIndex, const char *sFormat, ...)
{
    (void)BufferIndex; (void)sFormat;
    host_rtt_calls++;
    return 0;
}

unsigned SEGGER_RTT_Write(unsigned BufferIndex, const void *pBuffer, unsigned NumBytes)
{
    (void)BufferIndex; (void)pBuffer;
    host_rtt_calls++;
    return NumBytes;
}

unsigned SEGGER_RTT_WriteString(unsigned BufferIndex, const char *s)
{
    (void)BufferIndex; (void)s;
    host_rtt_calls++;
    return 0;
}
