/**
 * @file  motor.c
 * @brief 电机驱动（2xAT8236）：对外 API 与旧板一致，只换底层
 *
 * 旧板：每电机 1 路 PWM + 2 个方向 GPIO。
 * 新板：AT8236 每电机两根输入（IN1/IN2）都是 PWM 输入，没有独立方向脚。
 *      正速度 -> IN1 出 PWM、IN2 恒 0；负速度 -> IN1 恒 0、IN2 出 PWM。
 *      PWM 刻度保持旧板 0-7200（TIM8 PSC=0/ARR=7199 @170MHz -> 23.6kHz）。
 */
#include "motor.h"
#include "bsp_board.h"

#define PWM_MAX  MOTOR_PWM_MAX
#define PWM_MIN  MOTOR_PWM_MIN

extern TIM_HandleTypeDef htim8;
extern uint8_t stop;

/* 2026-10-03：这里原来有一个非 static 的 `int abs(int p)`（旧板沿用下来的），
 * 全工程没有任何调用者，却在链接期**顶掉了标准库的 abs()**：
 *   - 任何别的文件（含 newlib 内部）调 abs() 都会绑到这个版本，传 long/float 会被静默截成 int；
 *   - 谁哪天用到 labs()（newlib 里和 abs 同一个目标文件）就会直接撞成重复符号链接错误。
 * 已验证全工程只有定义、没有调用，故删除；要绝对值请直接用 <stdlib.h> 的 abs() 或 fabsf()。 */

void Motor_Init(void)
{
	bsp_motor_init();
}

/* ---- 台架标定（2026-09-30）----
 * 实测 Load(+,+) 时【左轮前进、右轮后退】：左右电机是镜像安装的，同一电气极性下
 * 两轮物理转向本来就该相反，原来固件没补偿这一点。
 * 这里把右路 B 的符号翻过来，让 Load 的对外语义统一成
 *     正值 = 该轮朝【车头 / 前进】方向转
 * 配套的编码器侧翻转在 encoder.c 的 Encoder_ReadDelta() 里（左路取负），两处必须一起改。
 * 标定依据：+6000 时两路编码器都读负；手推前进时左编码器 -21676 / 右编码器 +14623。
 * 翻完再代回去：Load(+) -> 左轮前进、右轮前进；两路编码器都为 +。 */
void Load(int moto1, int moto2)
{
	int a = moto1;
	int b = -moto2;
	Limit(&a, &b);

	__HAL_TIM_SetCompare(&htim8, TIM_CHANNEL_1, (a > 0) ? (uint32_t)a : 0U);
	__HAL_TIM_SetCompare(&htim8, TIM_CHANNEL_2, (a < 0) ? (uint32_t)(-a) : 0U);
	__HAL_TIM_SetCompare(&htim8, TIM_CHANNEL_3, (b > 0) ? (uint32_t)b : 0U);
	__HAL_TIM_SetCompare(&htim8, TIM_CHANNEL_4, (b < 0) ? (uint32_t)(-b) : 0U);
}

void Limit(int *motoA, int *motoB)
{
	if (*motoA > PWM_MAX) *motoA = PWM_MAX;
	if (*motoA < PWM_MIN) *motoA = PWM_MIN;
	if (*motoB > PWM_MAX) *motoB = PWM_MAX;
	if (*motoB < PWM_MIN) *motoB = PWM_MIN;
}

void Set_Pwm(int mode, uint16_t left_speed, uint16_t right_speed)
{
	switch (mode)
	{
		case 1: Load(left_speed, right_speed); break;
		case 2: Load(-(int)left_speed, -(int)right_speed); break;
		case 3: Load(-(int)left_speed, (int)right_speed); break;
		case 4: Load((int)left_speed, -(int)right_speed); break;
		case 5: Load(0, 0); break;
		default: break;
	}
}
