#include "renwu.h"
#include "motor.h"
#include "tracing.h"

#include "PID_tracing.h"
#include "servo.h"
#include "buzzer.h"
#include "rgb.h"
#include "gyro_control.h"

#include "host_link.h"
#include "bsp_board.h"

/* ================= 已废弃：任务编排已由上位机可编程任务取代（2026-10-03） =================
 * 本文件不再从 main_app.c 的主循环里运行（那一小段已摘掉）。取代它的是
 *   App/Svc/task_prog.c（程序格式与校验）+ App/Svc/task_run.c（非阻塞状态机解释器）
 *   + App/Svc/task_store.c（烧进页 59..62 的持久化）
 * —— 用户在浏览器里拖出状态机、烧进 Flash，车离线自主执行；不再需要这份编译进固件的
 * A/B/C 固定流程。**保留不删**：它是赛道 11 个障碍与旧板时序的唯一参考实现（哪个障碍
 * 用哪个动作、哪一段要多少 PWM 与延时都还在这里），写新程序时照抄这里的数值最省事。
 * 上面的刻度换算（RW(v)）仍然有效，别删。 */

/* ================= 旧板刻度 → 本板 PWM（2026-09-30 m02112 同步） =================
 * 本文件是从旧板工程 Haixin/Hardware/renwu.c 逐行搬过来的（两份文件只差 include 行），
 * 里面的 900 / 920 / 935 / -500 / -700 全是**旧板刻度**：
 *   旧板 STM32F103VET6 的 TIM8 Prescaler=71 / Period=1000-1（Haixin/Core/Src/tim.c），
 *   PWM 满量程只有 1000，Load() 把数值直接写进 CCR ⇒ 旧板 900 = 90% 占空比、1000 = 满油门；
 *   本板满量程 MOTOR_PWM_MAX = 7200（bsp_board.c: ARR=7199 @170MHz = 23.6kHz）。
 * 照抄过来的 900 在本板只有 12.5% 占空比，低于静摩擦起步死区（约 2500~2900）⇒ 车推不动。
 *
 * RW(v)：把旧刻度换算成本板 PWM。基准 = “旧板 900” 对应 HP_TASK_SPEED（默认 6480 = 90%），
 * 于是流程内部的比例（920/935/500/-700 …）整体保留，只是刻度放大 7.2 倍。
 * 想让比赛流程整体快/慢：改 HP_TASK_SPEED（菜单/上位机可改），不必逐个重标数字。 */
static int task_pwm(int old_scale)
{
    int base = (int)host_param_get(HP_TASK_SPEED, 6480.0f);
    if (base < 0)             base = 0;
    if (base > MOTOR_PWM_MAX) base = MOTOR_PWM_MAX;
    return (old_scale * base) / 900;
}
#define RW(v) task_pwm(v)

uint8_t renwu_flag=0;
uint8_t black_num=0;

// 添加静态变量来跟踪陀螺仪控制状态
static uint8_t gyro_control_active = 0;

// 添加静态变量来保存关闭前的基础速度
static int saved_base_speed_left = 0;
static int saved_base_speed_right = 0;

// 新增函数：控制陀螺仪PID的启用和禁用
// 2026-10-03：改为 static —— 全工程只有本文件的 A()/B()/C() 调它，头文件里从来没有声明
//（以前没原型，编译器连实参类型都管不了），收成内部符号就不会再污染全局命名空间。
static void Toggle_Gyro_Control(uint8_t enable, int base_speed_left, int base_speed_right) 
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
    default:
     /* 2026-10-03：补 default。renwu_flag 现在只有 host_link.c 的 CMD_STOP/START_A/B/C
      * 写 0/1/2/3，理论上进不来别的值；但真进来一个野值时原来的 switch 什么都不做 =
      * 主循环把任务当"在跑"（s_renwu_active 仍为 1）却没有任何状态机在推进，电机就停在
      * 上一拍的输出上。宁可退成停止：归零让主循环走 renwu_stop() 断电机。 */
     renwu_flag = 0;
     black_num = 0;
     break;
 }
}

/* 任务被外部停掉（上位机 CMD_STOP / renwu_flag 归零）时调用：
 * 关掉流程可能还开着的角度环并断电机。不能改成 Toggle_Gyro_Control(0, 0, 0)——
 * 那个分支的语义是"直行途中临时关角度环"，它会把 saved_base_speed_* 重新写回电机。 */
void renwu_stop(void)
{
    if (gyro_control_active) {
        GyroControl_Stop();
        Gyro_Timer_Stop();
        gyro_control_active = 0;
    }
    Load(0, 0);
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
        Toggle_Gyro_Control(1, RW(900), RW(935));
        Buzzer_blink();
        Load(RW(900), RW(935));
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
        Toggle_Gyro_Control(1, RW(930), RW(900));
        Buzzer_blink();
				Servo_Move(4);
				Load(RW(920), RW(900));
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
        
				Load(RW(920), RW(880));
				HAL_Delay(300);
        Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnLeftFixed();
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0);
				Servo_Move(2);
				Load(RW(-500), RW(-500));
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
        Toggle_Gyro_Control(1, RW(900), RW(960));
        Buzzer_blink();
				Load(RW(900), RW(950));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(450);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(70);
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0);
        black_num=7;
    }else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==7)
    {

        Buzzer_blink();
        
				Load(RW(900), RW(900));
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
				
				Toggle_Gyro_Control(1, RW(900), RW(900));
        
        
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
        Toggle_Gyro_Control(1, RW(900), RW(900));
        Buzzer_blink();
				Load(RW(910), RW(900));
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
				Load(RW(900), RW(900));
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
				
				Toggle_Gyro_Control(1, RW(900), RW(900));
				
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
			
				Toggle_Gyro_Control(1, RW(900), RW(900));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(700);
				
				Load(RW(-700), RW(700));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(700);
				
				Load(RW(-700), RW(700));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(300);
				Load(RW(500), RW(-900));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(400);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(60);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //zuo转90
				Load(RW(-900), RW(-900));
				HAL_Delay(600);
        black_num=18;
    }
		
		/*****************************十字路口********************************************************************************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==18)
    {
       
        Buzzer_blink();
        
				Load(RW(900), RW(900));
				HAL_Delay(400);
				
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRightFixed();
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //右转90
				Load(RW(300), RW(0));
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
        
				Load(RW(900), RW(900));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(400);
				Load(RW(500), RW(-900));
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
        
				Load(RW(900), RW(900));
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
        
				Load(RW(900), RW(900));
				HAL_Delay(500);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(80);
				HAL_Delay(700);
        Toggle_Gyro_Control(0, 0, 0); //左转90
			
				Load(RW(-900), RW(-900));
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
				Load(RW(600), RW(600));
				HAL_Delay(700);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRight(88);
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0); //左转90
				Load(RW(-500), RW(-500));
				HAL_Delay(1100);
        black_num=24;
    }
		/******************************超级大坡******************************/
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==24)
    {
        
        Buzzer_blink();
				Toggle_Gyro_Control(1, RW(900), RW(900));
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
				Toggle_Gyro_Control(1, RW(900), RW(900));
				HAL_Delay(1100);
				Servo_Move(13);
				HAL_Delay(1300);
				Servo_Move(2);
				HAL_Delay(1000);
				Toggle_Gyro_Control(0, 0, 0);
				
				Toggle_Gyro_Control(1, RW(-900), RW(-900));
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
				Toggle_Gyro_Control(1, RW(800), RW(800));
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
				Load(RW(900), RW(900));
				HAL_Delay(450);
				Toggle_Gyro_Control(1, 0, 0);
				GyroControl_TurnRightFixed();
				HAL_Delay(500);
        Toggle_Gyro_Control(0, 0, 0); //右转90
				Load(RW(-500), RW(-500));
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
        Toggle_Gyro_Control(1, RW(900), RW(900));
        Buzzer_blink();
        Load(RW(900), RW(900));
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
				Toggle_Gyro_Control(1, RW(500), RW(500));
				Servo_Move(11);
				HAL_Delay(3000);
				Toggle_Gyro_Control(0, 0, 0);
				black_num=27;
				
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==27)
    {
        
        Buzzer_blink();
				Servo_Move(1);
				Load(RW(-900), RW(-900));
				HAL_Delay(1000);
//				Load(700,700);
//				HAL_Delay(700);
        black_num=28;
    }
		else if (tracing_data[0]==0 && tracing_data[1]==0 && tracing_data[2]==0 && tracing_data[3]==0 && tracing_data[4]==0 && tracing_data[5]==0 && tracing_data[6]==0 && tracing_data[7]==0 && black_num==28)
    {
        Load(0,0);
        Buzzer_blink();
				Toggle_Gyro_Control(1, RW(900), RW(900));
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


