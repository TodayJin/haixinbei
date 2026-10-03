#include "pid.h"
#include "host_link.h"   /* PID 参数统一放在 host_link 的参数表里（HP_LINE_*） */

/* 状态从原来的"函数内 static"提到文件作用域，只为了能提供 PID_Reset()：
 * 丢线又找回、或者重新起步时，上一次的积分和上次误差必须清掉，否则会攒出
 * 一个很大的转向冲击（原来没有任何清零入口）。 */
static float s_integral;
static float s_last_error;
static int   s_has_last;      /* 是否已经有过上一次误差（决定要不要算微分） */
static int   s_pivot;         /* 1 = 当前按"原地 pivot"那组增益算 */

/* ---------------------------------------------------------------------------
 * 原地 pivot（base_speed == 0，左右轮反向对转、车不前进）专用增益。
 *
 * 为什么不能沿用前进那组：前进循迹的增益是按"车在往前走、探头以车速扫过线"
 * 标定的；原地转时车不动、只有车身在转，同一个重心偏差对应的车身转角大得多
 * （台架实测 0.016~0.216 单位/度，强非线性）。2026-10-02 实测：用旧那组
 * Kp=2700 追一个 0.9 单位的偏差，车转过头 42° 并把线甩出探头。
 *
 * 这组值怎么来的（全套记录见 docs/自主优化记录.md 第 15 节）：
 *   · 网格：Kp ∈ {600,900,1200} × Kd ∈ {0.15,0.30,0.45}×Kp，Ki=0，
 *     先用循迹环把线停在 4.5+0.6（偏置 0.6 单位）静置，再阶跃回 4.5；
 *   · 决赛圈：4 组各重复 3 次（0.6 单位扰动），只有 Kp600/Kd270 与 Kp900/Kd405
 *     三次残差全部 ≤0.05；Kp1200/Kd180 有一把停偏 0.24；
 *   · 鲁棒性：Kp900/Kd405 在 1.0 单位扰动下 1.089 → −0.057、峰值命令 918mm/s；
 *     设定值来回切换（4.5→5.2→3.8→4.5）四段末值 0.012 / −0.074 / −0.038 / 0.011，
 *     峰值 1760mm/s ≈ 满量程 24%，全程没丢线；
 *   · Ki 取 0：偏置 0.9 单位 + 3000ms 时 Ki=10.8 会积分 windup，实测把车甩飞 −131.9°。
 * 保守备选（更温柔、峰值命令只有 ~340mm/s）：Kp=600 / Ki=0 / Kd=270。
 * --------------------------------------------------------------------------- */
#define PID_PIVOT_KP  900.0f
#define PID_PIVOT_KI    0.0f
#define PID_PIVOT_KD  405.0f

void PID_SetPivot(int on)
{
    int want = (on != 0) ? 1 : 0;

    if (want == s_pivot) return;   /* 没换模式就别清：这个函数每 5ms 被调一次 */
    s_pivot = want;
    PID_Reset();                   /* 换了增益 = 换了被控对象，积分/微分历史必须清 */
}

void PID_Reset(void)
{
    s_integral   = 0.0f;
    s_last_error = 0.0f;
    s_has_last   = 0;
}

/* 位置式PID计算 */
float PID_Compute(float setpoint, float current_value)
{
    // 计算当前误差
    float error;
    float output;
    float Kp, Ki, Kd, integral_limit, d_term;

    /* 输入保护：NaN/Inf 一旦进到这里，后面 (int) 强转就是未定义行为，
     * 输出会变成野值（实测能把 PWM 打到满）。宁可这一拍不出力。 */
    if (!(setpoint == setpoint) || !(current_value == current_value) ||
        setpoint > 1.0e9f || setpoint < -1.0e9f ||
        current_value > 1.0e9f || current_value < -1.0e9f) {
        return 0.0f;
    }

    error = setpoint - current_value;

    // PID参数：从参数表实时取（菜单「PID -> 循迹环」或上位机都能改），
    // 取不到时用兜底值 —— 兜底值 = 旧固件硬编码值 ×7.2（旧板满量程 1000、本板 7200，
    // 这个 PID 的输出是直接加在左右轮上的 PWM 偏置，所以增益要按量程折算；
    // 积分限幅不折算，它的量纲是误差本身）。详见 app_ui.c 的 g_cfg 默认值注释。
    // 注意：这套值是从旧代码抄来的、单位与硬件都变了，用户已明确说"不能再当基准"，
    // 前进循迹的增益要等能直行时重新标；原地 pivot 用下面那组实测值。
    if (s_pivot) {
        /* pivot 增益也进了参数表（上位机「调参 → 循迹环」下能改、能固化），
         * 表里的值坏掉时回落到上面实测出来的兜底值。 */
        Kp = host_param_get(HP_PIVOT_KP, PID_PIVOT_KP);
        Ki = host_param_get(HP_PIVOT_KI, PID_PIVOT_KI);
        Kd = host_param_get(HP_PIVOT_KD, PID_PIVOT_KD);
    } else {
        Kp = host_param_get(HP_LINE_KP, 2700.0f); // 比例系数（旧 375.0 ×7.2）
        Ki = host_param_get(HP_LINE_KI,   10.8f); // 积分系数（旧   1.5 ×7.2）
        Kd = host_param_get(HP_LINE_KD,  360.0f); // 微分系数（旧  50.0 ×7.2）
    }

    /**
     * @brief 积分限幅值（integral_limit）的设置方法
     *
     * 通常，积分限幅的大小应根据系统的实际需求和积分饱和可能带来的影响来确定。
     * 一般做法如下：
     * 1. 根据期望的最大输出或系统允许的最大误差积分量来设定。
     * 2. 可通过仿真或实验，观察积分项对系统响应的影响，逐步调整限幅值，防止积分饱和导致系统震荡或响应变慢。
     * 3. 经验上，积分限幅可以设置为输出最大值的10%~50%，具体比例需结合实际调试。
     * 4. 也可根据积分项的单位和实际误差范围，估算积分项在正常工作时的合理上限。
     */
    integral_limit = host_param_get(HP_LINE_ILIMIT, 50.0f); // 积分限幅，防止积分项过大导致积分饱和
    if (!(integral_limit > 0.0f)) integral_limit = 0.0f;    /* NaN/负值当 0 处理 */

    // 积分项计算并限幅
    s_integral += error; // 累加误差到积分项
    if (s_integral > integral_limit)
        s_integral = integral_limit; // 积分上限
    else if (s_integral < -integral_limit)
        s_integral = -integral_limit; // 积分下限

    // PID输出计算
    // 比例项：Kp * error
    // 积分项：Ki * integral
    // 微分项：Kd * (error - last_error)
    //   第一次算（含每次 PID_Reset 之后）不做微分：否则 Kd×(误差-0) 会变成一个
    //   很大的瞬时冲击（Kd=360、误差 5 就是 1800 PWM），起步/重新上线那一下就甩头。
    d_term = s_has_last ? (Kd * (error - s_last_error)) : 0.0f;
    output = Kp * error + Ki * s_integral + d_term;

    // 保存本次误差，为下次微分项计算做准备
    s_last_error = error;
    s_has_last   = 1;

    // 返回PID控制器输出
    return output;
}
