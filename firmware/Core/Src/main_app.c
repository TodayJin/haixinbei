/**
 * @file  main_app.c
 * @brief 全地形机器人主固件（旧固件上层 + 新板底层 + KK_UI 显示）
 *
 * 与自检固件的区别：自检固件（main_selftest.c）只做上电体检；这一份是比赛用的主程序。
 * 显示由 KK_UI 独占整屏刷新，主循环只负责把输入归一化和时间喂给它。
 */
#include "main.h"
#include "board.h"
#include "app_ui.h"
#include "oled_diag.h"    /* 只在 -DAPP_OLED_DIAG 的构建里被调用 */
#include "kk_oled.h"
#include "SEGGER_RTT.h"

/* 旧固件上层（不动）+ 新板 BSP */
#include "bsp_board.h"
#include "host_link.h"
#include "tracing.h"
#include "trace_cal.h"
#include "nvram.h"      /* 设置/标定的 Flash 存档（最后一页） */
#include "bus_health.h" /* I2C 总线看门狗（连续失败 -> 手动打时钟恢复总线） */
#include "jy61p.h"
#include "motor.h"
#include "servo.h"
#include "encoder.h"
#include "speed_control.h"
#include "motion.h"
#include "gyro_control.h"  /* 任务流程（renwu.c）要用角度环，见主循环里的任务闸门 */
#include "renwu.h"         /* 旧板的比赛任务流程 A/B/C（host_link 的 CMD_START_A/B/C 启动） */
#include "task_run.h"      /* 上位机可编程任务（状态机）解释器，见主循环末尾的每拍推进 */
#include "task_store.h"    /* 任务程序在 Flash 里的 3 个槽位（页 60/61/62）+ 开机自启 */
#include "motor_bench.h" /* 只在 -DMOTOR_BENCH 的构建里干活，见 motor_bench.c */

extern volatile uint32_t g_oled_tx_done;
extern volatile uint32_t g_oled_tx_err;

void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

/* ---- DWT 周期计数器：用来量每次 KK_UI_Update 真实花了多少周期 ---- */
static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

int main(void)
{
  uint32_t now;
  uint32_t last_frame = 0;
  uint32_t cyc_max = 0, cyc_sum = 0, cyc_n = 0, cyc_last;
  uint32_t report_at = 0;
  uint32_t gyro_frames_prev = 0, gyro_seen_ms = 0;

  HAL_Init();

  /* DWT 周期计数器要尽早开：Motion_Task() 里的 5ms 控制中断耗时统计
   * （g_isr_cyc_*，见 motion.c）从第一拍就得有数，不能等主循环走到后面。 */
  dwt_init();

  SEGGER_RTT_Init();
  SEGGER_RTT_WriteString(0, "\r\n=== haixin main app (KK_UI) ===\r\n");
  SEGGER_RTT_printf(0, "BUILD: %s %s\r\n", __DATE__, __TIME__);

  /* --- 时钟 --- */
  {
    clock_report_t clk;
    board_clock_init(&clk);
    SEGGER_RTT_printf(0, "CLK: HSE=%u PLL=%u SYSCLK=%lu Hz\r\n",
                      clk.hse_ok, clk.pll_used, (unsigned long)clk.sysclk_hz);
  }

  /* --- 板级外设：按键 + I2C1（OLED 挂在这条总线上）--- */
  board_gpio_keys_init();
  board_buzzer_init();
  if (board_i2c_init_hz(400000U) != HAL_OK) {
    SEGGER_RTT_WriteString(0, "ERR: I2C1 init failed\r\n");
  }
  SEGGER_RTT_WriteString(0, "I2C1 @400kHz\r\n");
  if (board_i2c_enable_dma() != HAL_OK) {
    SEGGER_RTT_WriteString(0, "ERR: I2C1 DMA setup failed\r\n");
  }

  /* --- 上位机链路（LPUART1 / 无线下载器）--- */
  host_link_init();
  host_link_send_text("\r\n[fw] host link ready (LPUART1 115200)\r\n");
  SEGGER_RTT_WriteString(0, "host link init OK\r\n");

  /* --- 旧固件需要的底层（新板 BSP）---
   * 注意：Motor_Init 让 TIM8 四路 PWM 上电即为 0 -> 电机不转（安全）。 */
  bus_health_init();      /* I2C 总线看门狗：清连续失败计数 / 恢复次数（诊断行要显示） */
  bsp_trace_init();
  trace_cal_init();       /* 灰度模块：尽力切到"输出全部"，主循环再轮询 IIC 模拟量 */
  Motor_Init();
  Servo_Init();

  /* --- 编码器 + 三层串级（循迹环 -> 角度环 -> 速度环）---
   * Motion 默认是 MOTION_IDLE：只测速，不碰 PWM，所以上电不会自己跑；
   * 模块测试页直接调 Load() 依然有效（速度环未使能时不写 PWM）。 */
  bsp_encoder_init();
  Encoder_Init();
#ifdef MOTOR_BENCH
  /* 台架自测必须在 bsp_periodic_start() 之前：TIM6 的 SpeedControl_Task()
   * 每 5ms 就把编码器 CNT 读走清零，跑在它后面会偷计数。 */
  MotorBench_Run();
#endif
  Motion_Init();
  bsp_periodic_start();   /* TIM6 -> Motion_Task（运动 5ms）；TIM7 -> Servo_Task（舵机缓动） */
  SEGGER_RTT_WriteString(0, "encoder + speed loop init OK\r\n");

  /* --- 陀螺仪：JY61P 走 USART3（PB10/PB11）115200，中断收 + 环形缓冲 ---
   * 旧上层 jy61p.c 的解析器按字节喂，帧是 0x55 开头的 11 字节。 */
  board_uart_jy61_init();
  board_jy61_start_rx();
  SEGGER_RTT_WriteString(0, "JY61P (USART3) init OK\r\n");

  /* --- 显示：KK_OLED 初始化（应用负责），随后 KK_UI 接管整屏刷新 --- */
  if (OLED_Init() != OLED_OK) {
    SEGGER_RTT_WriteString(0, "ERR: OLED_Init failed\r\n");
    while (1) { HAL_Delay(500); }
  }
  app_ui_setup_display();       /* 按安装方向设置屏幕翻转（见 app_ui.h） */
  SEGGER_RTT_WriteString(0, "OLED init OK\r\n");

#ifdef APP_OLED_DIAG
  oled_diag_run();              /* 四组合轮播，不返回（原型在 App/Inc/oled_diag.h） */
#endif
  app_ui_test_pattern();        /* 先画一屏字体/图元自检，肉眼可见 */
  HAL_Delay(1200);

  app_ui_init();
  SEGGER_RTT_WriteString(0, "KK_UI init OK\r\n");

  /* 设置/标定存档（Flash 最后一页 0x0801F800）。必须在 app_ui_init() 之后：
   * 恢复存档的最后一步是 app_ui_settings_loaded()，它把 PID 参数 push 回
   * host_link 参数表——而 app_ui_init() 里的 pull_pid_params() 正好是反向覆盖。 */
  nvram_init();
  SEGGER_RTT_printf(0, "nvram loaded=%u\r\n", (unsigned)nvram_loaded());

  /* 上位机烧进 Flash 的任务程序（页 59 目录 + 页 60..62 程序区，见 App/Svc/task_store.c）。
   * 放在 nvram_init() 之后：两者各读自己的 Flash 页，但任务一启动就接管电机，必须等
   * 参数表（PID 等标定量）已经从存档恢复完。 */
  TaskStore_Init();
  {
    uint8_t slot;
    for (slot = 0U; slot < TaskStore_SlotCount(); slot++) {
      char nm[TASK_STORE_NAME_MAX + 1u];
      uint8_t present = TaskStore_Present(slot);
      (void)TaskStore_Name(slot, nm, sizeof nm);
      SEGGER_RTT_printf(0, "task slot %u: %s len=%u crc=%04X flags=0x%02X%s\r\n",
                        (unsigned)slot, present ? nm : "(empty)",
                        (unsigned)TaskStore_Len(slot), (unsigned)TaskStore_Crc(slot),
                        (unsigned)TaskStore_Flags(slot),
                        ((TaskStore_Flags(slot) & TASK_STORE_FLAG_AUTO) != 0U) ? " AUTO" : "");
    }

    /* 开机自启：上位机给任务置了 flags.bit0 就上电自己跑（比赛不能遥控，车离线自主
     * 执行）。没有任何任务带这个标记时这里什么都不做，所以台架上不会因为这个改动
     * 自己动起来 —— 自启是上位机上**显式**勾的选项。 */
    slot = TaskStore_AutoSlot();
    if (slot != TASK_STORE_NO_SLOT) {
      int e = TaskStore_Load(slot);
      SEGGER_RTT_printf(0, "task auto: slot=%u load=%d\r\n", (unsigned)slot, e);
      if (e == 0) {
        e = TaskRun_Start((uint16_t)slot);
        SEGGER_RTT_printf(0, "task auto: start=%d\r\n", e);
      }
    }
  }

  /* 旧比赛任务流程的收尾标志（见主循环里那一小段）：1=旧 renwu 流程曾被启动过 */
  static uint8_t s_renwu_active = 0U;

  while (1) {
    now = HAL_GetTick();

    /* 输入采样必须 ≥ 每 10ms 一次，不能被 20ms 的绘制间隔限制 */
    /* 灰度模块：先让 trace_cal 读一帧 IIC 模拟量（20ms 一次，内部自己躲 OLED 传输），
     * 再算 8 路 0/1；没标定/没模拟量时 trace_read_channel() 自动回落数字口。 */
    trace_cal_poll();
    read_tracing_data();
    {
      int i;
      for (i = 0; i < 8; i++) {
        g_live.trace[i] = (uint8_t)tracing_data[i];
        g_live.gray[i]  = trace_cal_level((uint8_t)i);   /* 0..255：模拟量归一化，或数字口 0/255 */
      }
    }
    /* 陀螺仪：把 USART3 收到的字节喂给旧上层的 JY61P 解析器 */
    {
      uint8_t jb;
      while (board_jy61_get_byte(&jb)) { jy61p_ReceiveData(jb); }
    }
    g_live.yaw_x10   = (int32_t)(Yaw * 10.0f);
    g_live.pitch_x10 = (int32_t)(Pitch * 10.0f);
    g_live.roll_x10  = (int32_t)(Roll * 10.0f);
    /* 陀螺仪在线判据：看"校验通过的帧数"有没有在涨，而不是"开机以来收过多少字节"。
     * 旧写法（字节数 > 32）一旦线断/模块死掉会永远显示在线；帧计数（jy61p.c）
     * 只在整帧校验通过时才加一，配 300ms 窗就能反映真实活性。 */
    {
      uint32_t jf = jy61p_frame_count();
      if (jf != gyro_frames_prev) { gyro_frames_prev = jf; gyro_seen_ms = now; }
      g_live.gyro_ok = ((uint32_t)(now - gyro_seen_ms) < 300U) ? 1U : 0U;
    }

    cyc_last = DWT->CYCCNT;
    app_ui_tick(now);
    cyc_last = DWT->CYCCNT - cyc_last;
    if (cyc_last > cyc_max) cyc_max = cyc_last;
    cyc_sum += cyc_last;
    cyc_n++;

    /* 存 Flash 第二步：app_ui_tick() 已经把"正在保存"画上屏并锁住按键，
     * 这里才真擦写（擦写停 CPU ~30ms，所以要排在绘制之后）。 */
    nvram_service();

    /* 上位机链路：收帧处理 + 按设定频率回遥测（loop_us 用 UI 刷新耗时） */
    host_link_poll(cyc_last / 170u);
    host_link_tick_1hz();

    /* 自定义页的实时数据：主循环里刷新 + 定期失效重绘。
     * 200ms（5Hz）-> 40ms（25Hz，用户 m02607"陀螺仪界面有点掉帧"）-> 20ms：
     * 20ms 正是 KK_UI 自己的帧节拍（kk_ui_config.h KK_UI_FRAME_INTERVAL_MS），
     * 应用侧再设一道 40ms 闸门就是白丢一半帧——环里长按翻页的过场只有 140ms
     * （KK_UI_PAGE_MS_FAST），40ms 只够画 3~4 帧，看着就是"一顿一顿"。
     * 真正的上限还是 OLED 异步搬运（脏页差分只搬变化的页/列）。 */
    if (now - last_frame >= 20U) {
      last_frame = now;
      KK_UI_Invalidate();
    }

    /* 每秒报一次刷新耗时（us），用数据看"实时"到底改善了多少 */
    if (now - report_at >= 1000U) {
      report_at = now;
      SEGGER_RTT_printf(0, "FRAME: update avg=%lu us max=%lu us (n=%lu) | oled tx_done=%lu err=%lu busy=%u st=%d\r\n",
                        (unsigned long)(cyc_sum / (cyc_n ? cyc_n : 1U) / 170U),
                        (unsigned long)(cyc_max / 170U),
                        (unsigned long)cyc_n,
                        (unsigned long)g_oled_tx_done, (unsigned long)g_oled_tx_err,
                        (unsigned)OLED_IsBusy(), (int)OLED_GetLastStatus());
      /* 运动环：ENC = 编码器 5ms 计数（手转轮子就能看到它动）；v = 轮缘线速度 mm/s；
       * tgt/out = 速度环的目标与实际 PWM；mode 见 motion.h（0=IDLE 1=GYRO 2=TRACE 3=TRACE_GYRO） */
      SEGGER_RTT_printf(0, "MOTION: mode=%d enc l=%d r=%d | v mm/s l=%d r=%d | tgt %d,%d out %d,%d | yaw=%d fault=%u\r\n",
                        (int)Motion_GetMode(),
                        SpeedControl_Counts(0), SpeedControl_Counts(1),
                        SpeedControl_SpeedMmps(0), SpeedControl_SpeedMmps(1),
                        SpeedControl_Target(0), SpeedControl_Target(1),
                        SpeedControl_Output(0), SpeedControl_Output(1),
                        (int)(Yaw * 10.0f),
                        (unsigned)Motion_GetFault());
      /* 5ms 控制中断耗时（us，已折算 /170）：avg 是这一秒的平均，max 是峰值，
       * over 是开机以来超过 5ms 预算的拍数。正常情况 max 应该只有几十 us、
       * over 恒 0；over 涨了就是有长操作堵住了中断（擦 Flash / 关中断太久）。 */
      SEGGER_RTT_printf(0, "ISR: avg=%lu us max=%lu us n=%lu over=%lu (last=%lu us) | i2c recover=%lu\r\n",
                        (unsigned long)(g_isr_cyc_sum / (g_isr_n ? g_isr_n : 1U) / 170U),
                        (unsigned long)(g_isr_cyc_max / 170U),
                        (unsigned long)g_isr_n,
                        (unsigned long)g_isr_over,
                        (unsigned long)(g_isr_cyc_last / 170U),
                        (unsigned long)board_i2c_recover_count());
      g_isr_cyc_max = 0; g_isr_cyc_sum = 0; g_isr_n = 0;
      cyc_max = 0; cyc_sum = 0; cyc_n = 0;
    }

    /* 设置存档：200ms 看一眼快照有没有变，变了再等 1.5s 稳定才擦写最后一页
     * （擦+写约 30ms，期间取指停摆；内部自己避开 OLED 传输）。 */
    nvram_poll();

    /* ---- 旧比赛任务流程（renwu.c 的 A/B/C）已从主循环摘掉 -------------------------
     * 任务编排已由上位机可编程任务取代（App/Svc/task_prog.c + task_run.c，见下）。
     * renwu.c **保留不删**（当旧流程的参考），但不再从主循环里跑：它与解释器都会
     * 接管电机，同时跑必然互相踩。
     * 只留"收尾清 flag"这一段：上位机的旧 CMD_START_A/B/C 仍会把 renwu_flag 置起来，
     * 而 host_link 把 renwu_flag != 0 当"车在动"（任务存储区拒绝擦写的判据之一）
     * ⇒ 不清掉它，一次老式 START 会让任务存储区**永远 BUSY**。 */
    if (renwu_flag != 0U) {
      renwu_flag = 0U;
    }
    if (s_renwu_active != 0U) {
      s_renwu_active = 0U;
      renwu_stop();           /* 关角度环 + Load(0,0) 断电机（防御性保留） */
    }

    /* ---- 上位机可编程任务（App/Svc/task_run.c + task_prog.c）----------------------
     * 状态机解释器，全非阻塞、5ms 一拍；没有"载入 + 启动"过程序时是空操作（什么都不做）。
     * 程序由上位机拼好（拖动组合）、烧进 Flash（App/Svc/task_store.c），车离线自主执行
     * —— 比赛不能遥控，所以运行期解释器不受链路看门狗约束；只有显式的停止命令/END
     * 块/故障才停车。 */
    TaskRun_Tick(HAL_GetTick());

    HAL_Delay(2);
  }
}

/**
 * @brief 周期中断（TIM6/TIM7 都是 5ms）
 *   TIM6 -> 运动控制：三层串级（循迹环/角度环）+ 速度环
 *   TIM7 -> 舵机缓动（Servo_MoveTo 的非阻塞推进）
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6) {
    Motion_Task();
  } else if (htim->Instance == TIM7) {
    Servo_Task();
  }
}
