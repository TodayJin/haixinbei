#include "pid.h"
/* 位置式PID计算 */
float PID_Compute(float setpoint, float current_value)
{
    // 静态变量用于保持积分项和上一次误差，实现连续计算
    static float integral = 0.0f;
    static float last_error = 0.0f;
    // 计算当前误差
    float error = setpoint - current_value;
    float output;

    // PID参数（请根据实际调整）
    float Kp = 375.0f; // 比例系数
    float Ki = 1.5f; // 积分系数
    float Kd = 50.0f; // 微分系数

    /**
     * @brief 积分限幅值（integral_limit）的设置方法
     *
     * 通常，积分限幅的大小应根据系统的实际需求和积分饱和可能带来的影响来确定。
     * 一般做法如下：
     * 1. 根据期望的最大输出或系统允许的最大误差积分量来设定。
     * 2. 可通过仿真或实验，观察积分项对系统响应的影响，逐步调整限幅值，防止积分饱和导致系统震荡或响应变慢。
     * 3. 经验上，积分限幅可以设置为输出最大值的10%~50%，具体比例需结合实际系统调试。
     * 4. 也可根据积分项的单位和实际误差范围，估算积分项在正常工作时的合理上限。
     */
    float integral_limit = 50.0f; // 积分限幅，防止积分项过大导致积分饱和

    // 积分项计算并限幅
    integral += error; // 累加误差到积分项
    if (integral > integral_limit)
        integral = integral_limit; // 积分上限
    else if (integral < -integral_limit)
        integral = -integral_limit; // 积分下限

    // PID输出计算
    // 比例项：Kp * error
    // 积分项：Ki * integral
    // 微分项：Kd * (error - last_error)
    output = Kp * error + Ki * integral + Kd * (error - last_error);

    // 保存本次误差，为下次微分项计算做准备
    last_error = error;

    // 返回PID控制器输出
    return output;
}
