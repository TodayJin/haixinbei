#include <stdint.h>
#include "jy61p.h"

static uint8_t RxBuffer[11]; /*接收数据数组*/
static volatile uint8_t RxState = 0; /*接收状态标志位*/
static uint8_t RxIndex = 0; /*接受数组索引*/
float Roll, Pitch, Yaw; /*角度信息*/

float Gyro_Z = 0.0f;  // Z轴角速度变量

/* 成功解析出来的帧数（0x53 角度 / 0x52 角速度都算一帧）。
 * 给上层做"陀螺仪是不是还在说话"的活性判据用：只有在**校验通过**时才加一，
 * 所以拿它配时间戳就能区分"数据在更新"和"只有噪声/线断了"。 */
static volatile uint32_t s_frames = 0U;

uint32_t jy61p_frame_count(void)
{
    return s_frames;
}

/**
 * @brief       数据包处理函数
 * @param       串口接收的数据RxData
 * @retval      无
 */
void jy61p_ReceiveData(uint8_t RxData)
{
    uint8_t i,sum=0;
    
    if (RxState == 0)   //等待包头
    {
        if (RxData == 0x55) //收到包头
        {
            RxBuffer[RxIndex] = RxData;
            RxState = 1;
            RxIndex = 1; //进入下一状态
        }
    }
    
    else if (RxState == 1)
    {
        // 修改这里可以选择读取不同类型的数据
        // 0x53为角度输出，0x52为角速度输出
        if (RxData == 0x53 || RxData == 0x52)
        {
            RxBuffer[RxIndex] = RxData;
            RxState = 2;
            RxIndex = 2; //进入下一状态
        }
        else
        {
            /* ★2026-10-16(#16 审计): 半帧残留 —— 类型字节不认识时必须回状态 0，
             * 否则后续字节会被当类型字节吞掉，最坏连丢 2~3 帧 ⇒ 500ms 失联判据误报'陀螺失联'停车。 */
            RxState = 0;
            RxIndex = 0;
            if (RxData == 0x55)   /* 正好是包头：立刻按新帧重入 */
            {
                RxBuffer[0] = 0x55;
                RxState = 1;
                RxIndex = 1;
            }
        }
    }
    
    else if (RxState == 2)  //接收数据
    {
        RxBuffer[RxIndex++] = RxData;
        if(RxIndex == 11)   //接收完成
        {
            for(i=0;i<10;i++)
            {
                sum = sum + RxBuffer[i]; //计算校验和
            }
            if(sum == RxBuffer[10])     //校验成功
            {
                s_frames++;
                // 根据数据类型解析数据
                if(RxBuffer[1] == 0x53)  // 角度数据
                {
                    // 使用int16_t正确解析有符号数据
                    Roll = ((int16_t)(RxBuffer[3] << 8 | RxBuffer[2])) / 32768.0f * 180.0f;
                    Pitch = ((int16_t)(RxBuffer[5] << 8 | RxBuffer[4])) / 32768.0f * 180.0f;
                    Yaw = ((int16_t)(RxBuffer[7] << 8 | RxBuffer[6])) / 32768.0f * 180.0f;
                }
                else if(RxBuffer[1] == 0x52)  // 角速度数据
                {
                    // 偏航(Z)轴角速度 (单位: °/s)，量程±2000°/s。
                    // 0x52 的字段序和各轴：2,3=wx / 4,5=wy / 6,7=wz —— 偏航角速度在 6,7。
                    // （2026-10-01 实测纠错：原来取的 4,5 是 wy。取证办法见 out/_gyro_axis.ps1：
                    //   原地匀速转，同时读 Yaw 和 0x52 包，wz 与 dYaw/dt 吻合、wy 差 4 倍。）
                    Gyro_Z = ((int16_t)(RxBuffer[7] << 8 | RxBuffer[6])) / 32768.0f * 2000.0f;
                }
            }
            RxState = 0;
            RxIndex = 0; //读取完成，回到最初状态，等待包头
        }
    }
}


