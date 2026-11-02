#include <stdint.h>
#include "jy61p.h"

static uint8_t RxBuffer[11]; /*接收数据数组*/
static volatile uint8_t RxState = 0; /*接收状态标志位*/
static uint8_t RxIndex = 0; /*接受数组索引*/
float Roll, Pitch, Yaw; /*角度信息*/

float Gyro_Z = 0.0f;  // Z轴角速度变量

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
                    // 解析Z轴角速度 (单位: °/s)，量程±2000°/s
                    Gyro_Z = ((int16_t)(RxBuffer[5] << 8 | RxBuffer[4])) / 32768.0f * 2000.0f;
                }
            }
            RxState = 0;
            RxIndex = 0; //读取完成，回到最初状态，等待包头
        }
    }
}


