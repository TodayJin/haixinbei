#include "tracing.h"

#include "motor.h"
#include "trace_cal.h"   /* 标定过的 IIC 模拟量优先；没有就回落数字口 */

int tracing_data[8];
uint8_t trace_invert = 0;     /* 0 = 压黑线为高（本模块默认）；1 = 反相 */

int trace_read_channel(uint8_t idx)
{
    uint8_t v;
    int a;

    if (idx > 7U) return 0;

    /* 优先用灰度模块的 IIC 模拟量（trace_cal.c）：
     * 做过黑白两点标定后，每路有自己的阈值和极性，比数字口那个固定比较器稳。
     * trace_cal_channel() 返回 -1 = 没标定 / 模拟量不通 -> 回落 PA0–PA7 数字口。 */
    a = trace_cal_channel(idx);
    if (a < 0) {
        v = (bsp_trace_bit(idx) == GPIO_PIN_SET) ? 1U : 0U;
    } else {
        v = (uint8_t)a;
    }
    if (trace_invert) v = v ? 0U : 1U;
    return (int)v;
}

int trace_on_line_count(void)
{
    int i, n = 0;

    for (i = 0; i < 8; i++) {
        if (trace_read_channel((uint8_t)i)) n++;
    }
    return n;
}

void read_tracing_data(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        tracing_data[i] = trace_read_channel((uint8_t)i);
    }
}

void read_tracing_data_mv(void)
{
    /* 模拟量/偏移量版本：本板走的是数字量 OUT1..OUT8，这里留空 */
}

void show_tracing_data(void)
{
    /* 显示交给 KK_UI（app_ui.c），旧板这里在 OLED 上画条 */
}

/**
 * @brief 旧板的开关式循迹（无 PID）：保留下来当备用/对照
 */
void motor_tracing(int speed_zhi,int speed_cha)
{
	    // 循迹逻辑
    if ((tracing_data[2] == 1) && (tracing_data[3] == 1) && (tracing_data[4] == 1) && (tracing_data[5] == 1)) {
        Set_Pwm(1, speed_zhi, speed_zhi);  // 直行
    }
    else if ((tracing_data[3] != 0) && (tracing_data[4] != 0)) {
        Set_Pwm(1, speed_zhi, speed_zhi);  // 直行
    }
    else if ((tracing_data[3] != 0) && (tracing_data[4] == 0)) {  // 右偏，左转
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[3] == 0) && (tracing_data[4] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[2] != 0) && (tracing_data[3] != 0)) {  // 右偏，左转
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[4] != 0) && (tracing_data[5] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[1] != 0) && (tracing_data[2] != 0)) {  // 右偏，左转
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[5] != 0) && (tracing_data[6] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[0] != 0) && (tracing_data[1] != 0)) {  // 右偏，左转
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[6] != 0) && (tracing_data[7] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, 0);
    }
    else if ((tracing_data[1] != 0) && (tracing_data[2] == 0)) {  // 右偏，左转
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[5] == 0) && (tracing_data[6] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, 0);
    }
    else if ((tracing_data[0] != 0) && (tracing_data[1] == 0)) {  // 右偏，左转
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[6] == 0) && (tracing_data[7] != 0)) {  // 左偏，右转
        Set_Pwm(1, speed_zhi, 0);
    }
	else if (tracing_data[2] == 1)  {  // 左偏，右转
        Set_Pwm(1, speed_cha, speed_zhi);
    }
	else if (tracing_data[5] == 1)  {  // 左偏，右转
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else {  // 无传感器信号，停止或直行
        Set_Pwm(2, 290, 290);
    }
}
