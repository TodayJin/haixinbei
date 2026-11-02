// gyro_control.h
#ifndef __GYRO_CONTROL_H
#define __GYRO_CONTROL_H

#include "main.h"
#include "jy61p.h"

// PID控制器实现
extern PID_Controller gyro_pid;

/* 角度工具（循迹环算目标航向时也要用） */
float NormalizeAngle(float angle);
float CalculateAngleError(float target, float actual);

// 陀螺仪保持控制函数
void GyroControl_Init(void);
void GyroControl_Start(void);
float GyroControl_CalibrateBias(uint32_t ms);   /* 静止零偏标定，返回 °/s */
float GyroControl_BiasZ(void);          /* 记当前航向为目标并开始保持 */
void GyroControl_Stop(void);
void GyroControl_SetBaseSpeed(int left, int right);
/* ★2026-10-26（用户发现：任务里填 110 结果全局也变成 110）：
 * 左右轮缩放的**临时覆盖**（供 TURN_DEG 块用）。
 * pct = 0 表示不覆盖（用参数表里的 0x011D/0x011E）。
 * 为什么不直接改参数：改全局会被保存进 Flash，一次没恢复就永久变了。 */
void GyroControl_SetScaleOverride(float pct);
void GyroControl_Task(void);
uint8_t GyroControl_IsEnabled(void);

/* 目标航向（度，±180）。角度环输出的是"左右轮目标速度"，
 * 速度环开启时写进速度环，否则直接写 PWM。 */
void  GyroControl_SetTargetAngle(float deg);
float GyroControl_GetTargetAngle(void);
float GyroControl_HeadingError(void);            /* 目标 - 实际（连续域模式下是未取模的差） */
/* ★2026-10-07 连续域目标（给"按住转向键累计转角"用）：
 * 上位机操控页的 `b` 是**累计转角**（可到 ±540°），而角度环默认按 ±180 取模、"最近路"算误差
 * （CalculateAngleError）—— 一旦目标领先车头超过 180°，它会把"继续往左"理解成"往右 160°"，
 * 车突然倒拐、来回拐（真车实测翻号点：183/201/204/209°）。
 * 连续域模式把目标与航向都放在**不取模**的坐标里（航向由 Yaw 逐拍解卷绕得到），
 * 误差永远是"还差多少度"，过 ±180° 也不会翻号。
 * 只在 `HOST_DRIVE_BASE` 里用；任务/循迹路径仍走 SetTargetAngle ⇒ 行为逐位不变。 */
void  GyroControl_SetTargetAngleCont(float deg); /* deg = 连续域绝对目标角（不取模） */
float GyroControl_GetYawCont(void);              /* 连续域当前航向（未取模） */
/* 遥测只读快照（上位机 angle 组）：本拍内部用的期望角速度 / 最终修正量 */
float GyroControl_DebugRateRef(void);
float GyroControl_DebugCorr(void);
void  GyroControl_AimRelative(float delta_deg);  /* 在当前目标上叠加 */
/* 循迹串级专用：给速率环直接前馈"期望偏航角速度"（°/s，正 = 向左）。
 * 消费式语义 —— GyroControl_Task() 每拍读走并清零，只影响设置它的那一拍。
 * 为什么需要（内环 τ≈1/ang_kp 的一阶滞后压在外环 ω_n 上）见 gyro_control.c 里的注释。 */
void  GyroControl_SetTraceRate(float rate_deg_s);
float GyroControl_GetTraceRate(void);

/* 转到某个航向：可查是否到位（用 HP_TURN_TOLERANCE / HP_TURN_TIMEOUT 判定） */
void    GyroControl_TurnTo(float deg, uint32_t now_ms);
void    GyroControl_TurnUpdate(uint32_t now_ms);
uint8_t GyroControl_IsTurning(void);
uint8_t GyroControl_TurnDone(void);

/* ------------------------------------------------------------------
 * 角度环整定参数（2026-10-02 起集中在这里声明）
 *
 * 它们本来就是 .c 里的文件作用域变量，专门留给台架用 SWD 直改（不用重烧板子，
 * 参数含义见 gyro_control.c 里每个定义处的注释）。放到头里只是为了：
 *   ① 台架脚本/主机侧单元测试能按名字引用，不用自己重抄一份 extern；
 *   ② 谁改了这个集合，链接期就能被发现（漏定义会直接报未定义符号）。
 * 正常固件流程里这些量由菜单/上位机经 host_link 参数表间接改，别到处直接赋值。
 * ------------------------------------------------------------------ */
extern volatile uint8_t s_law;        /* 0 老PID / 1 三档开关 / 2 连续速率环 / 3 速率环+到位锁存 / 4 开环 */
extern volatile float s_corr_limit;   /* 修正量安全上限（PWM 刻度） */
extern volatile float s_corr_ref;     /* 差速天花板随基准速度放大的参考刻度（0 = 不放大） */
extern volatile float s_ang_kp;       /* 外环 Kp：(°/s)/° */
extern volatile float s_rate_max;     /* 期望角速度上限 °/s（pivot：ff_rate == 0 时用） */
extern volatile float s_trace_rate_max; /* **循迹专用**期望角速度上限 °/s（ff_rate != 0 时用；HP_TRACE_RATE_MAX） */
extern volatile float s_rate_band;    /* s_law=1 内环滞环带宽 °/s */
extern volatile float s_corr_spin;    /* s_law=1 加速档 / s_law=2,3 起转助推上限 */
extern volatile float s_corr_hold;    /* s_law=1 维持档 / s_law=2 摩擦前馈 */
extern volatile float s_corr_brake;   /* s_law=1 反刹档 */
extern volatile float s_err_stop;     /* s_law=0,1 到位松手门限 ° */
extern volatile float s_rate_kp;      /* 速率环 Kp：PWM/(°/s) */
extern volatile float s_ff_rate;      /* 摩擦前馈方向判定阈值 °/s */
extern volatile float s_stuck_rate;   /* "真的停住了"阈值 °/s */
extern volatile float s_boost_rate;   /* 起转助推爬升斜率 PWM/s */
extern volatile float s_rate_lpf;     /* 角速度反馈低通系数 */
extern volatile float s_settle_deg;   /* s_law=3 停车带宽 ° */
extern volatile float s_settle_rate;  /* s_law=3 带内松手/锁存的角速度门限 °/s */
extern volatile float s_corr_slew;    /* corr 斜率限幅 PWM/s */
extern volatile float s_v_floor;      /* s_law=3 逼近段角速度下限 °/s（0 = 关） */
extern volatile float s_rate_kd;      /* s_law=3 速率微分阻尼 PWM/(°/s²) */
extern volatile float s_acc_lpf;      /* 角加速度反馈低通系数 */
extern volatile float s_decel;        /* s_law=2 √刹车曲线用减速度 °/s² */
extern volatile float s_open_corr;    /* s_law=4 开环台架专用 corr */
extern volatile float s_settle_ms;    /* s_law=3 带内待够多久认到位 ms */
extern volatile float s_boost_gap;    /* 起转助推冷却 ms */

/**
 * @brief 本拍生效的修正量（差速）上限：`s_corr_limit` 再按基准速度放大
 *        （见 gyro_control.c 的 GyroControl_CorrLimit 注释）。
 */
float GyroControl_CorrLimit(void);

void GyroControl_TurnRightFixed(void);
void GyroControl_TurnLeftFixed(void);

void GyroControl_TurnRight(float angle);
void GyroControl_TurnLeft(float angle);
// 定时器控制函数
void Gyro_Timer_Start(void);
void Gyro_Timer_Stop(void);

#endif
