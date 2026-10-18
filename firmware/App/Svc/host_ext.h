/**
 * @file  host_ext.h
 * @brief 上位机扩展功能（驾驶 / 动作表 / 舵机 / 固化）的胶水层
 *
 * 用户 m13466 要求的新上位机不只读数据，还要能"实时控制车的运行状态"：
 * 左右轮速度、前进/左转/右转/后退这些快捷指令、舵机角度与 14 个动作的切换、
 * 改完动作角度/PID 之后一键固化到小车 Flash。
 *
 * 这些功能每一个都横跨好几个模块（motion / speed_control / gyro_control /
 * servo / nvram / PID_tracing / task_run），**全部集中在本文件**，而
 * `host_link.c`（串口协议）只调这里 —— 这样：
 *   1) 协议层不认识业务模块，主机测试里只用一个 stub 文件就能编译；
 *   2) 驾驶的安全策略（任务在跑就拒绝、链路断了 500ms 自动停车）只有一份实现。
 *
 * 形态与用户 m12856 定下的两种形态一致：
 *   - 正常循迹 = 三级 PID 串级（`HOST_DRIVE_TRACE` → `MOTION_TRACE_GYRO`）
 *   - 障碍流程 = 只有速度环 + 角度环（`HOST_DRIVE_BASE` → `MOTION_GYRO`）
 *   - 另有 `HOST_DRIVE_WHEELS`：直接给左右轮目标 mm/s（速度环单环，调车/试车用）
 */
#ifndef HOST_EXT_H
#define HOST_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 驾驶 */

/* 驾驶模式（与 tools/console/docs/API.md 的 /api/drive 一致） */
#define HOST_DRIVE_OFF    0u   /* 关（停车） */
#define HOST_DRIVE_WHEELS 1u   /* a = 左轮速度刻度，b = 右轮速度刻度（0..7200 的 PWM 目标刻度，负 = 反转） */
#define HOST_DRIVE_BASE   2u   /* a = 基准速度刻度（0..7200，负 = 倒车），b = 目标转向角 °（相对进入本模式时的航向） */
#define HOST_DRIVE_TRACE  3u   /* a = 基准速度刻度（0..7200，三级串级循迹）；b 忽略 */
#define HOST_DRIVE_TRACE2 4u   /* a = 基准速度刻度（0..7200，★**直驱差速循迹**：误差直接改左右轮差速，不走角度环/陀螺仪，即老海信杯那套）；b 忽略 */
#define HOST_DRIVE_HOLD   5u   /* ★2026-10-18：a = 基准速度刻度（0 = 原地转），b = **绝对目标航向 × 10**（±1800 即 ±180.0°）
                                *
                                * 为什么要它（三条审计结论）：
                                *  1) BASE 的 b 是"相对进入基准的转角"，而固件在 HOST_BASE_REGRAB_MS(1s) 之后会重抓基准
                                *     ⇒ 只要上位机发帧有 >1s 空档，目标就被反复累加（"阶梯式转不停"）。
                                *  2) HOLD 的 b 是**绝对航向** ⇒ 天然幂等：连发 200 帧同值 == 发 1 帧 ⇒ 累加不可能发生，
                                *     固件也不需要"基准航向"这个状态。
                                *  3) 用户要"松手后车自己原地转到目标并锁住"：松手时上位机继续发 base=0 的 HOLD 帧，
                                *     对固件来说只有"目标是多少"，没有"松没松手"，所以不会撒手。
                                *  off 仍然只用于急停/切页（语义不变）。 */

/* HOLD 模式：目标变化超过这个角度才真的重写目标。
 * 为什么不能每帧都写：GyroControl_SetTargetAngle() 会清 gyro_pid.integral 和到位锁存 s_settled
 * （gyro_control.c:441-448）⇒ 10Hz 下发时"到位锁存"永远攒不满 s_settle_ms ⇒ 到点后 corr 回不到 0、
 * 电机会被反复点着微抖。 */
#define HOST_HOLD_EPS_DEG   0.2f
/* HOLD 模式下角度环"不在了"（陀螺失联保护 / 别人 Stop）连续这么久 ⇒ 失效退出（绝不自己恢复） */
#define HOST_HOLD_DEAD_MS   200U
/* 这么久没有收到任何合法帧 ⇒ 判定上位机/链路断了，立刻停车 */
#define HOST_DRIVE_TIMEOUT_MS 500U
/* 驾驶时的速度环斜坡（PWM/s）：要能立刻反向，不能用循迹那套 20000 软启动 */
#define HOST_DRIVE_RAMP 100000.0f

/* 母线（电机电池）低压保护 —— 用户 m29239「一上电他就跑，得拔电才停」。
 *
 * 拔掉电机电池并不会让 MCU 掉电（MCU / 上位机 / J-Link 走 USB 供电，INA226 也还在），
 * 实测那时母线读数是 ~0.9V（**非 0**，所以"I2C 读失败给 0"不会被误判成断电）。
 * 连续 HOST_VBUS_LOW_MS 都读到这么低 ⇒ 认定电池被拔：停驾驶会话 + 停任务解释器 +
 * 回 MOTION_IDLE，于是"再接上电池"也不会自己跑起来。
 * 电压回到 HOST_VBUS_OK_MV 以上才复位（迟滞，免得在阈值附近抖），复位只重新武装监测，
 * 不会自己起步。 */
#define HOST_VBUS_LOW_MV    3000U   /* 低于这个值 = 电池没接 / 接触不良 */
#define HOST_VBUS_OK_MV     9000U   /* 高于这个值 = 电池回来了（12V 包实测 ~11.8V） */
#define HOST_VBUS_LOW_MS    500U    /* 连续低压这么久就动手 */
#define HOST_VBUS_STALE_MS  1000U   /* 读数超过这么久没更新就不动手（宁可不动） */

void    HostExt_Init(void);
void    HostExt_Task(uint32_t now_ms);            /* host_link_poll() 每拍调：看门狗 + 低压保护 */
void    HostExt_DriveKeepAlive(uint32_t now_ms);  /* 每收到一个合法帧调一次 */
void    HostExt_NoteVbusMv(uint16_t mv);          /* host_link 每真读到一次母线电压就报一次 */
uint8_t HostExt_LowVoltageDisarms(void);

/* ★2026-10-15：驾驶会话被停的原因计数（诊断）。见 host_ext.c 的说明。 */
extern volatile uint32_t g_stop_src[4];          /* 因母线低压自动停机的次数（诊断） */

/* @return 0 = 成功；-1 = 模式非法；-2 = 任务解释器在跑，拒绝远程驾驶 */
int     HostExt_DriveSet(uint8_t mode, int a, int b);
void    HostExt_DriveStop(void);
uint8_t HostExt_DriveMode(void);
uint8_t HostExt_DriveEnabled(void);               /* 1 = 正在被上位机驾驶 */
int     HostExt_DriveGet(int *a, int *b);         /* 取回当前驾驶指令（0 = 没在驾驶） */
/* HOLD 模式状态（诊断/遥测）：0 = 没在保持，1 = 正在转，2 = 已到位锁存 */
uint8_t HostExt_HoldState(void);
uint8_t HostExt_DriveWatchdogTrips(void);         /* 因链路超时自动停车的次数（诊断） */

/* -------------------------------------------------------------- 动作/舵机 */

int         HostExt_ActionSet(int n, int a1, int a2);   /* 改 RAM 动作表；1 = 成功，0 = 动作号非法 */
int         HostExt_ActionGet(int n, int *a1, int *a2);
const char *HostExt_ActionName(int n);
int         HostExt_ActionPlay(int n);                  /* 只下发终点姿态（非阻塞） */
int         HostExt_ActionSave(void);                   /* 排一次 Flash 保存 */
uint8_t     HostExt_CurrentAction(void);                /* 最近一次下发的动作号（0 = 没下发过） */

int         HostExt_ServoSet(int ch, int deg);          /* ch 1/2，deg 0..180 */
int         HostExt_ServoCenter(void);
int         HostExt_ServoDeviation(int ch, int dev_us); /* −100..100 µs 中位偏差 */
float       HostExt_ServoAngle(int ch);

/* ------------------------------------------------------------------ 固化 */

int      HostExt_SaveParams(void);       /* 立即请求保存到 Flash（不走 1.5s 防抖） */
uint8_t  HostExt_NvramState(void);       /* NVRAM_STATE_* */
uint8_t  HostExt_MotorEnabled(void);     /* 1 = 速度环使能中（电机可能带电） */
uint8_t  HostExt_TaskRunning(void);   /* 1 = 任务解释器正在跑（占着电机） */
uint8_t  HostExt_MotionMode(void);       /* Motion_Mode */
uint8_t  HostExt_MotionFault(void);

/* ------------------------------------------------- 遥测用的只读量（TELEM2）*/

void  HostExt_GrayLevel(uint8_t out[8]);  /* trace_cal_level(i)：0..255（255 = 压黑线） */
float HostExt_TracePos(void);             /* 循迹重心 1..8 */
float HostExt_TracePid(void);             /* 循迹环本拍转向偏置 */
float HostExt_AngleErr(void);             /* 角度环误差 ° */
float HostExt_AngleRateRef(void);         /* 角度环本拍期望角速度 °/s */
float HostExt_AngleCorr(void);            /* 角度环本拍最终修正量（PWM 刻度） */
int   HostExt_TraceCross(void);           /* 1 = 检测到路口/横线 */
int   HostExt_TraceLost(void);            /* 1 = 循迹算法判丢线（含灰度链路给不出真值）*/

#ifdef __cplusplus
}
#endif
#endif /* HOST_EXT_H */
