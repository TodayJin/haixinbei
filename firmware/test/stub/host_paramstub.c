/**
 * @file  host_paramstub.c
 * @brief  参数表的最小替身：只为 t_task / t_taskstore 提供 host_param_get()
 *
 * 为什么单独一个文件：`TaskRun_Start()` 启动时会取一次 `HP_TASK_SPEED` 当默认基准速度
 * （`firmware/App/Svc/task_run.c`），而 t_task/t_taskstore 链的是真 task_run.c + 真
 * task_store.c/task_prog.c，不含协议层。
 *
 * 为什么不塞进 host_appstub.c：那个替身**同时**被管道对拍构建
 * （`tools/console/tests/test_firmware_pipe.py`）链进去，而那个构建里 host_link.c 是
 * **真源码**（它自己就定义 host_param_get）⇒ 塞进去立刻 multiple definition。
 *
 * 替身直接回落兜底值：真 host_param_get() 在参数表里查不到或存档值非有限时也是回落
 * fallback，所以这里的 6480 与真实现的首启值一致，测试里 stub_motion_speed 看到的就是
 * 真车首启会用的那个速度。
 */
#include <stdint.h>

float host_param_get(uint16_t id, float fallback)
{
    (void)id;
    return fallback;
}
