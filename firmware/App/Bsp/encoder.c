/**
 * @file  encoder.c
 * @brief 编码器测速（TIM3 = 电机 A / TIM4 = 电机 B，编码器模式 4 倍频）
 *
 * 注意命名：本工程里 htim4 这个名字被旧上层当成 TIM7 的别名用掉了
 * （见 bsp_board.c 的 htim4.Instance = TIM7），所以 TIM4 编码器句柄叫 htim4_enc。
 */
#include "encoder.h"
#include "bsp_board.h"

void Encoder_Init(void)
{
    Encoder_Reset();
    (void)HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL);
    (void)HAL_TIM_Encoder_Start(&htim4_enc, TIM_CHANNEL_ALL);
}

void Encoder_Reset(void)
{
    __HAL_TIM_SET_COUNTER(&htim3, 0U);
    __HAL_TIM_SET_COUNTER(&htim4_enc, 0U);
}

int Read_Speed(TIM_HandleTypeDef *htim)
{
    int16_t cnt;

    if (htim == 0) return 0;
    cnt = (int16_t)__HAL_TIM_GET_COUNTER(htim);   /* 读成有符号：编码器模式下 CNT 会上下计数 */
    __HAL_TIM_SET_COUNTER(htim, 0U);
    return (int)cnt;
}

void Encoder_ReadDelta(int *left, int *right)
{
    int l = Read_Speed(&htim3);
    int r = Read_Speed(&htim4_enc);

    /* ---- 左右编码器的正方向约定（★2026-10-15 定稿，别改反）----
     * 硬件事实：左右电机是**镜像安装** ⇒ 同一电气极性下两轮物理转向相反，
     *   所以**驱动的 PWM 符号本来就必须相反**。真车实测（用户在上位机操控页按"前进"）：
     *   显示 pwm 左 +7200 / 右 −7200，而**两个轮子都朝前** ✓ ⇒ 这是正确约定，不是 bug。
     * 编码器跟着镜像 ⇒ 原始计数方向也相反 ⇒ **只需要把左路翻正**，
     *   就能得到统一约定："轮子朝前进方向滚 ⇒ 两路计数都为正"。
     *   （台架手推实测：两轮都朝前推，左累计 −21676、右 +14623。）
     *
     * ⚠ 别犯这个错：看到"命令正 ⇒ 两路计数都负"就以为两路都反了。
     *   2026-10-15 一度改成 `l = -l; r = -r;`（commit 6d0b397），把镜像误当成错误 ⇒
     *   **右路反馈符号被翻反 ⇒ 右轮速度环成正反馈** ⇒ 实测症状：
     *     · 右路输出乱摆（pwm_r 一会满一会 0，多数帧 0）
     *     · 编码器读数乱跳（0 或 7 这类随机小值）
     *     · 速度完全不受控、PWM 打满而电流只有静止值
     *   ⇒ 2026-10-15 二次实测又把它改回两路都翻（见下）。
     * 右电机的输出符号翻转在 motor.c 的 Load() 里，与本处配套。 */
    /* ★2026-10-15 定案（直连串口实测，带符号的 vl）：
     *   l = -l 这个翻转对**现役 MG513X 是错的**，必须去掉 —— 原始编码器方向就是对的
     *   （前进 = 计数为正）。实测证据（kp=0 纯前馈、单轮）：
     *     发正目标 +3000/+4000/+6000 ⇒ 带符号 vl = **-546/-855/-869** ⇒ 轮子倒转 ✗
     *     而且越跑越快（正反馈）：环为了拿到正计数，一直往"倒转"方向加油。
     *   之前以为"手推前进读数为负" ⇒ 那是**翻过之后**的读数（GUI 显示的 s_mmps），
     *   不是原始值 ⇒ 据此加的翻转是错的。
     *   ⇒ 现在两路都不翻，约定 = "轮子朝前进方向滚 ⇒ 计数为正"（与 Load() 的右路取负配套）。 */
    if (left  != 0) *left  = l;
    if (right != 0) *right = r;
}
