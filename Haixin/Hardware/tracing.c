#include "tracing.h"
#include "OLED.h"
#include "motor.h"
#include "usart.h"
int tracing_data[8];

void read_tracing_data()
{
	tracing_data[0]=!HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_4);
	tracing_data[1]=!HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_5);
	tracing_data[2]=!HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_4);
	tracing_data[3]=!HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_5);
	tracing_data[4]=!HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_0);
	tracing_data[5]=!HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_1);
	tracing_data[6]=!HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_7);
	tracing_data[7]=!HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_8);
}
void read_tracing_data_mv()
{
//	tracing_data[0]=recv_buffer_mv[0];
//	tracing_data[1]=recv_buffer_mv[1];
//	tracing_data[2]=recv_buffer_mv[2];
//	tracing_data[3]=recv_buffer_mv[3];
//	tracing_data[4]=recv_buffer_mv[4];
//	tracing_data[5]=recv_buffer_mv[5];
//	tracing_data[6]=recv_buffer_mv[6];
//	tracing_data[7]=recv_buffer_mv[7];
}
void show_tracing_data()
{
	OLED_ShowNum(64,0,tracing_data[0],1,OLED_8X16);
	OLED_ShowNum(72,0,tracing_data[1],1,OLED_8X16);
	OLED_ShowNum(80,0,tracing_data[2],1,OLED_8X16);
	OLED_ShowNum(88,0,tracing_data[3],1,OLED_8X16);
	OLED_ShowNum(96,0,tracing_data[4],1,OLED_8X16);
	OLED_ShowNum(104,0, tracing_data[5],1,OLED_8X16);
	OLED_ShowNum(112,0,tracing_data[6],1,OLED_8X16);
	OLED_ShowNum(120,0,tracing_data[7],1,OLED_8X16);
}
void motor_tracing(int speed_zhi,int speed_cha)
{
	    // Ñ­¼£Âß¼­
    if ((tracing_data[2] == 1) && (tracing_data[3] == 1) && (tracing_data[4] == 1) && (tracing_data[5] == 1)) {
        Set_Pwm(1, speed_zhi, speed_zhi);  // Ö±ÐÐ
    }
    else if ((tracing_data[3] != 0) && (tracing_data[4] != 0)) {
        Set_Pwm(1, speed_zhi, speed_zhi);  // Ö±ÐÐ
    }
    else if ((tracing_data[3] != 0) && (tracing_data[4] == 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[3] == 0) && (tracing_data[4] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[2] != 0) && (tracing_data[3] != 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[4] != 0) && (tracing_data[5] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[1] != 0) && (tracing_data[2] != 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, speed_cha, speed_zhi);
    }
    else if ((tracing_data[5] != 0) && (tracing_data[6] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else if ((tracing_data[0] != 0) && (tracing_data[1] != 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[6] != 0) && (tracing_data[7] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, 0);
    }
    else if ((tracing_data[1] != 0) && (tracing_data[2] == 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[5] == 0) && (tracing_data[6] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, 0);
    }
    else if ((tracing_data[0] != 0) && (tracing_data[1] == 0)) {  // ÓÒÆ«£¬×ó×ª
        Set_Pwm(1, 0, speed_zhi);
    }
    else if ((tracing_data[6] == 0) && (tracing_data[7] != 0)) {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, 0);
    }
	else if (tracing_data[2] == 1)  {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_cha, speed_zhi);
    }
	else if (tracing_data[5] == 1)  {  // ×óÆ«£¬ÓÒ×ª
        Set_Pwm(1, speed_zhi, speed_cha);
    }
    else {  // ÎÞ´«¸ÐÆ÷ÐÅºÅ£¬Í£Ö¹»òÖ±ÐÐ
        Set_Pwm(2, 290, 290);
    }

}
//void motor_tracing_mv(int speed_zhi,int speed_cha)
//{
//	    // Ñ­¼£Âß¼­
//    if ((tracing_data[2] == 9) && (tracing_data[3] == 9) && (tracing_data[4] == 9) && (tracing_data[5] == 9)) {
//        Set_Pwm(1, speed_zhi, speed_zhi);  // Ö±ÐÐ
//    }
//    else if ((tracing_data[3] != 8) && (tracing_data[4] != 8)) {
//        Set_Pwm(1, speed_zhi, speed_zhi);  // Ö±ÐÐ
//    }
//    else if ((tracing_data[3] != 8) && (tracing_data[4] == 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[3] == 8) && (tracing_data[4] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else if ((tracing_data[2] != 9) && (tracing_data[3] != 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[4] != 8) && (tracing_data[5] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else if ((tracing_data[1] != 8) && (tracing_data[2] != 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[5] != 8) && (tracing_data[6] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else if ((tracing_data[0] != 8) && (tracing_data[1] != 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[6] != 8) && (tracing_data[7] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else if ((tracing_data[1] != 8) && (tracing_data[2] == 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[5] == 8) && (tracing_data[6] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else if ((tracing_data[0] != 8) && (tracing_data[1] == 8)) {  // ÓÒÆ«£¬×ó×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//    else if ((tracing_data[6] == 8) && (tracing_data[7] != 8)) {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//		else if (tracing_data[2] == 9)  {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_cha, speed_zhi);
//    }
//		else if (tracing_data[5] == 9)  {  // ×óÆ«£¬ÓÒ×ª
//        Set_Pwm(1, speed_zhi, speed_cha);
//    }
//    else {  // ÎÞ´«¸ÐÆ÷ÐÅºÅ£¬Í£Ö¹»òÖ±ÐÐ
//        Set_Pwm(2, 290, 290);
//    }
//}
