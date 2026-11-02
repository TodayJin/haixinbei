/**
 * @file  stm32g4xx_it.c
 * @brief 中断与异常处理
 */
#include "main.h"
#include "board.h"
#include "bsp_board.h"
#include "SEGGER_RTT.h"

/* ---------------- Cortex-M4 异常 ---------------- */
void NMI_Handler(void)        { while (1) { } }
void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }

void MemManage_Handler(void)  { while (1) { } }
void BusFault_Handler(void)   { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }

void HardFault_Handler(void)
{
  uint32_t lr, msp, psp, pc;
  volatile uint32_t *frame;

  __asm volatile ("mov %0, lr"  : "=r" (lr));
  __asm volatile ("mrs %0, msp" : "=r" (msp));
  __asm volatile ("mrs %0, psp" : "=r" (psp));

  frame = (volatile uint32_t *)((lr & 0x4u) ? psp : msp);
  pc    = frame[6];

  SEGGER_RTT_printf(0,
    "\r\n!! HARDFAULT pc=0x%08lX lr=0x%08lX\r\n"
    "   CFSR =0x%08lX HFSR=0x%08lX\r\n"
    "   BFAR =0x%08lX MMFAR=0x%08lX\r\n",
    (unsigned long)pc, (unsigned long)lr,
    (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
    (unsigned long)SCB->BFAR, (unsigned long)SCB->MMFAR);

  while (1) { }
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

/* USART3 = JY61P */
void USART3_IRQHandler(void)
{
  HAL_UART_IRQHandler(board_jy61_handle());
}

/* LPUART1 = 无线下载器（上位机链路） */
void LPUART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(board_dl_handle());
}

/* I2C1 = OLED（KK_OLED 异步刷新用中断发页命令、DMA 发页数据） */
void I2C1_EV_IRQHandler(void)
{
  HAL_I2C_EV_IRQHandler(board_i2c());
}

void I2C1_ER_IRQHandler(void)
{
  HAL_I2C_ER_IRQHandler(board_i2c());
}

void DMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(board_i2c_dma_tx());
}

/* LPUART1 收 DMA（DMA1_Ch2，循环搬运）：绕圈一次报一次，只用来数溢出 */
void DMA1_Channel2_IRQHandler(void)
{
  board_dl_dma_rx_irq();
}

/* LPUART1 发 DMA（DMA1_Ch3，槽位队列）：HAL 在这里收尾，最终回调 TxCplt 再 kick 下一帧 */
void DMA1_Channel3_IRQHandler(void)
{
  HAL_DMA_IRQHandler(board_dl_dma_tx());
}

/* TIM6 = 循迹 PID 周期（替代旧板 TIM2）；TIM7 = 陀螺仪任务（替代旧板 TIM4） */
void TIM6_DAC_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

void TIM7_DAC_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim7);
}
