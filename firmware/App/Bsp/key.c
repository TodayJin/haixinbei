#include "key.h"

unsigned char key()
{
    static unsigned char last_key = 0;
    static unsigned char key_state = 0;
    static unsigned char debounce_count = 0;
    unsigned char current_key = 0;
    
    // 检查哪个按键被按下
    if (HAL_GPIO_ReadPin(KEY_CENTER_PORT, KEY_CENTER_PIN) == GPIO_PIN_RESET)
    {
        current_key = 1;
    }
    else if (HAL_GPIO_ReadPin(KEY_UP_PORT, KEY_UP_PIN) == GPIO_PIN_RESET)
    {
        current_key = 2;
    }
    else if (HAL_GPIO_ReadPin(KEY_DOWN_PORT, KEY_DOWN_PIN) == GPIO_PIN_RESET)
    {
        current_key = 3;
    }
    
    // 消抖处理
    if (current_key != 0 && current_key == last_key)
    {
        debounce_count++;
        // 等待连续4次检测到相同按键（假设定时间隔约5ms，则总共20ms）
        if (debounce_count >= 4 && key_state == 0)
        {
            key_state = 1;
            return current_key;
        }
    }
    else
    {
        debounce_count = 0;
        if (current_key == 0)
        {
            key_state = 0;
        }
    }
    
    last_key = current_key;
    return 0;
}