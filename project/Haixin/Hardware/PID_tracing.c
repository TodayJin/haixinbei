#include "PID_tracing.h"
#include "OLED.h"
#include "motor.h"
#include "pid.h"
/*
* 获取PID循迹数据
*/
static float last_data = 4.5,data=4.5; // 上次读取的数据，初始化为中心值4.5
static uint8_t all_sensors_lost = 0; // 标记是否所有传感器都失去信号
static float last_pid_output = 0; // 保存上一次的PID输出

void read_tracing_data_pid()
{
    float num=0;
    uint8_t L1=0,L2=0,L3=0,L4=0,R1=0,R2=0,R3=0,R4=0;
    uint8_t all_lost = 1; // 假设所有传感器都失去信号

    if (HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_4) == GPIO_PIN_SET) {L1 = 1;num++;all_lost = 0;}else {L1 = 0;}
    if (HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_5) == GPIO_PIN_SET) {L2 = 1;num++;all_lost = 0;}else {L2 = 0;}
    if (HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_4) == GPIO_PIN_SET) {L3 = 1;num++;all_lost = 0;}else {L3 = 0;}
    if (HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_5) == GPIO_PIN_SET) {L4 = 1;num++;all_lost = 0;}else {L4 = 0;}
    if (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_0) == GPIO_PIN_SET) {R1 = 1;num++;all_lost = 0;}else {R1 = 0;}
    if (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_1) == GPIO_PIN_SET) {R2 = 1;num++;all_lost = 0;}else {R2 = 0;}
    if (HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_7) == GPIO_PIN_SET) {R3 = 1;num++;all_lost = 0;}else {R3 = 0;}
    if (HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_8) == GPIO_PIN_SET) {R4 = 1;num++;all_lost = 0;}else {R4 = 0;}

    all_sensors_lost = all_lost; // 更新所有传感器失去信号的标记

     // 只有当至少有一个传感器检测到信号时才更新data
    if(num > 0) {
        data=(L1*1+L2*2+L3*3+L4*4+R1*5+R2*6+R3*7+R4*8)/num;
        last_data = data;
    }
    // 当所有传感器都失去信号时，保持上一次的值
    // 不需要额外处理，因为data会自动保持上一次的值

}
void show_tracing_data_pid()
{
    // 显示PID循迹数据
    OLED_ShowNum(64,32,data*10,3,OLED_8X16); 
}
void motor_tracing_pid(float target)
{
    int speed = 1000; // 设定速度
    read_tracing_data_pid(); // 读取循迹数据

    //show_tracing_data_pid(); // 显示循迹数据

    float pid_output;
    
    // 当所有传感器都失去信号时，使用上一次的PID输出值
    if(all_sensors_lost) {
        pid_output = last_pid_output; // 使用上一次的PID输出保持方向
    } else {
        pid_output = PID_Compute(target, data); // 计算PID输出
        last_pid_output = pid_output; // 保存当前PID输出供丢失信号时使用
    }
 
    float speed_left = speed  - pid_output;
    float speed_right = speed + pid_output;

    Load(speed_left,speed_right);
}

// 基于定时器2的PID循迹控制任务函数
void motor_tracing_pid_0_task(void)
{
    // 使用速度为0的PID循迹控制
    motor_tracing_pid_0(4.5);
}

/**
 * @brief 启动PID循迹控制定时器
 * 使用TIM2定时器每10ms中断一次
 */
void PID_Tracing_Timer_Start(void)
{
    // 启动定时器
    HAL_TIM_Base_Start_IT(&htim2);
}

/**
 * @brief 停止PID循迹控制定时器
 */
void PID_Tracing_Timer_Stop(void)
{
    // 停止定时器
    HAL_TIM_Base_Stop_IT(&htim2);
}

void motor_tracing_pid_0(float target)
{
    int speed = 200; // 设定速度
    read_tracing_data_pid(); // 读取循迹数据

    show_tracing_data_pid(); // 显示循迹数据

    float pid_output;
    
    // 当所有传感器都失去信号时，使用上一次的PID输出值
    if(all_sensors_lost) {
        pid_output = last_pid_output; // 使用上一次的PID输出保持方向
    } else {
        pid_output = PID_Compute(target, data); // 计算PID输出
        last_pid_output = pid_output; // 保存当前PID输出供丢失信号时使用
    }
 
    float speed_left = speed  - pid_output;
    float speed_right = speed + pid_output;

    Load(speed_left,speed_right);
}

