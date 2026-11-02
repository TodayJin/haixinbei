/**
 * @file  bsp_board.c
 * @brief 新板 BSP 实现（定时器、循迹输入）
 */
#include "bsp_board.h"
#include "board.h"

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim8;
TIM_HandleTypeDef htim6;
TIM_HandleTypeDef htim7;
TIM_HandleTypeDef htim2;    /* 别名，供旧上层引用 */
TIM_HandleTypeDef htim4;
TIM_HandleTypeDef htim3;       /* 编码器 A：TIM3 (PB4/PB5)  */
TIM_HandleTypeDef htim4_enc;   /* 编码器 B：TIM4 (PB6/PB7)，不是上面的 htim4(=TIM7) */

/* 命令周期（ms）：旧板 TIM2 跑循迹 PID、TIM4 跑陀螺仪任务，这里合并到 TIM6/TIM7 */
#define BSP_TRACE_PERIOD_MS   5U
#define BSP_GYRO_PERIOD_MS    5U

/* =====================================================================
 * 循迹 8 路：PA0–PA7，输入 + 上拉（模块是 3.3V 逻辑）
 * ===================================================================== */
void bsp_trace_init(void)
{
    GPIO_InitTypeDef io = {0};
    uint16_t pins = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                    GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    io.Pin  = pins;
    io.Mode = GPIO_MODE_INPUT;
    io.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &io);
}

GPIO_PinState bsp_trace_bit(uint8_t idx)
{
    static const uint16_t pin_map[8] = {
        GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3,
        GPIO_PIN_4, GPIO_PIN_5, GPIO_PIN_6, GPIO_PIN_7
    };
    if (idx > 7U) return GPIO_PIN_RESET;
    return HAL_GPIO_ReadPin(GPIOA, pin_map[idx]);
}

/* =====================================================================
 * TIM3 / TIM4：编码器模式（4 倍频）
 *   TIM3_CH1/CH2 = PB4/PB5（AF2）  电机 A
 *   TIM4_CH1/CH2 = PB6/PB7（AF2）  电机 B
 * 编码器是 5V 型、板上有 10k 上拉到 5V，所以这里不开内部上拉。
 * ===================================================================== */
static void tim_encoder_init(TIM_HandleTypeDef *h, TIM_TypeDef *inst)
{
    TIM_Encoder_InitTypeDef enc = {0};

    h->Instance               = inst;
    h->Init.Prescaler         = 0U;                 /* 不做分频：转速就是计数值 */
    h->Init.CounterMode       = TIM_COUNTERMODE_UP; /* 编码器模式下由硬件决定方向 */
    h->Init.Period            = 0xFFFFU;            /* 16 位满量程，周期内清零 */
    h->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    h->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(h);

    enc.EncoderMode  = TIM_ENCODERMODE_TI12;        /* A/B 两相双边沿 -> 4 倍频 */
    enc.IC1Polarity  = TIM_ICPOLARITY_RISING;
    enc.IC1Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC1Prescaler = TIM_ICPSC_DIV1;
    enc.IC1Filter    = 10U;                         /* 霍尔 + 100nF，取大一点的数字滤波 */
    enc.IC2Polarity  = TIM_ICPOLARITY_RISING;
    enc.IC2Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC2Prescaler = TIM_ICPSC_DIV1;
    enc.IC2Filter    = 10U;
    (void)HAL_TIM_Encoder_Init(h, &enc);
}

void bsp_encoder_init(void)
{
    GPIO_InitTypeDef io = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();

    io.Mode      = GPIO_MODE_AF_PP;
    io.Pull      = GPIO_NOPULL;
    io.Speed     = GPIO_SPEED_FREQ_LOW;

    io.Alternate = GPIO_AF2_TIM3;
    io.Pin       = GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOB, &io);

    io.Alternate = GPIO_AF2_TIM4;
    io.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOB, &io);

    tim_encoder_init(&htim3, TIM3);
    tim_encoder_init(&htim4_enc, TIM4);
}

/* =====================================================================
 * TIM1：舵机 50Hz，1us 刻度（PSC=169 -> 1MHz, ARR=19999 -> 20ms）
 * 通道 PA8–PA11 = CH1–CH4
 * ===================================================================== */
static void tim1_servo_init(void)
{
    GPIO_InitTypeDef io = {0};
    TIM_OC_InitTypeDef oc = {0};
    TIM_ClockConfigTypeDef clk = {0};
    TIM_MasterConfigTypeDef master = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();

    io.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    io.Mode      = GPIO_MODE_AF_PP;
    io.Pull      = GPIO_NOPULL;
    io.Speed     = GPIO_SPEED_FREQ_LOW;
    io.Alternate = GPIO_AF6_TIM1;
    HAL_GPIO_Init(GPIOA, &io);

    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 170U - 1U;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = 20000U - 1U;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(&htim1);

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    (void)HAL_TIM_ConfigClockSource(&htim1, &clk);
    (void)HAL_TIM_PWM_Init(&htim1);

    master.MasterOutputTrigger = TIM_TRGO_RESET;
    master.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    (void)HAL_TIMEx_MasterConfigSynchronization(&htim1, &master);

    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = 500U;
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2);
    /* ★2026-10-13（审计 #9）：CH3/CH4（PA10/PA11 = 3/4 号舵机座）只在这里配通道，
     *   **绝不启动**（见 bsp_servo_init()）：一旦启动，它们就会拿上面那个 500us 的初始
     *   脉宽一直输出，而 Servo_SetPulse() 只认 1/2 号（servo.c:38-42）没人能改它 ⇒
     *   插在这两个座上的舵机被永久钉在 0°、连"松掉"都做不到。 */
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_3);
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_4);
}

/* =====================================================================
 * TIM8：电机 PWM，23.6kHz（PSC=0, ARR=7199，保留旧板 0–7200 的刻度）
 * 通道 PC6–PC9 = CH1–CH4
 * ===================================================================== */
static void tim8_motor_init(void)
{
    GPIO_InitTypeDef io = {0};
    TIM_OC_InitTypeDef oc = {0};
    TIM_ClockConfigTypeDef clk = {0};
    TIM_MasterConfigTypeDef master = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE();

    io.Pin       = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    io.Mode      = GPIO_MODE_AF_PP;
    io.Pull      = GPIO_NOPULL;
    io.Speed     = GPIO_SPEED_FREQ_HIGH;
    io.Alternate = GPIO_AF4_TIM8;
    HAL_GPIO_Init(GPIOC, &io);

    htim8.Instance               = TIM8;
    htim8.Init.Prescaler         = 0U;
    htim8.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim8.Init.Period            = MOTOR_PWM_MAX - 1U;
    htim8.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim8.Init.RepetitionCounter = 0;
    htim8.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(&htim8);

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    (void)HAL_TIM_ConfigClockSource(&htim8, &clk);
    (void)HAL_TIM_PWM_Init(&htim8);

    master.MasterOutputTrigger = TIM_TRGO_RESET;
    master.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    (void)HAL_TIMEx_MasterConfigSynchronization(&htim8, &master);

    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = 0U;                 /* 上电默认 0 -> 电机不转（安全） */
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    (void)HAL_TIM_PWM_ConfigChannel(&htim8, &oc, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_ConfigChannel(&htim8, &oc, TIM_CHANNEL_2);
    (void)HAL_TIM_PWM_ConfigChannel(&htim8, &oc, TIM_CHANNEL_3);
    (void)HAL_TIM_PWM_ConfigChannel(&htim8, &oc, TIM_CHANNEL_4);
}

/* =====================================================================
 * TIM6 / TIM7：两个周期任务（替代旧板 TIM2 / TIM4）
 * ===================================================================== */
static void tim_periodic_init(TIM_HandleTypeDef *h, TIM_TypeDef *inst, uint32_t period_ms)
{
    /* 1kHz 计数：PSC=169 -> 1MHz，每 1000 个计数 1ms */
    h->Instance               = inst;
    h->Init.Prescaler         = 170U - 1U;
    h->Init.CounterMode       = TIM_COUNTERMODE_UP;
    h->Init.Period            = period_ms * 1000U - 1U;
    h->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    h->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(h);
}

void bsp_motor_init(void)
{
    tim8_motor_init();
    HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_4);
}

void bsp_servo_init(void)
{
    tim1_servo_init();
    /* ★2026-10-13（审计 #9：CH3/CH4 上电即被固定 500us 驱动 low）：
     * 这里原来把四路全 Start 掉，但上层只有 1/2 号舵机（Servo_SetPulse() 的
     * case 1/2，servo.c:38-42），CH3/CH4 的 CCR 从初始化那一刻起的 500us（oc.Pulse=500）
     * 就再没人动过 ⇒ 3/4 号座上的舵机被一条永不停止的 500us 脉冲永久钉在 0°，
     * 既改不了角度也松不掉（退不出力）；没插舵机时这两路也只是白跑。
     * 现在只启动真正在用的 CH1/CH2：CH3/CH4 的 CCxE 保持 0，引脚不再输出脉冲，
     * 插上去的舵机是"收不到信号"的松状态（要"钉住"必须由软件主动给脉宽，而不是上电默认）。
     * 以后真要用 3/4 号座：① 在 servo.c 的 Servo_SetPulse() 里补 case 3/4，
     * ② 在这里补回对应的 HAL_TIM_PWM_Start()（通道配置 tim1_servo_init() 已经做好了）。 */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
}

void bsp_periodic_start(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();
    __HAL_RCC_TIM7_CLK_ENABLE();

    tim_periodic_init(&htim6, TIM6, BSP_TRACE_PERIOD_MS);
    tim_periodic_init(&htim7, TIM7, BSP_GYRO_PERIOD_MS);

    /* 旧上层 extern 的是 htim2 / htim4，这里把句柄指过去 */
    htim2.Instance = TIM6;
    htim4.Instance = TIM7;

    /* ★2026-10-19 修 B10：原来优先级 6，而 I2C1_EV/ER、DMA1_Ch1/2/3、USART3、LPUART1
     * 都是 5（数字越小越高）⇒ **5ms 的运动控制拍会被显示/传感器中断抢占**
     * ⇒ 控制周期抖动 ⇒ 角度环/速度环相位不稳 ⇒ 表现为"抖"。
     * motion.c 的 g_isr_over 一直在量这个现象（ISR 超时），只是没归因到优先级。
     * ⚠2026-10-19 实测回退：提到 0 之后原地转直接失去控制（峰值 300deg、末值随机）
     *   ⇒ 推测 5ms 控制拍抢占 I2C/UART 事务中间态、撕坏传感器数据。
     *   **保持 6**（与历史一致）；要提升时序也应该只提到 4（低于外设，但高于显示）。 */
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
    HAL_NVIC_SetPriority(TIM7_DAC_IRQn, 6, 0);   /* G4 上 TIM7 与 DAC2/4 共用这一条中断向量；同 B10 提到 0 */
    HAL_NVIC_EnableIRQ(TIM7_DAC_IRQn);

    (void)HAL_TIM_Base_Start_IT(&htim6);
    (void)HAL_TIM_Base_Start_IT(&htim7);
}
