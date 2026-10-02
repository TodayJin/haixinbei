#ifndef _SERVO_H
#define _SERVO_H

#include "board.h"
#include "bsp_board.h"
#include "stm32g4xx_hal.h"
#include "main.h"

/* LDX-227（PWM 数字舵机）依据规格书/教程：
 *   脉宽 500~2500us，中点 1500us，死区 4us，频率 50~330Hz（本板 50Hz）；
 *   500 -> 0°，2500 -> 180°（规格书写的是 270° 行程，四份教程都按 180° 举例，
 *   装机标定前先实测满行程）；控制量是位置不是速度。
 * 中位偏差是**纯软件偏置**：脉宽 = 角度对应脉宽 + deviation（±100us），舵机里存不了。 */

#define SERVO_PULSE_MIN     500     /* us */
#define SERVO_PULSE_MAX     2500    /* us */
#define SERVO_PULSE_CENTER  1500    /* us */
#define SERVO_DEVIATION_MAX 100     /* us，规格书/教程给的偏差范围 */
#define SERVO_ANGLE_MAX     180.0f  /* 教程口径的行程 */

extern TIM_HandleTypeDef htim1;

void Servo_Init(void);
void Servo_Control(int x, uint8_t angle);            /* 旧接口：角度 0..180，偏差用当前设定 */
void Servo_Move(int x);                              /* 旧接口：按动作编号摆位（保留比赛动作表） */

/* 动作表接口（用户 m13303：舵机测试页要能按号选动作）
 * 动作编号 1..SERVO_ACTION_N，与旧代码 Servo_Move() 的 case 一一对应；
 * 终点姿态只有一处真值来源（servo.c 的 k_action_pose[]）。 */
#define SERVO_ACTION_N 14
int   Servo_ActionCount(void);                       /* = SERVO_ACTION_N */
void  Servo_ActionPose(int x, int *a1, int *a2);      /* 取第 x 个动作的终点姿态（不动舵机） */
void  Servo_ApplyAction(int x);                      /* 只下发终点姿态（不含 5/9 号的中间延时） */
const char *Servo_ActionName(int x);                 /* 动作名（给界面显示） */
float Servo_GetAngle(int x);                         /* 当前角度档（1/2 号），用于界面回读同步 */

void Servo_SetPulse(int x, int pulse_us, int dev_us);/* 直接给脉宽（内部会限幅到 500..2500） */
void Servo_SetAngleDev(int x, float angle, int dev_us);
void Servo_SetAngle(int x, float angle);
void Servo_SetDeviation(int x, int dev_us);          /* 设中位偏差（±100us），立即生效 */
int  Servo_GetDeviation(int x);

/* 非阻塞缓动：按文档"软件逐级逼近脉宽"的做法，每 SERVO_STEP_MS 走一步 */
#define SERVO_STEP_MS 20U
void Servo_MoveTo(int x, float angle, uint32_t ms);  /* 用 ms 时间走到 angle */
void Servo_Task(void);                               /* 由 TIM7(5ms) 回调驱动 */

#endif
