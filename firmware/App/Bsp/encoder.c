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

    /* ---- 台架标定（2026-09-30，手推试验）----
     * 把两轮都朝【车头 / 前进】方向转动，实测：左编码器累计 -21676、右编码器 +14623。
     * 即左右两条编码器的"正方向"约定是相反的（左右电机镜像安装，同一电气极性下
     * 物理转向本就相反，编码器跟着反向）。
     * 这里把左路翻正，使得约定统一为：**车轮朝前进方向滚 -> 两路计数都为正**。
     * 右路不动，配套的右电机符号翻转在 motor.c 的 Load() 里，两处必须一起改。
     *
     * ★2026-10-15（换 MG513X 之后实测）：**两路都要翻**。
     *  现象（直连串口、不经过上位机实测）：
     *    只驱动左轮 +4000 ⇒ vl = **-345** mm/s
     *    只驱动右轮 +3000 ⇒ vr = **-144** mm/s
     *  即"命令正 ⇒ 两路计数都负" ⇒ 速度环变成**正反馈**：
     *  要正计数、拿到负计数 ⇒ 误差越修越大 ⇒ 猛加油/松油门来回跳
     *  （实测电流 300↔3500mA 反复、PWM 一会满一会 0、转速 0↔380 跳）。
     *  物理原因：新电机的编码器 A/B 与旧电机相反（用户：换电机时接线没变，
     *  只有 A/B 可能调换）⇒ 正交计数方向整体反过来。
     *  ⇒ 这里把**右路也翻正**，恢复"车轮朝前进方向滚 ⇒ 两路计数都为正"的约定。
     *  ⚠ 改这里不影响电机转向（那是 motor.c 的 Load()），只把**反馈符号**改对。 */
    l = -l;
    r = -r;

    if (left  != 0) *left  = l;
    if (right != 0) *right = r;
}
