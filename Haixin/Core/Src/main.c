/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "OLED.h"
#include "motor.h"
#include "key.h"
#include "PID_tracing.h"
#include "renwu.h"
#include "buzzer.h"
#include "servo.h"
#include "jy61p.h"
#include "gyro_control.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint32_t sys_tick=0;
int Encoder_Left,Encoder_Right;
uint8_t display_buf[20];
uint8_t key_num=0,menu=0;

uint8_t jy61p_rx_buffer[11];  

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
//void Read(void)
//{
//  if(uwTick-sys_tick>10)//10ms
//  {
//    sys_tick=uwTick;
//    Encoder_Right=-Read_Speed(&htim3);
//    Encoder_Left=Read_Speed(&htim2);
//  }
//  else if(uwTick-sys_tick>20)//20ms
//  {
//    
//  }
//}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART3_UART_Init();
  MX_TIM2_Init();
  MX_TIM8_Init();
  MX_I2C1_Init();
  MX_TIM1_Init();
  MX_TIM4_Init();
  /* USER CODE BEGIN 2 */
  OLED_Init();
  Motor_Init();
  Servo_Init();
  

  uint8_t cmd1[] = {0xFF, 0xAA, 0x02, 0x02, 0x00}; 
  HAL_UART_Transmit(&huart3, cmd1, 5, 100);
  HAL_Delay(100);
  
  uint8_t cmd2[] = {0xFF, 0xAA, 0x03, 0x06, 0x00}; 
  HAL_UART_Transmit(&huart3, cmd2, 5, 100);
  HAL_Delay(100);


  HAL_UART_Receive_DMA(&huart3, jy61p_rx_buffer, 11);


  Gyro_Timer_Start();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  Buzzer_blink();
  while (1)
  {
//    Read();
    key_num=key();


    if(key_num==1)
    {
      if(renwu_flag==0)
      {
        if (menu==0){menu=1;}
      else if (menu==1){menu=0;}
      }
    }
    if (key_num==2 && menu==1)
    {
      if (renwu_flag==0){renwu_flag=1;}
      else if (renwu_flag==1){renwu_flag=0;}
    }
    if (key_num==3 && menu==1)
    {
      if (renwu_flag==0){renwu_flag=2;}
      else if (renwu_flag==2){renwu_flag=0;}
    }

		
    if (menu==0)
    {
      OLED_Clear();
      OLED_ShowString(0,0,"循迹测试:",OLED_8X16);
      read_tracing_data();
      show_tracing_data();
//      sprintf((char *)display_buf,"转速_L:%3d",Encoder_Left);
//      OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
//      sprintf((char *)display_buf,"转速_R:%3d",Encoder_Right);
//      OLED_ShowString(0,32,(char *)display_buf,OLED_8X16);
      sprintf((char *)display_buf,"z:%.1f",Yaw);
      OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
      OLED_Update();
    }
    else if (menu==1)
    {
      OLED_Clear();
      if (renwu_flag==0)
      {
        OLED_ShowString(0,0,"任务选择:",OLED_8X16);
        OLED_ShowString(0,16,"1. A",OLED_8X16);
        OLED_ShowString(0,32,"2. B",OLED_8X16);
        Load(0,0);
      }
      if (renwu_flag==1)
      {
        OLED_ShowString(0,0,"任务A:",OLED_8X16);
        read_tracing_data();
        show_tracing_data();
//        sprintf((char *)display_buf,"转速_L:%3d",Encoder_Left);
//        OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
//        sprintf((char *)display_buf,"转速_R:%3d",Encoder_Right);
//        OLED_ShowString(0,32,(char *)display_buf,OLED_8X16);
        sprintf((char *)display_buf,"z:%.1f",Yaw);
        OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
      }
      if (renwu_flag==2)
      {
        OLED_ShowString(0,0,"任务B:",OLED_8X16);
        read_tracing_data();
        show_tracing_data();
//        sprintf((char *)display_buf,"转速_L:%3d",Encoder_Left);
//        OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
//        sprintf((char *)display_buf,"转速_R:%3d",Encoder_Right);
//        OLED_ShowString(0,32,(char *)display_buf,OLED_8X16);
        sprintf((char *)display_buf,"z:%.1f",Yaw);
        OLED_ShowString(0,16,(char *)display_buf,OLED_8X16);
      }
      OLED_Update();
    }

    
    renwu();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart == &huart3)
    {
        
        for(int i = 0; i < 11; i++) {
            jy61p_ReceiveData(jy61p_rx_buffer[i]);
        }
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4)
    {
        GyroControl_Task();
    }
    else if (htim->Instance == TIM2)
    {
        motor_tracing_pid_0_task();
    }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
