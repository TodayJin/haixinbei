// gyro_control.c
#include "gyro_control.h"
#include "motor.h"
#include "jy61p.h"

// PID控制器实现
PID_Controller gyro_pid;

extern TIM_HandleTypeDef htim4;

extern TIM_HandleTypeDef htim2;
// 控制状态标志
static uint8_t gyro_control_enabled = 0;

// PID参数 (需要根据实际情况调整)
#define GYRO_KP 100.0f
#define GYRO_KI 1.5f
#define GYRO_KD 10.0f

// 基础速度变量
static int base_speed_left = 0;
static int base_speed_right = 0;

// 添加目标角度变量，用于记录启动时的角度作为目标角度
static float target_angle = 0.0f;
static uint8_t angle_initialized = 0;

// 转向角度偏移量
#define TURN_ANGLE_OFFSET 90.0f

/**
 * @brief 角度规范化函数，将角度限制在 [-180, 180] 范围内
 * @param angle 输入角度
 * @return 规范化后的角度
 */
float NormalizeAngle(float angle)
{
    while (angle > 180.0f) {
        angle -= 360.0f;
    }
    while (angle < -180.0f) {
        angle += 360.0f;
    }
    return angle;
}

/**
 * @brief 计算角度差值，考虑角度环绕特性
 * @param target 目标角度
 * @param actual 实际角度
 * @return 角度差值
 */
float CalculateAngleError(float target, float actual)
{
    float error = target - actual;
    error = NormalizeAngle(error);
    return error;
}

/**
 * @brief 初始化PID控制器
 */
void PID_Init(PID_Controller* pid, float kp, float ki, float kd)
{
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    pid->target = 0.0f;
    pid->actual = 0.0f;
    pid->error = 0.0f;
    pid->last_error = 0.0f;
    pid->integral = 0.0f;
    pid->output = 0.0f;
}

/**
 * @brief PID计算
 * @param pid PID控制器指针
 * @param target 目标值
 * @param actual 实际值
 * @param dt 时间间隔(s)
 * @return PID输出值
 */
float PID_Compute_z(PID_Controller* pid, float target, float actual, float dt)
{
    pid->target = target;
    pid->actual = actual;
    
    // 使用角度差值计算，考虑角度环绕特性
    pid->error = CalculateAngleError(target, actual);
    
    // 积分
    pid->integral += pid->error * dt;
    
    // 积分限幅防止积分饱和
    if(pid->integral > 1000.0f) pid->integral = 1000.0f;
    if(pid->integral < -1000.0f) pid->integral = -1000.0f;
    
    // 微分
    float derivative = (pid->error - pid->last_error) / dt;
    
    // PID计算
    pid->output = pid->kp * pid->error + 
                  pid->ki * pid->integral + 
                  pid->kd * derivative;
    
    pid->last_error = pid->error;
    
    return pid->output;
}

/**
 * @brief 初始化陀螺仪控制
 */
void GyroControl_Init(void)
{
    // 初始化PID控制器
    PID_Init(&gyro_pid, GYRO_KP, GYRO_KI, GYRO_KD);
    
    // 重置角度初始化标志
    angle_initialized = 0;
    
    gyro_control_enabled = 0;
}

/**
 * @brief 启动陀螺仪控制
 */
void GyroControl_Start(void)
{
    // 在启动时记录当前角度作为目标角度
    if (!angle_initialized) {
        target_angle = NormalizeAngle(Yaw);  // 使用Yaw角度而不是Gyro_Z角速度，并规范化角度
        angle_initialized = 1;
    }
    
    gyro_control_enabled = 1;
}

/**
 * @brief 停止陀螺仪控制
 */
void GyroControl_Stop(void)
{
    gyro_control_enabled = 0;
    // 不再主动停止电机，让主控制逻辑接管
    // Load(0, 0);
}

/**
 * @brief 设置基础速度
 * @param left 左轮速度
 * @param right 右轮速度
 */
void GyroControl_SetBaseSpeed(int left, int right)
{
    base_speed_left = left;
    base_speed_right = right;
}

/**
 * @brief 陀螺仪控制任务 (在定时器中断中调用)
 */
void GyroControl_Task(void)
{
    if (!gyro_control_enabled) return;
    
    // 假设定时器每10ms执行一次，即dt = 0.01s
    static float dt = 0.01f;
    
    // 获取Yaw角度，目标是保持启动时的角度
    float actual_angle = NormalizeAngle(Yaw);  // 规范化实际角度
    
    // PID计算，以启动时的角度为目标角度
    float correction = PID_Compute_z(&gyro_pid, target_angle, actual_angle, dt);
    
    // 根据PID输出调整左右轮速度
    int left_speed = base_speed_left - (int)correction;
    int right_speed = base_speed_right + (int)correction;
    
    // 限制速度范围
    if(left_speed > 7200) left_speed = 7200;
    if(left_speed < -7200) left_speed = -7200;
    if(right_speed > 7200) right_speed = 7200;
    if(right_speed < -7200) right_speed = -7200;
    
    // 应用速度到电机
    Load(left_speed, right_speed);
}

/**
 * @brief 启动陀螺仪控制定时器
 * 使用TIM4定时器每10ms中断一次
 */
void Gyro_Timer_Start(void)
{
    // 假设TIM4已经初始化，不需要再初始化
    // 假设定时器周期已经设置好，不需要再设置
    
    // 启动定时器
    HAL_TIM_Base_Start_IT(&htim4);  // 根据实际使用的定时器修改
}


/**
 * @brief 停止陀螺仪控制定时器
 */
void Gyro_Timer_Stop(void)
{
    // 停止定时器
    HAL_TIM_Base_Stop_IT(&htim4);   // 根据实际使用的定时器修改
}

/**
 * @brief 向左转（调整目标角度）
 * @param angle 左转的角度（相对于当前目标角度）
 */
void GyroControl_TurnLeft(float angle)
{
    if (gyro_control_enabled) {
        target_angle = NormalizeAngle(target_angle - angle);  // 减少目标角度实现左转，并规范化角度
    }
}

/**
 * @brief 向右转（调整目标角度）
 * @param angle 右转的角度（相对于当前目标角度）
 */
void GyroControl_TurnRight(float angle)
{
    if (gyro_control_enabled) {
        target_angle = NormalizeAngle(target_angle + angle);  // 增加目标角度实现右转，并规范化角度
    }
}

/**
 * @brief 左转固定角度（默认10度）
 */
void GyroControl_TurnRightFixed(void)
{
    GyroControl_TurnLeft(TURN_ANGLE_OFFSET);
}

/**
 * @brief 右转固定角度（默认10度）
 */
void GyroControl_TurnLeftFixed(void)
{
    GyroControl_TurnRight(TURN_ANGLE_OFFSET);
}
