/**
 * @file  PID_tracing.c
 * @brief 循迹环：8 路灰度 -> 位置误差 -> （差速直出 | 目标航向）
 *
 * 数据口径（沿用旧板，不改）：
 *   data = 压线通道的加权重心，范围 1..8，车在正中间时 = 4.5；
 *   丢线时保持上一次的值（all_sensors_lost），避免突然打到满舵。
 *
 * 两种用法：
 *   1) motor_tracing_pid_speed(base)：PID 输出直接当左右轮差速（旧板思路，不用陀螺仪）
 *   2) motor_tracing_cascade(base)：PID 输出变成"目标航向偏移"，
 *      交给角度环（gyro_control.c）算左右轮速度，再交给速度环（speed_control.c）
 */
#include "PID_tracing.h"

#include "motor.h"
#include "pid.h"
#include "jy61p.h"
#include "tracing.h"
#include "trace_cal.h"     /* 单帧自适应：trace_cal_frame() / TRACE_FRAME_xxx */
#include "speed_control.h"
#include "gyro_control.h"
#include "host_link.h"
#include "bsp_board.h"
#include <math.h>

#define TRACE_SETPOINT       4.5f    /* 车在线上正中时的重心值 */
#define TRACE_ANGLE_PER_UNIT_DEFAULT 10.0f  /* 重心每偏 1 格 -> 10 度目标航向偏移（出厂值） */
#define TRACE_ANGLE_MAX_DEFAULT      35.0f  /* 航向偏移上限（度）（出厂值） */
#define TRACE_ANGLE_PER_UNIT_MIN      0.5f  /* 台架/上位机写坏值时的兜底夹子 */
#define TRACE_ANGLE_PER_UNIT_MAX     60.0f
#define TRACE_ANGLE_MAX_LIMIT_MIN     1.0f
#define TRACE_ANGLE_MAX_LIMIT_MAX    90.0f

/* 串级外环（重心 -> 目标航向）的两个标定量：**真值在 host_link 的参数表里**
 * （HP_TRACE_ANGLE_PER_UNIT / HP_TRACE_ANGLE_MAX），上位机「调参 → 串级」页能改、
 * 能固化进 Flash；下面的 DEFAULT 只是出厂值/坏值兜底。
 *
 * 这两个数决定"重心偏 1 格要求车头转多少度"。台架实测几何（重心 1 单位 ≈ 5~7° 车身角，
 * 细区，见 docs/自主优化记录.md 第 12/15 节）曾让人推测 10°/格 过量 1.5~2 倍，但第十轮
 * 受控阶梯实测（4/6/8/10/12/15/20，见第 16 节）结论是 **10.0f 最好**：外环还要对抗内环
 * 滞后与静摩擦死区，增益大反而有利。所以出厂默认保持 10.0f。 */
#define TRACE_CROSS_MIN      4       /* ≥4 路同时压线 = 路口（说明书第七章，按线宽可改） */
#define TRACE_LOST_MS        300U    /* 丢线超过这么久就减速找线 */
#define TRACE_LOST_DECAY_MS  500U    /* 丢线超时后，再把"保持的满舵"线性收到 0 的时间 */

#define TRACE_EDGE_SAT       0.9f    /* 最外一路黑度 ≥ 0.9×最黑 = 黑带被阵边界切掉（外推的唯一入口）*/
#define TRACE_BARW_INIT      3.0f    /* 黑带宽度初值（格=传感器间距），前面几帧就会自己收敛 */
#define TRACE_BARW_TAU       0.02f   /* 宽度跟踪系数（50Hz -> 约 1s 时间常数）*/
#define TRACE_POS_MED_N      3U      /* 位置中值窗（帧）：丢单帧脉冲，只滞后 1 帧 = 20ms */

static float   s_setpoint = TRACE_SETPOINT;
static float   data       = TRACE_SETPOINT;   /* 当前重心 */
static float   last_data  = TRACE_SETPOINT;
static uint8_t all_sensors_lost = 0;
static float   last_pid_output  = 0.0f;

/* 边缘外推（用户 m07972 第 1 条）*/
static float   s_bar_w    = TRACE_BARW_INIT;
static uint8_t s_bw_valid = 0U;

/* 位置中值滤波 + 抖动统计（用户 m07972 第 4 条）*/
static float   s_ph[TRACE_POS_MED_N];
static uint8_t s_ph_n      = 0U;
static float   s_prev_raw  = 0.0f;
static float   s_prev_filt = 0.0f;

/* 可见状态（给台架/真车读，不参与控制；声明在 PID_tracing.h）*/
float    trace_bar_w        = TRACE_BARW_INIT;  /* 跟踪到的黑带宽度（格）*/
float    trace_pos_jit_raw  = 0.0f;             /* Σ|重心 - 上帧重心|（滤波前）*/
float    trace_pos_jit_filt = 0.0f;             /* 同上，滤波后 */
uint32_t trace_pos_frames   = 0U;               /* 统计帧数（均值 = 上面两个 / 它）*/
uint32_t trace_edge_cnt     = 0U;               /* 累计触发外推的帧数 */
uint8_t  trace_edge_extrap  = 0U;               /* 本帧是否触发外推 */

/* 消抖：连续 2 次一样才认（每 5ms 采一次 -> 10ms 消抖） */
static uint8_t s_stable_raw = 0;
static uint8_t s_pend_raw   = 0;
static uint8_t s_pend_n     = 0;

/* 路口 */
static uint8_t  s_cross       = 0;
static uint32_t s_cross_at    = 0;
static uint8_t  s_cross_cnt   = 0;
static uint32_t s_lost_since  = 0;

/* 串级：循迹基准航向 */
static float   s_base_yaw   = 0.0f;
static uint8_t s_base_valid = 0;

static void apply_speeds(float left, float right)
{
    int li = (int)left;
    int ri = (int)right;

    if (li >  MOTOR_PWM_MAX) li =  MOTOR_PWM_MAX;
    if (li <  MOTOR_PWM_MIN) li =  MOTOR_PWM_MIN;
    if (ri >  MOTOR_PWM_MAX) ri =  MOTOR_PWM_MAX;
    if (ri <  MOTOR_PWM_MIN) ri =  MOTOR_PWM_MIN;

    if (SpeedControl_IsEnabled()) {
        SpeedControl_SetTarget(li, ri);     /* 交给速度环（编码器闭环） */
    } else {
        Load(li, ri);                       /* 速度环没开就直接写 PWM */
    }
}

/**
 * @brief 读 8 路灰度 -> 加权重心 data / all_sensors_lost / 路口
 *
 * 两条路：
 *   ① 单帧自适应（trace_cal.h 末「单帧自适应」，默认开）：8 路归一化黑度当权重，
 *      重心连续取值（1..8，正中 4.5），亚克力/换灯/电压漂带来的"整体抬高、
 *      对比度变小"它自己每帧补偿；真的没对比度（塌到噪声里）就判 WEAK 当丢线
 *      （data 保持上一帧），**不会**像固定阈值那样把噪声判成压线。
 *   ② 老的中点阈值二值法（没标定 / 没模拟量 / trace_adapt_enable = 0）：消抖 + 8 个 0/1 取平均。
 */
void read_tracing_data_pid(void)
{
    float   w[TRACE_CAL_CH];
    trace_frame_t fr = trace_cal_frame(w);
    float   num = 0.0f, sum = 0.0f, wmax = 0.0f;
    uint8_t raw = 0;
    int     i, on;
    uint32_t now = HAL_GetTick();
    uint8_t lost;

    if (fr == TRACE_FRAME_OK) {
        /* ---- ① 连续加权重心（L1..L4 = 1..4，R1..R4 = 5..8） ---- */
        for (i = 0; i < 8; i++) {
            if (w[i] > wmax) wmax = w[i];
        }
        on  = 0;
        num = 0.0f;
        sum = 0.0f;
        for (i = 0; i < 8; i++) {
            /* 过本帧半高 = 这一路算压线：和柱子上的那条分界线、界面底部的 0/1 完全一致 */
            if (w[i] * 2.0f > wmax) on++;
            num += w[i];
            sum += w[i] * (float)(i + 1);
        }
        if (num > 0.0f) {
            float pos   = sum / num;   /* 加权重心：1..8 */
            float cover = 0.0f;
            float m;

            for (i = 0; i < 8; i++) cover += w[i];   /* w[] 已归一化，Σ = 可见覆盖长度（格）*/

            /* ---- ①.5 边缘外推（用户 m07972 第 1 条）----
             * 线跑出阵外时，可见的那部分把重心顶在 1（或 8），误差封顶 -> aim 也就顶在 ±35°。
             * 用黑带宽度把线心推回阵外：可见覆盖长度 cover = Σ 归一化黑度，
             *     右边界被切：线心 = 0.5 + cover - 黑带宽度/2
             *     左边界被切：线心 = 8.5 - cover + 黑带宽度/2
             * 只在"最外那一路黑度 ≥ 0.9×最黑"时启用 —— 这恰好等价于"黑带的外边界已经越过
             * 阵边界"（那一路全被盖住）；没被切时最外路必然盖不满，重心本身就是准的，
             * 一个计数都不动。用户说线基本跑不出阵，所以平时这条永不生效（trace_edge_cnt 可查）。
             * 黑带宽度用慢速 EMA 学，且只用没被切的帧更新（被切的帧量不到完整宽度）。*/
            trace_edge_extrap = 0U;
            if (wmax > 0.0f) {
                uint8_t cut_l = (w[0] >= TRACE_EDGE_SAT * wmax) ? 1U : 0U;
                uint8_t cut_r = (w[7] >= TRACE_EDGE_SAT * wmax) ? 1U : 0U;

                if (!cut_l && !cut_r) {
                    if (!s_bw_valid) { s_bar_w = cover; s_bw_valid = 1U; }
                    else             { s_bar_w += (cover - s_bar_w) * TRACE_BARW_TAU; }
                }
                if (s_bar_w < 1.0f) s_bar_w = 1.0f;
                if (s_bar_w > 6.5f) s_bar_w = 6.5f;
                trace_bar_w = s_bar_w;

                if (cut_l) {
                    float p = 0.5f + cover - s_bar_w * 0.5f;
                    if (p < pos) { pos = p; trace_edge_extrap = 1U; }
                } else if (cut_r) {
                    float p = 8.5f - cover + s_bar_w * 0.5f;
                    if (p > pos) { pos = p; trace_edge_extrap = 1U; }
                }
                if (trace_edge_extrap) trace_edge_cnt++;
                if (pos < -2.0f) pos = -2.0f;
                if (pos > 11.0f) pos = 11.0f;
            }

            /* ---- ①.6 位置 3 帧中值（用户 m07972 第 4 条）----
             * 要求"滤一下波但别那么滞后"：中值只滞后 1 帧（20ms），单帧脉冲直接丢掉，
             * 斜坡信号不像低通那样越滤越迟。抖动统计把它和滤波前都累起来（除以帧数得均值）。*/
            if (s_ph_n < TRACE_POS_MED_N) {
                s_ph[s_ph_n++] = pos;
                m = pos;
            } else {
                float a, b, c;
                s_ph[2] = s_ph[1];
                s_ph[1] = s_ph[0];
                s_ph[0] = pos;
                a = s_ph[0]; b = s_ph[1]; c = s_ph[2];
                m = (a < b) ? ((b < c) ? b : ((a < c) ? c : a))
                            : ((a < c) ? a : ((b < c) ? c : b));
            }

            if (trace_pos_frames > 0U) {
                trace_pos_jit_raw  += fabsf(pos - s_prev_raw);
                trace_pos_jit_filt += fabsf(m   - s_prev_filt);
            }
            s_prev_raw  = pos;
            s_prev_filt = m;
            trace_pos_frames++;

            data      = m;
            last_data = data;
        }
        lost = 0U;
    } else if (fr == TRACE_FRAME_WEAK) {
        /* 模拟量在，但这帧对比度不够（没信息）：当丢线，data 保持上一帧 */
        on   = 0;
        lost = 1U;
    } else {
        /* ---- ② 老的固定阈值 + 消抖 ---- */
        for (i = 0; i < 8; i++) {
            if (trace_read_channel((uint8_t)i)) raw |= (uint8_t)(1U << i);
        }
        if (raw == s_stable_raw) {
            s_pend_n = 0;
        } else {
            if (raw == s_pend_raw) {
                if (s_pend_n < 2U) s_pend_n++;
            } else {
                s_pend_raw = raw;
                s_pend_n   = 1U;
            }
            if (s_pend_n >= 2U) {
                s_stable_raw = raw;
                s_pend_n     = 0U;
            }
        }
        raw = s_stable_raw;

        on  = 0;
        num = 0.0f;
        sum = 0.0f;
        for (i = 0; i < 8; i++) {
            if (raw & (uint8_t)(1U << i)) {
                num += 1.0f;
                sum += (float)(i + 1);
                on++;
            }
        }
        if (num > 0.0f) {
            data      = sum / num;
            last_data = data;
        }
        lost = (on == 0) ? 1U : 0U;
    }
    /* 丢线时 data 保持不变（= 上一次的重心），方向就不会突然反打 */
    all_sensors_lost = lost;

    if (all_sensors_lost) {
        if (s_lost_since == 0U) s_lost_since = now;
    } else {
        /* 刚重新找到线：把循迹 PID 清一次。丢线期间 data 保持不变、误差也不变，
         * 积分照样在累；不清的话重新压线那一拍会把攒下的积分一次性放出来，
         * 表现为"找回线就猛打一下方向"。 */
        if (s_lost_since != 0U) PID_Reset();
        s_lost_since = 0U;
    }

    /* 路口：≥TRACE_CROSS_MIN 路同时压线，连续 2 次才认，认完 200ms 内不重复报 */
    if (on >= TRACE_CROSS_MIN) {
        if (s_cross_cnt < 2U) s_cross_cnt++;
        if (s_cross_cnt >= 2U && !s_cross && (uint32_t)(now - s_cross_at) > 200U) {
            s_cross    = 1U;
            s_cross_at = now;
        }
    } else {
        s_cross_cnt = 0U;
        if (on <= TRACE_CROSS_MIN - 2) s_cross = 0U;
    }
}

uint8_t tracing_cross(void)
{
    return s_cross;
}

void tracing_cross_clear(void)
{
    s_cross = 0U;
}

void show_tracing_data_pid(void)
{
    /* 显示交给 KK_UI */
}

/**
 * @brief 循迹环 + 速度环（差速直出，不用陀螺仪）
 */
void motor_tracing_pid_speed(int base_speed)
{
    float pid_output, l, r;

    read_tracing_data_pid();

    /* base=0 就是"原地对转"（pivot）：左右轮反向、车不前进。这时被控对象和
     * 前进循迹完全不同，用 pid.c 里实测标出来的 pivot 那组增益；非 0 还是走
     * 参数表里的前进增益。模式切换时 PID_SetPivot() 内部会清积分/微分历史。 */
    PID_SetPivot(base_speed == 0);

    if (all_sensors_lost) {
        /* 丢线分两段：短时间保持上一次的转向（等它扫回来）；超过 TRACE_LOST_MS 之后
         * 必须把"保持的转向"收掉 —— last_pid_output 是丢线前那一刻的值，如果那次正好是
         * 满舵（追大偏差时就是 7200），一直保持就会变成"原地打转停不下来"。
         * 实测（2026-10-02 台架）：设定值从 4.5 切到 2.0 时线甩出探头，reqL 卡在 7200
         * 转了 4 秒以上、yaw 绕了 360°+。这里按 TRACE_LOST_DECAY_MS 线性收到 0。 */
        uint32_t lost_ms = (s_lost_since != 0U) ? (uint32_t)(HAL_GetTick() - s_lost_since) : 0U;

        if (lost_ms > TRACE_LOST_MS) {
            uint32_t over = lost_ms - TRACE_LOST_MS;
            float k = (over >= TRACE_LOST_DECAY_MS)
                        ? 0.0f
                        : (1.0f - (float)over / (float)TRACE_LOST_DECAY_MS);
            pid_output = last_pid_output * k;
            base_speed = base_speed / 2;              /* 丢太久：减速找线 */
        } else {
            pid_output = last_pid_output;             /* 短暂丢线：保持上一次的转向 */
        }
    } else {
        pid_output    = PID_Compute(s_setpoint, data);
        last_pid_output = pid_output;
    }

    l = (float)base_speed - pid_output;
    r = (float)base_speed + pid_output;

    apply_speeds(l, r);
}

/**
 * @brief 设置三级串级的外环标定量（重心偏 1 格 -> 目标航向偏多少度）
 *
 * 这两个量的真值现在放在 host_link 的参数表里（HP_TRACE_ANGLE_PER_UNIT / _MAX），
 * 所以这个 setter 就是"写参数表"；超范围的写不进（参数表自己带上下限）。
 * 参数表的兜底出厂值是 10°/格、±35°，来自第十轮受控阶梯实测（第 16 节）：
 * 4/6 收不干净，10/12/15/20 等价，10 最优。
 */
void PID_Tracing_SetCascadeGains(float deg_per_unit, float aim_max_deg)
{
    if (deg_per_unit >= TRACE_ANGLE_PER_UNIT_MIN && deg_per_unit <= TRACE_ANGLE_PER_UNIT_MAX) {
        host_param_set_local(HP_TRACE_ANGLE_PER_UNIT, deg_per_unit);
    }
    if (aim_max_deg >= TRACE_ANGLE_MAX_LIMIT_MIN && aim_max_deg <= TRACE_ANGLE_MAX_LIMIT_MAX) {
        host_param_set_local(HP_TRACE_ANGLE_MAX, aim_max_deg);
    }
}

/* 读值时兜一层：参数表里是 0/NaN/巨大值时用出厂值，绝不把车带飞 */
static float cascade_per_unit(void)
{
    float v = host_param_get(HP_TRACE_ANGLE_PER_UNIT, TRACE_ANGLE_PER_UNIT_DEFAULT);
    if (!(v >= TRACE_ANGLE_PER_UNIT_MIN && v <= TRACE_ANGLE_PER_UNIT_MAX)) {
        v = TRACE_ANGLE_PER_UNIT_DEFAULT;
    }
    return v;
}

static float cascade_aim_max(void)
{
    float v = host_param_get(HP_TRACE_ANGLE_MAX, TRACE_ANGLE_MAX_DEFAULT);
    if (!(v >= TRACE_ANGLE_MAX_LIMIT_MIN && v <= TRACE_ANGLE_MAX_LIMIT_MAX)) {
        v = TRACE_ANGLE_MAX_DEFAULT;
    }
    return v;
}

void PID_Tracing_GetCascadeGains(float *deg_per_unit, float *aim_max_deg)
{
    if (deg_per_unit != 0) { *deg_per_unit = cascade_per_unit(); }
    if (aim_max_deg  != 0) { *aim_max_deg  = cascade_aim_max(); }
}

/**
 * @brief 三层串级：灰度误差 -> 目标航向 -> 角度环 -> 左右轮目标速度 -> 速度环
 *
 * 符号约定（与 gyro_angles 一致）：偏航角**增大 = 向左转**。
 * 线偏到车的右侧（data > 4.5）要向右转 -> 目标航向要减小。
 */
void motor_tracing_cascade(int base_speed)
{
    float err, aim, now_yaw;

    read_tracing_data_pid();

    err = data - s_setpoint;                 /* >0：线在车的右侧 */

    /* 丢线时 aim 不更新（data 保持），等于保持上一拍的方向 */
    {
        float per_unit = cascade_per_unit();
        float aim_max  = cascade_aim_max();
        aim = -per_unit * err;
        if (aim >  aim_max) aim =  aim_max;
        if (aim < -aim_max) aim = -aim_max;
    }

    now_yaw = NormalizeAngle(Yaw);

    if (!s_base_valid) {
        s_base_yaw   = now_yaw;              /* 进串级那一刻的航向 = 基准 */
        s_base_valid = 1U;
    } else if (fabsf(err) < 0.6f) {
        s_base_yaw = now_yaw;                /* 车在线中间时重记基准，消掉陀螺仪漂移 */
    }

    if (all_sensors_lost &&
        s_lost_since != 0U &&
        (uint32_t)(HAL_GetTick() - s_lost_since) > TRACE_LOST_MS) {
        base_speed = base_speed / 2;         /* 丢线太久：减速找线 */
    }

    /* 外两层：目标航向 -> 角度环 -> 左右轮目标速度 */
    GyroControl_SetTargetAngle(NormalizeAngle(s_base_yaw + aim));
    GyroControl_SetBaseSpeed(base_speed, base_speed);
    GyroControl_Task();
}

void motor_tracing_reset_cascade(void)
{
    s_base_valid    = 0U;
    last_pid_output = 0.0f;
    s_lost_since    = 0U;
    data            = TRACE_SETPOINT;
    last_data       = TRACE_SETPOINT;
    all_sensors_lost = 0U;
}

/* ============= 旧接口（renwu.c 的 A() 在调）的基准速度（2026-09-30 m02112 同步） =============
 * 旧板 PWM 满量程只有 1000，所以旧代码里写死的是 1000（满油门）和 200（20%）；
 * 本板满量程 7200。照抄的 1000 在本板只有 13.9% 占空比（低于静摩擦死区 ~2500）⇒ 推不动。
 * 现在跟随“当前循迹”的两个参数：正常档 = 菜单里的“循迹速度”（HP_BASE_SPEED，默认 6480 = 旧 900），
 * 慢速档 = HP_BASE_SPEED_SLOW（默认 2500，不是旧 200×7.2=1440，因为 1440 仍在死区以下）。 */
static int trace_base_pwm(void)
{
    int base = (int)host_param_get(HP_BASE_SPEED, 6480.0f);
    if (base < 0)             base = 0;
    if (base > MOTOR_PWM_MAX) base = MOTOR_PWM_MAX;
    return base;
}

static int trace_base_pwm_slow(void)
{
    int base = (int)host_param_get(HP_BASE_SPEED_SLOW, 2500.0f);
    if (base < 0)             base = 0;
    if (base > MOTOR_PWM_MAX) base = MOTOR_PWM_MAX;
    return base;
}

void motor_tracing_pid(float target)
{
    s_setpoint = target;
    motor_tracing_pid_speed(trace_base_pwm());
}

/**
 * @brief 遥测只读快照（给上位机的 trace 组）
 *        `data` = 本拍重心（1..8，车在正中 4.5），`last_pid_output` = 循迹环本拍
 *        给左右轮的转向偏置。都不参与控制，只是让上位机看见环里在算什么。
 */
float PID_Tracing_TracePos(void)
{
    return data;
}

float PID_Tracing_PidOutput(void)
{
    return last_pid_output;
}

void motor_tracing_pid_0(float target)
{
    s_setpoint = target;
    motor_tracing_pid_speed(trace_base_pwm_slow());
}

void motor_tracing_pid_0_task(void)
{
    s_setpoint = TRACE_SETPOINT;
    motor_tracing_pid_speed(trace_base_pwm_slow());
}

/* ---------------------------------------------------------------------------
 * 后台循迹环的开关与时基（2026-09-30 m02112 同步）
 *
 * 旧板：`renwu.c` 里写 `PID_Tracing_Timer_Start(); HAL_Delay(1000); PID_Tracing_Timer_Stop();`
 * ——那 1 秒里主循环阻塞在 HAL_Delay，真正循迹的是 TIM2 中断里的 motor_tracing_pid_0_task()
 * （TIM2：PSC=7199 / ARR=999 @72MHz = 10Hz）。
 *
 * 新板没有多余的周期定时器：TIM6(5ms)=Motion_Task、TIM7(5ms)=Servo_Task，而且都由
 * bsp_periodic_start() 在初始化时统一启动。这里原来写的是 HAL_TIM_Base_Start_IT(&htim2)——
 * 而 htim2 只是 TIM6 的别名，于是 Start 没事（TIM6 本来就在跑）、**Stop 会把整个 5ms 运动节拍
 * 停掉**（角度环/速度环一起停），语义完全不对。
 *
 * 现在 Start/Stop 只翻一个软件开关，真正的执行交给 Motion_Task()（TIM6 5ms 中断）：
 * 每 PID_TRACE_BG_DIV(20) 拍跑一次 = 100ms = 旧板 10Hz。必须分频——同一个 PID
 * （pid.c 的 PID_Compute，含 Kd·Δe 和 Ki·Σe）采样率快了 20 倍，D 项会强 20 倍、I 项弱 20 倍。
 * ------------------------------------------------------------------------- */
static uint8_t s_bg_on  = 0U;
static uint8_t s_bg_div = 0U;

void PID_Tracing_Timer_Start(void)
{
    s_bg_on  = 1U;
    s_bg_div = 0U;
}

void PID_Tracing_Timer_Stop(void)
{
    s_bg_on  = 0U;
    s_bg_div = 0U;
}

uint8_t PID_Tracing_BackgroundOn(void)
{
    return s_bg_on;
}

uint8_t PID_Tracing_BackgroundTask(void)
{
    if (!s_bg_on) return 0U;

    if (++s_bg_div < PID_TRACE_BG_DIV) return 0U;
    s_bg_div = 0U;

    motor_tracing_pid_0_task();
    return 1U;
}

