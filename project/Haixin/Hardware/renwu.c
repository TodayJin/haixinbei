#include "usart.h"
#include "renwu.h"
#include "motor.h"
#include "tracing.h"
#include "OLED.h"
#include "PID_tracing.h"
#include "servo.h"
#include "buzzer.h"
#include "gyro_control.h"

uint8_t renwu_flag=0;
uint8_t black_num=0;

// 添加静态变量来跟踪陀螺仪控制状态
static uint8_t gyro_control_active = 0;

// 添加静态变量来保存关闭前的基础速度
static int saved_base_speed_left = 0;
static int saved_base_speed_right = 0;

// 新增函数：控制陀螺仪PID的启用和禁用
void Toggle_Gyro_Control(uint8_t enable, int base_speed_left, int base_speed_right) 
{
    if (enable && !gyro_control_active) {
        // 保存当前的基础速度，供关闭时使用
        saved_base_speed_left = base_speed_left;
        saved_base_speed_right = base_speed_right;
        
        // 启用陀螺仪控制
        GyroControl_Init();
        GyroControl_SetBaseSpeed(base_speed_left, base_speed_right);
        GyroControl_Start();
        Gyro_Timer_Start();
        gyro_control_active = 1;
    } else if (!enable && gyro_control_active) {
        // 禁用陀螺仪控制
        GyroControl_Stop();
        Gyro_Timer_Stop();
        gyro_control_active = 0;
        
        // 恢复到之前的速度，保持电机运行
        Load(saved_base_speed_left, saved_base_speed_right);
    }
}

void renwu()
{
 switch(renwu_flag)
 {
	case 1://A
     A();
     break;
    case 2://B
     B();
     break;
    case 3://C
     C();
     break;
    case 0://D
			black_num=0;
     break;
 }
}

void A()
{
    // 默认情况下使用普通的PID循迹
    motor_tracing_pid(4.5);
		/******************************2级台阶******************************/
    if(tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==0)
    {
        
        Buzzer_blink();
				Servo_Move(2);
        HAL_Delay(300);
        black_num=1;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==1)
    {
        // 开启陀螺仪PID控制
        Toggle_Gyro_Control(1, 900, 935);
        Buzzer_blink();
        Load(900,935);
        HAL_Delay(450);
        Servo_Move(3);
        HAL_Delay(700);
        Servo_Move(2);
        HAL_Delay(600);
        Servo_Move(4);
        HAL_Delay(600);
        Servo_Move(5);
        HAL_Delay(1500);
        // 关闭陀螺仪PID控制
        Toggle_Gyro_Control(0, 0, 0);
        black_num=2;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==2)
    {
        
        Buzzer_blink();
		HAL_Delay(300);
        black_num=3;
    }
		/******************************木刺******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==3)
    {
        // 开启陀螺仪PID控制
        Toggle_Gyro_Control(1, 930, 900);
        Buzzer_blink();
				Servo_Move(4);
				Load(920,900);
				HAL_Delay(1000);
				Servo_Move(2);
				HAL_Delay(1950);
				Servo_Move(1);
				Buzzer_blink();
        // 关闭陀螺仪PID控制
        Toggle_Gyro_Control(0, 0, 0);
        black_num=4;
    }
/******************************栅栏******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==4)
    {
        Buzzer_blink();
        
				Load(920,880);
				HAL_Delay(300);
        Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeftFixed();
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0);
				Servo_Move(2);
				Load(-500,-500);
				HAL_Delay(800);
			
        motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
//				motor_tracing_pid(4.5);
//				HAL_Delay(20);
				

				
				
        // 开启陀螺仪PID控制
        Toggle_Gyro_Control(1, 900, 960);
        Buzzer_blink();
				Load(900,950);
				Servo_Move(3);
				HAL_Delay(500);
				Servo_Move(2);
				HAL_Delay(2050);
		    Buzzer_blink();
        // 关闭陀螺仪PID控制
        Toggle_Gyro_Control(0, 0, 0);
				
				black_num=6;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0&& /*tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && */black_num==6)
    {

        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(450);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(70);
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0);
        black_num=7;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==7)
    {

        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(500);
				Servo_Move(1);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeftFixed();
				HAL_Delay(800);
        Toggle_Gyro_Control(0, 0, 0);
			
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
//				motor_tracing_pid(4.5);
				
				Toggle_Gyro_Control(1, 900, 900);
        
        
				HAL_Delay(3800);
        // 关闭陀螺仪PID控制
        Toggle_Gyro_Control(0, 0, 0);
				Buzzer_blink();
        black_num=9;
    }
		/******************************减速带******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==8)
    {
        // 开启陀螺仪PID控制
        
        black_num=9;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==9)
    {
        Buzzer_blink();
				HAL_Delay(300);
        black_num=10;
    }
		/******************************草******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==10)
    {
        // 开启陀螺仪PID控制
        Toggle_Gyro_Control(1, 900, 900);
        Buzzer_blink();
				Load(910,900);
				HAL_Delay(1000);
				HAL_Delay(1800);
        // 关闭陀螺仪PID控制
        Toggle_Gyro_Control(0, 0, 0);
        black_num=11;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==11)
    {
        
        Buzzer_blink();
				HAL_Delay(300);
				
        black_num=13;
    }
		/******************************方形通道******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==12)
    {
        
        Buzzer_blink();
				HAL_Delay(300);
        black_num=18;
    }
		else if (/*tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && */tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==13)
    {
        
        Buzzer_blink();
				Load(900,900);
				HAL_Delay(600);
			
//				Load(500,-900);
//				HAL_Delay(500);
//			
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeft(80);
				HAL_Delay(800);
        Toggle_Gyro_Control(0, 0, 0); //左转90
			
				PID_Tracing_Timer_Start();
				HAL_Delay(1000);
				PID_Tracing_Timer_Stop();
			
				Load(0,0);
				HAL_Delay(200);
				
				Toggle_Gyro_Control(1, 900, 900);
				
				HAL_Delay(400);
				Servo_Move(12);
				HAL_Delay(1600);
				Buzzer_blink();
        Toggle_Gyro_Control(0, 0, 0);//直行
//			
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeftFixed();
				HAL_Delay(1700);
        Toggle_Gyro_Control(0, 0, 0); //左转90

				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
			
				Toggle_Gyro_Control(1, 900, 900);
				HAL_Delay(2000);
				Buzzer_blink();
        Toggle_Gyro_Control(0, 0, 0);//直行
				
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeftFixed();
				HAL_Delay(1700);
        Toggle_Gyro_Control(0, 0, 0); //左转90
				
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
				motor_tracing_pid(4.5);
				HAL_Delay(20);
        black_num=17;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 /*&& tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0*/ && black_num==14)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(700);
				
				Load(-700,700);
				HAL_Delay(600);
			
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnLeftFixed();
//				HAL_Delay(700);
//        Toggle_Gyro_Control(0, 0, 0); //zuo转90
        black_num=15;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 /*&& tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0*/ && black_num==15)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(700);
				
				Load(-700,700);
				HAL_Delay(600);
			
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnLeftFixed();
//				HAL_Delay(700);
//        Toggle_Gyro_Control(0, 0, 0); //zuo转90
        black_num=16;
    }
		else if (/*tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 &&*/ tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==16)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(300);
				Load(500,-900);
				HAL_Delay(400);
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnRightFixed();
//				HAL_Delay(700);
//        Toggle_Gyro_Control(0, 0, 0); //右转90
        black_num=17;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 /*&& tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0*/ && black_num==17)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(400);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(60);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //zuo转90
				Load(-900,-900);
				HAL_Delay(600);
        black_num=18;
    }
		
		/*****************************十字路口********************************************************************************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==18)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(400);
				
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRightFixed();
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //右转90
				Load(300,0);
				HAL_Delay(300);
				Load(0,0);
				Servo_Move(12);
				HAL_Delay(300);
        black_num=19;
    }
		/******************************UUUUU******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 /* && tracing_data[4]==0  && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 */&& black_num==19)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(400);
			
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(60);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //左转90
			
				
				PID_Tracing_Timer_Start();
				HAL_Delay(500);
				PID_Tracing_Timer_Stop();
			
			
        black_num=21;
    }
		else if (/*tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && */tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==20)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(400);
				Load(500,-900);
				HAL_Delay(400);
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnRightFixed();
//				HAL_Delay(700);
//        Toggle_Gyro_Control(0, 0, 0); //右转90
//			
//				Toggle_Gyro_Control(1, 900, 900);
//				HAL_Delay(1000);
//        Toggle_Gyro_Control(0, 0, 0); //直行进洞
        black_num=21;
    }
		else if (/*tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 &&*/ tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==21)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(500);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeft(80);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //右转90
				
//				Load(-900,-900);
//				HAL_Delay(100);
//				
//				PID_Tracing_Timer_Start();
//				HAL_Delay(1000);
//				PID_Tracing_Timer_Stop();
//				
//				Toggle_Gyro_Control(1, 900, 900);
//				HAL_Delay(1000);
//				Buzzer_blink();
//        Toggle_Gyro_Control(0, 0, 0);//直行
        black_num=23;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 /*&& tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 */&& black_num==22)
    {
       
        Buzzer_blink();
        
				Load(900,900);
				HAL_Delay(500);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(80);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //左转90
			
				Load(-900,-900);
				HAL_Delay(100);
				
				PID_Tracing_Timer_Start();
				HAL_Delay(1000);
				PID_Tracing_Timer_Stop();
        black_num=23;
    }
		
		/******************************十字路口*****************************************************************************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==23)
    {
        Servo_Move(2);
        Buzzer_blink();
				Load(600,600);
				HAL_Delay(700);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(88);
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0); //左转90
				Load(-500,-500);
				HAL_Delay(1100);
        black_num=24;
    }
		/******************************超级大坡******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==24)
    {
        
        Buzzer_blink();
				Toggle_Gyro_Control(1, 900, 900);
				HAL_Delay(600);
				Servo_Move(6);
				HAL_Delay(1400);//shijian
				Load(0,0);
				HAL_Delay(500);
				Buzzer_blink();
				Toggle_Gyro_Control(0, 0, 0); 
			
			
				Servo_Move(14);
			
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeft(88);
				HAL_Delay(1000);
				Toggle_Gyro_Control(0, 0, 0);
				HAL_Delay(500);
//				Load(0,0);
//				HAL_Delay(500000);
				Toggle_Gyro_Control(1, 900, 900);
				HAL_Delay(1100);
				Servo_Move(13);
				HAL_Delay(1300);
				Servo_Move(2);
				HAL_Delay(1000);
				Toggle_Gyro_Control(0, 0, 0);
				
				Toggle_Gyro_Control(1, -900, -900);
				HAL_Delay(800);
				Servo_Move(11);
				HAL_Delay(1650);
				Servo_Move(2);
				Toggle_Gyro_Control(0, 0, 0);
				
				
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(88);//zuozhuan
				HAL_Delay(1000);
				Toggle_Gyro_Control(0, 0, 0);
			
				HAL_Delay(500);
				Toggle_Gyro_Control(1, 800, 800);
				Servo_Move(11);
				HAL_Delay(1750);
				Buzzer_blink();
				Toggle_Gyro_Control(0, 0, 0);
        
        black_num=25;
    }
		
		
		
		/******************************十字路口******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==25)
    {
        
        Buzzer_blink();
				Servo_Move(2);
				Load(900,900);
				HAL_Delay(450);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRightFixed();
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0); //右转90
				Load(-500,-500);
				HAL_Delay(900);
				PID_Tracing_Timer_Start();
				HAL_Delay(1000);
				PID_Tracing_Timer_Stop();
        black_num=26;
    }
		/******************************3级台阶******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==26)
    {
        
        // 开启陀螺仪PID控制
        Toggle_Gyro_Control(1, 900, 900);
        Buzzer_blink();
        Load(900,900);
        HAL_Delay(500);
			
        Servo_Move(7);
        HAL_Delay(1300);
        //Servo_Move(2);
        HAL_Delay(500);
			
				Servo_Move(3);
				//HAL_Delay(1300);
				Servo_Move(2);
				//HAL_Delay(200);
				Servo_Move(8);
				HAL_Delay(400);
			
        Servo_Move(9);
				HAL_Delay(900);
				Servo_Move(2);
				Toggle_Gyro_Control(0, 0, 0);
				Load(0,0);
				
				
				
				
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnLeft(88);
//				HAL_Delay(700);
//				Toggle_Gyro_Control(0, 0, 0);
//				HAL_Delay(500);
//				Toggle_Gyro_Control(1, 900, 900);
//				HAL_Delay(1000);
//				Servo_Move(3);
//				HAL_Delay(1000);
//				Servo_Move(2);
//				HAL_Delay(1000);
//				Toggle_Gyro_Control(0, 0, 0);
//				
//				Toggle_Gyro_Control(1, -900, -900);
//				HAL_Delay(1000);
//				Servo_Move(11);
//				HAL_Delay(1180);
//				Servo_Move(2);
//				HAL_Delay(400);
//				Toggle_Gyro_Control(0, 0, 0);
//				Toggle_Gyro_Control(1, 0, 0);
//				GyroControl_TurnRight(88);//zuozhuan
//				HAL_Delay(700);
//				Toggle_Gyro_Control(0, 0, 0);



				HAL_Delay(500);
				Toggle_Gyro_Control(1, 500, 500);
				Servo_Move(11);
				HAL_Delay(3000);
				Toggle_Gyro_Control(0, 0, 0);
				black_num=27;
				
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==27)
    {
        
        Buzzer_blink();
				Servo_Move(1);
				Load(-900,-900);
				HAL_Delay(1000);
//				Load(700,700);
//				HAL_Delay(700);
        black_num=28;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==28)
    {
        Load(0,0);
        Buzzer_blink();
				Toggle_Gyro_Control(1, 900, 900);
				HAL_Delay(10000);
        Toggle_Gyro_Control(0, 0, 0); 
				Load(0,0);
				renwu_flag=3;
    }
}
		
void B()
{
    PID_Tracing_Timer_Start();
}

void C()
{
	RGB_blink();
}


