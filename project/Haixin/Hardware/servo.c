#include "servo.h"
/**
 * @brief ���ƶ���ת����ָ���Ƕ�
 * @param htim: ��ʱ������ָ��
 * @param channel: PWMͨ��
 * @param angle: Ŀ���Ƕ� (0-180��)
 */
void Servo_Control(int x, uint8_t angle)
{
    // �������Ʋ��� (����ʹ��50Hz PWMƵ��)
    // 0.5ms ��Ӧ 0��, 1.5ms ��Ӧ 90��, 2.5ms ��Ӧ 180��
    // ���趨ʱ��ʱ��Ϊ72MHz, Ԥ��Ƶϵ��Ϊ72-1, ��������Ϊ20000-1
    
    if(angle > 180) angle = 180;  // �Ƕ�������0-180�ȷ�Χ��
    
    // ����Ŀ���Ƚ�ֵ (��������)
    // 0.5ms -> 500us, 2.5ms -> 2500us
    // ��Ӧ����ֵ: 500 ~ 2500
    uint16_t pulse = 500 + (2000 * angle / 180);
    
    // ����PWMռ�ձ�
    switch (x)
    {
    case 1:
        __HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_4, pulse);
        break;

    case 2:
        __HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_3, pulse);
        break;
    }
}

/**
 * @brief ��ʼ������PWM����
 * @param htim: ��ʱ������ָ��
 * @param channel: PWMͨ��
 */
void Servo_Init()
{
    // ����PWM����
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4);
	
	Servo_Control(1,0);
	Servo_Control(2,0);
}


void Servo_Move(int x)
{
    switch (x)
    {
    case 1://收回
        Servo_Control(2,0);
        Servo_Control(1,0);
        break;
		case 2://打下准备
        Servo_Control(1,60);
				Servo_Control(2,120);
        break;
		case 3://打下1
        Servo_Control(1,105);
				Servo_Control(2,120);
        break;
		case 4://打下2
        Servo_Control(1,135);
				Servo_Control(2,150);
        break;
		case 5://2级台阶2收回
        Servo_Control(1,135);
				Servo_Control(2,170);
				HAL_Delay(400);
				Servo_Control(1,90);
				HAL_Delay(100);
				Servo_Control(1,80);
        break;
		case 6://大坡1
        Servo_Control(1,100);
				Servo_Control(2,120);      
        break;
		case 7://3级12
				Servo_Control(1,95);
				Servo_Control(2,120);
        break;
		case 8://3级2收回
				Servo_Control(1,40);
				Servo_Control(2,10);
        break;
		case 9://3级3打
				Servo_Control( 1,70);
				Servo_Control(2,10);
				HAL_Delay(200);
				Servo_Control(1,100);
				Servo_Control(2,50);
				HAL_Delay(100);
				Servo_Control(1,100);
				Servo_Control(2,80);
        break;
		case 10://高台
        Servo_Control(1,115);
				Servo_Control(2,120);
		case 11://gaotaixia
        Servo_Control(1,85);
				Servo_Control(2,120);
        break;
		case 12://收回2
        Servo_Control(2,0);
        Servo_Control(1,40);
        break;
		case 13://打下1
        Servo_Control(1,90
		
		
		
		
		);
				Servo_Control(2,60);
				break;
		case 14://打下准备
        Servo_Control(1,30);
				Servo_Control(2,60);
        break;
    }
}