/**
 * @file  host_taskrunstub.c
 * @brief 任务解释器的**最小**替身：只给 t_hostext 用
 *
 * host_ext.c 的逻辑是"任务解释器在跑就拒绝远程驾驶"，它只调用一个
 * TaskRun_IsRunning()。真解释器在 App/Svc/task_run.c（由 t_task/t_taskstore 用真源码测），
 * 那两块测试自己会链真实现，所以这里**不能**把替身塞进 host_appstub.c
 * （会和控制模块替身一起被链进 t_task/t_taskstore，符号撞车）。
 *
 * 观测变量名 stubx_ 开头（x = "扩展层测试"），和 host_taskstub.c 的 stubt_ 区分开：
 * 两个文件永远不会同时被链进同一个可执行文件，但名字不同才不容易看错。
 */
#include <stdint.h>

#include "task_run.h"

uint8_t stubx_taskrun_running;      /* 测试里直接改：1 = 造"解释器在跑" */

uint8_t TaskRun_IsRunning(void) { return stubx_taskrun_running; }
