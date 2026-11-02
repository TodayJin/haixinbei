# portable/firmware —— 固件移植包内容清单

> **这是「换主控/换板子也要带走」的那一层，不是一份能直接 `make` 的完整工程。**
> 完整可编译工程在 `project/firmware/`；本目录由 `project/tools/pack_portable.py` 生成。
> 落地步骤与验证判据见 [`../移植手册.md`](../移植手册.md)，逐函数契约见 [`../docs/移植与接口说明.md`](../docs/移植与接口说明.md)。

## 目录

[1 来源](#1-这份包怎么来的) · [2 结构](#2-顶层结构) · [3 逐目录](#3-逐目录说明) · [4 缺什么](#4-包里没有的东西) ·
[5 接入](#5-三分钟接入步骤) · [6 改哪里](#6-换什么改哪里) · [7 自检](#7-动过没动过自检清单)

## 1. 这份包怎么来的

```bash
python project/tools/pack_portable.py     # 在仓库根目录跑
```

把 `project/firmware/` 里可移植的模块复制过来，再加一份**参考板级适配层**
（`Core/` 的 `board.c/.h`、`main.h` 与链接脚本）。

⚠️ **`portable/` 是生成物**：脚本会先删掉 `portable/firmware/`、`portable/host/`、
`portable/docs/` 再重建。要改请改 `project/` 下的源；本 README 与另两份移植文档是
**移植包自带的说明**，重跑脚本前请先备份（当前脚本会连目录一起删，见第 4 节末）。

## 2. 顶层结构

| 路径 | 是什么 | 换板子时 |
|---|---|---|
| `App/Svc/` | 业务/算法：通信、任务解释器、三层控制环、存储、自愈 | ✅ 照搬 |
| `App/Bsp/` | 板级外设：电机 PWM、舵机、编码器、按键、蜂鸣器、台架 | 🟡 按引脚重写 |
| `App/Src/` | `app_ui.c`（界面示例）、`oled_glue.c`（HAL 回调接线）、`oled_diag.c`（屏刷诊断） | 🟡 按需改 |
| `App/Inc/` | 页面号、`app_settings_t`（`g_cfg`） | ✅ |
| `App/Fonts/` | u8g2 字模（12px 正文 / 15px 首页）+ 32×32 图标 | ✅（改文案要重生成） |
| `Middlewares/kk_oled/` | OLED 驱动 + 画图/字体/位图（SSD1315，I2C） | 🟡 换屏/换总线改驱动 |
| `Middlewares/kk_ui/` | 菜单/页面/动画中间件，配置全在 `include/kk_ui_config.h` | ✅ |
| `Middlewares/SEGGER_RTT/` | RTT 日志（调试命脉） | ✅ |
| `参考板级适配层/` | `board.c/.h`、`main.h`、`STM32G474RBTx_FLASH.ld` | ❌ 必须重写板级 |
| `tools/font/` | `check_charset.mjs`（查字）、`gen_font.mjs`（重生成）、`measure_text.mjs` | ✅ |

## 3. 逐目录说明

### 3.1 `App/Svc/` —— 全部照搬

| 文件 | 内容 / 关键点 |
|---|---|
| `host_link.c/.h` | 上位机协议：帧编解码、**58 项参数表 `SPECS[]`**、命令分发、遥测。参数表是契约，与上位机 `bridge/params.py` 必须同序同值 |
| `host_ext.c/.h` | 协议↔业务胶水：驾驶模式、动作表、一键固化、**500ms 链路看门狗**（安全策略只此一份） |
| `task_prog.c/.h` | 任务镜像解析/校验（块 op、条件码、状态表）——纯逻辑，主机可单测 |
| `task_run.c/.h` | 任务解释器：5ms 一拍、全非阻塞 |
| `task_store.c/.h` | 任务落 Flash：页 58 目录 + 页 59..61 程序区（依赖 4KB 擦除块） |
| `nvram.c/.h` | 设置+标定存档：最后一页 `0x0801F800`、v13 格式、CRC32 |
| `motion.c/.h` | 运动调度（TIM6 每 5ms 的总入口，含 `g_isr_*` 耗时统计） |
| `speed_control.c/.h` | 速度环：编码器 → 双轮 PI + 前馈 → PWM；机械参数在参数表里 |
| `gyro_control.c/.h` | 角度环：目标航向 → 左右轮目标速度；`GYRO_DT` 写死 `0.005f` |
| `PID_tracing.c/.h` | 循迹环：8 路灰度重心（1..8，正中 4.5）→ 差速或目标航向 |
| `tracing.c/.h` | 8 路判线：优先模拟量标定结果，回落数字口 |
| `trace_cal.c/.h` | 灰度模块双通道（I2C `0xA2` / UART4）+ 两点标定；换模块改这里 |
| `jy61p.c/.h` | JY61P 解析（0x55 帧、11 字节、校验和）；换 IMU 替换本文件 |
| `pid.c/.h` | 通用 PID 结构体（角度环/循迹环共用） |
| `bus_health.c/.h` | I2C 总线看门狗：连续失败 5 次 → 手动打 9 个时钟恢复 |
| `disp_heal.c/.h` | 显示故障自愈决策（KK_UI 传输错误 → 重试 / 复位总线） |

### 3.2 `App/Bsp/` —— 按引脚/定时器重写

| 文件 | 内容 / 换板子时 |
|---|---|
| `bsp_board.c/.h` | TIM1 舵机 50Hz、TIM8 电机 PWM 23.6kHz、TIM3/TIM4 编码器、TIM6/TIM7 5ms、8 路数字灰度 + 句柄别名 `htim2/htim4`。**必改**：实例、通道、AF、频率 |
| `motor.c/.h` | `Load(left,right)` 双输入 H 桥；**右路取负**（电机镜像安装）。改引脚映射，别改符号约定 |
| `servo.c/.h` | 脉宽 API、角度↔脉宽、非阻塞缓动 `Servo_MoveTo`、动作表；通道号在 `bsp_board.h` |
| `encoder.c/.h` | 16 位 CNT 读增量、**只翻右路**；换电机后 `HP_COUNTS_PER_REV` 跟着改 |
| `key.c/.h` | 旧接口单键扫描（UI 走 `board.h` 的 `keys_raw()`），保留即可 |
| `buzzer.c/.h` / `rgb.c/.h` | 蜂鸣器开关 / RGB 空实现（本板不做 WS2812，可删） |
| `motor_bench.c/.h` | 电机+编码器台架自测（`-DMOTOR_BENCH` 才编入）；换电机后正好用它标定 |

### 3.3 `App/Src/` 与 `App/Inc/`

- `app_ui.c`：**界面示例**（15 页、8 个自定义页、菜单绑定、按键归一化、蜂鸣）；
  `app_ui.h` 的 `APP_UI_DISPLAY_ROT180` 一个开关同时翻显示和按键。
- `oled_glue.c`：把 `HAL_I2C_MemTxCpltCallback` / `HAL_I2C_ErrorCallback` 转发给 OLED 驱动
  ——**异步刷新必须有它**，否则状态机永远收不到完成事件。`oled_diag.c` 是屏刷四组合诊断（`-DAPP_OLED_DIAG`）。

### 3.4 `App/Fonts/`

`kk_font_body.c`（12px 正文）、`kk_font_home.c`（15px 首页）、`kk_icons.c`/`kk_icon_pid.c`。
⚠️ KK_OLED 是**静态子集字模**，没有的字**静默不画**；改文案前必跑
`node tools/font/check_charset.mjs App/Fonts/kk_font_body.c '文案'`。

### 3.5 `参考板级适配层/` —— 每块板必须重写

| 文件 | 内容 |
|---|---|
| `board.h` | 引脚总表 + 板级接口契约（时钟、I2C、UART、INA226、按键、蜂鸣、总线恢复） |
| `board.c` | 上述实现（913 行）：PLL 170MHz、I2C 速率自适应、LPUART1 DMA、INA226、蜂鸣器保险丝、总线恢复 |
| `main.h` | 只 include HAL + 声明 `Error_Handler()` |
| `STM32G474RBTx_FLASH.ld` | Flash 只给 **116K**（页 58 起留给任务区/NVRAM），96K RAM + 32K CCM |

## 4. 包里**没有**的东西

本包故意只带可移植层，下面这些新工程要自己准备：

| 缺什么 | 为什么要它 | 怎么办 |
|---|---|---|
| `Makefile` / IDE 工程 | 构建脚本与工具链绑定 | 照 `project/firmware/Makefile` 抄 |
| `Core/Src/main.c`、`main_app.c` | 初始化顺序与主循环节拍 | 按移植手册第 3/4 节写；顺序错了会静默黑屏 |
| `Core/Src/stm32g4xx_it.c` | 中断向量 | CubeMX 生成后补 `LPUART1_IRQHandler`、`DMA1_Channel2/3_IRQHandler`、`TIM6/TIM7_DAC_IRQHandler` |
| `Core/Src/syscalls.c` | newlib `_sbrk` 桩，缺了链接不过 | CubeMX 自带 |
| `Core/Inc/stm32g4xx_hal_conf.h` | HAL 模块开关 | CubeMX 生成；确认 I2C/UART/TIM/FLASH/PWR/DMA 全开 |
| HAL/CMSIS/启动文件 | 芯片支持包 | STM32CubeG4，只引用不复制 |
| 自检固件（`selftest.c`、`ssd1315.c`、`pa15test.c`、`probe_jy61p.c`） | 焊接体检 | 需要就从 `project/firmware/Core/Src/` 一起拷 |
| 与固件同仓的 6 个上位机测试 | 校验参数表/默认值/任务字节 | 独立上位机已删；见移植手册第 12 节 |

> ⚠️ 本 README 与 `../docs/移植与接口说明.md` 都住在会被脚本删除的目录里：
> **重跑 `pack_portable.py` 前先备份**，或把"生成/保留这两份文档"加进脚本。

## 5. 三分钟接入步骤

1. CubeMX 建空工程（选新主控，配好 HAL + CMSIS + 启动文件），拷 `App/`、`Middlewares/` 进根目录。
2. 拷 `参考板级适配层/board.c/.h`、`main.h` 进 `Core/`，**先只改 `board.h` 的引脚宏**；再改
   `board.c` 的 `board_clock_init()`（HSE 频率、PLL、`FLASH_LATENCY`）与 `bsp_board.c`（定时器/通道/AF/频率）。
3. 写 `main.c` 初始化顺序（移植手册第 3 节）；主循环留三个调用：
   `app_ui_tick()` → `host_link_poll()` → `nvram_poll()/nvram_service()`。
4. 编过之后别急着接电机：先把 RTT 横幅 → I2C 扫描 → 屏点亮 → 按键跑通，再上电机。

## 6. 换什么、改哪里

| 变化 | 必改 | 可以不动 |
|---|---|---|
| 只换引脚（同系列同外设） | `board.h`、`bsp_board.c` 的 GPIO/AF | 所有 `App/Svc/` |
| 换同系列 G4 型号 | `board.h` 引脚、`.ld` 的 Flash/RAM、`Makefile` 的 `DEVICE` | `App/Svc/`、`Middlewares/` |
| 换到非 STM32 | `board.c`、`bsp_board.c`、`.ld`、所有 HAL 调用点 | `bus_health`/`disp_heal`/`task_prog` 等纯逻辑 |
| 换电机/减速比/轮径 | **不用改代码**：上位机改参数 `0x0305`~`0x0308` | 全部 |
| 换 OLED 控制器 / SPI 屏 | `kk_oled/driver/kk_oled_driver.c`（初始化序列 + 传输函数） | `kk_oled` 画图层、`kk_ui` |
| 换 IMU | `jy61p.c/.h`（保持 `Yaw/Pitch/Roll/Gyro_Z`、`jy61p_frame_count()` 符号） | 控制环、任务解释器 |
| 换灰度模块 | `trace_cal.c/.h`（保持 `trace_cal_channel()/level()` 语义） | 循迹环 |
| 换上位机链路串口 | `board.c` 的 LPUART1 段 + `board_dl_*()`，`DL_BAUD` 一处 | `host_link.c` |

## 7. "动过没动过"自检清单

新板第一次编过之后逐条确认（前 6 条不过，后面的都别信）：

- [ ] `clock_report_t.sysclk_hz` 与预期一致，`FLASH_LATENCY` 对得上主频。
- [ ] RTT 打出启动横幅（`SEGGER_RTT_Init()` 在 `HAL_Init()` 之后）。
- [ ] I2C 扫到屏与 INA226（或你板上的从机）。
- [ ] 屏点亮（`OLED_Init()` 成功；`main_app.c` 有 150ms 等待 + 10 次重试 + 总线恢复）。
- [ ] `keys_raw()` 五个方向都能单独读到。
- [ ] TIM6 5ms 中断真的进了 `HAL_TIM_PeriodElapsedCallback()`（RTT 有 `MOTION:`/`ISR:` 行）。
- [ ] 手转轮子 `MOTION: enc l/r` 有正负变化，且两轮朝前进方向推时**都为正**。
- [ ] 上位机握手成功、参数表 CRC 一致（`HELLO` 帧里带着）。
- [ ] 改参数 → 回读变化 → 断电重启 → 值还在（NVRAM 生效）。
- [ ] 屏刷 10 分钟不黑不卡（`FRAME:` 行 `err=0 busy=0`）。
