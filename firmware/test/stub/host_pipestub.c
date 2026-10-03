/**
 * @file  host_pipestub.c   pipe_hostlink 专用补桩（只给 firmware/test 用）
 *
 * 为什么需要它：真 `host_link.c` 的遥测/标定回读要用到 `jy61p_frame_count()` 与
 * `trace_cal_get_data()`，而 `host_appstub.c`（给 host_ext.c 用的那套桩）没提供这两个 ——
 * 这里补上，让「真 host_link.c + 真 host_ext.c + 真 servo.c」能链接。
 *
 * 带 `-DHOST_PIPE_REAL_SERVO` 时 `host_appstub.c` 的 servo 段被关掉、改用真
 * `firmware/App/Bsp/servo.c`。真 servo.c 写脉宽用的是 `__HAL_TIM_SetCompare(&htim1, …)`，
 * 假 HAL 头把它映射到 `host_tim_set_compare()`（不需要 `htim1.Instance`，所以这里不用
 * 造假 TIM 寄存器块）；但它调用的 `bsp_servo_init()`（真板子上配 TIM1 + 四路 PWM）
 * 主机上没有，要在这里给个空实现，否则链接不上。
 *
 * `firmware/test/run_tests.ps1` 的既有 10 个测试不需要这个文件。
 */
#include <string.h>

#include "servo.h"
#include "jy61p.h"
#include "trace_cal.h"

#ifdef HOST_PIPE_REAL_SERVO

void bsp_servo_init(void) { }       /* 真板子上是配 TIM1 + 四路 PWM，主机上是空操作 */

#else

/* host_appstub.c 里的有状态舵机桩（非 static，主机测试可直接访问） */
extern int stub_servo_dev[3];

int Servo_GetDeviation(int x)
{
    return (x >= 1 && x <= 2) ? stub_servo_dev[x] : 0;
}

#endif /* HOST_PIPE_REAL_SERVO */

uint32_t jy61p_frame_count(void)
{
    return 0u;      /* 主机上没有真陀螺仪：遥测里的"帧数"恒 0，别拿它当断言 */
}

void trace_cal_get_data(trace_cal_data_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}
