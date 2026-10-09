/**
 * @file  task_run.h
 * @brief 任务程序解释器：**全非阻塞**、5ms 一拍，把上位机拼的状态机跑在车上
 *
 * 设计文档 `docs/任务编程.md`。要点：
 *   - 程序格式见 task_prog.h；本模块只管执行语义（块推进、条件等待、状态跳转）。
 *   - 一个时刻只有一个状态在跑：先跑该状态的 enter 序列（若有），再跑 body 序列。
 *   - body 跑到底没有跳转 = 状态停住（上下位机都不该这样，TaskProg_Validate 会拦）。
 *   - 运行期间**不受**上位机链路看门狗约束（比赛要离线自主跑）；急停/停止命令仍然随时生效。
 *
 * 调用约定：`TaskRun_Tick(HAL_GetTick())` 由主循环调用（不必精确 5ms，里程积分按实际 dt 算）。
 */
#ifndef __TASK_RUN_H__
#define __TASK_RUN_H__

#include <stdint.h>

/* 程序镜像缓冲（RAM）：上传/从 Flash 拷出来的程序都放这里，运行期不碰 Flash */
#define TASK_RUN_MAX_PROG  4096u

#ifndef TASK_RUN_OFF
#define TASK_RUN_OFF       0xFFu   /* 状态号/未运行 */
#endif

/* 运行标志位（遥测回传） */
#define TASK_RUN_F_TIMEOUT  (1u << 0)   /* 上一次"等待条件"是超时才过的 */
#define TASK_RUN_F_DONE     (1u << 1)   /* 任务已正常结束（END 块） */

/* 载入一个程序（会校验格式 + 拷进 RAM）。返回 0 或 -TASK_E_xxx */
int      TaskRun_Load(const uint8_t *prog, uint32_t len);
/* 启动（先清状态；没有载入过程序则返回 -1）。task_id 只是给遥测用的标签 */
int      TaskRun_Start(uint16_t task_id);
/* 停止并停车（幂等） */
void     TaskRun_Stop(void);
/* 5ms 一拍 */
void     TaskRun_Tick(uint32_t now_ms);

uint8_t  TaskRun_IsRunning(void);
uint16_t TaskRun_Id(void);
uint8_t  TaskRun_State(void);        /* TASK_RUN_OFF = 没在跑 */
uint8_t  TaskRun_Phase(void);        /* 0 = enter 序列，1 = body 序列 */
uint16_t TaskRun_Block(void);        /* 当前序列里的块下标 */
uint16_t TaskRun_BlockCount(void);
uint32_t TaskRun_ElapsedMs(void);    /* 任务已运行时间 */
uint32_t TaskRun_BlockMs(void);      /* 当前块已停留时间 */
uint8_t  TaskRun_Flags(void);
uint8_t  TaskRun_Stage(void);        /* ★当前段号（旧代码的 black_num）；任务启动时清 0 */
uint8_t  TaskRun_Done(void);         /* 1 = 跑到过 END 块（正常结束） */
uint8_t  TaskRun_Loaded(void);       /* 1 = RAM 里有一份校验过的程序 */
uint16_t TaskRun_ProgLen(void);
uint16_t TaskRun_CrossCount(void);   /* 解释器累计的横线次数 */
uint32_t TaskRun_DistMm(void);

#endif /* __TASK_RUN_H__ */
