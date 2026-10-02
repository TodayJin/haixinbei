# 旧固件 -> 新板 移植方案

来源：`D:\Today\haixinbei\Haixin`（STM32F103VET6 + HAL，Keil 工程）。
目标：**上层逻辑不动**，只换**底层驱动**，并把 **OLED 驱动换成 KK_UI**。

## 1. 分层（哪些动、哪些不动）

| 层 | 旧文件 | 处理 |
|---|---|---|
| **上层：任务/算法** | `renwu.c`（任务A/B 状态机）、`tracing.c`、`PID_tracing.c`、`pid.c`、`gyro_control.c` | **不动**，只改 `#include` 与引脚宏 |
| **上层：显示** | `main.c` 里的 `OLED_ShowString/ShowNum/Clear/Update` | 从"主循环直接刷屏"改成 **KK_UI 自定义页**里画（KK_UI 独占整屏刷新） |
| **底层：板级驱动** | `motor.c` `servo.c` `encoder.c` `key.c` `buzzer.c` `rgb.c` `jy61p.c` | **重写**为新板引脚/外设（保留同名对外 API） |
| **底层：显示驱动** | `OLED.c`(48KB 江协科技软件I2C) + `OLED_Data.c`(字库) | **整体删掉**，换成 `kk_oled`(图形+SSD1315驱动) + `kk_ui`(菜单UI) |
| **底层：MCU** | CubeMX 生成的 `Core/*`（F103） | 换成本工程 `firmware/Core`（G474，自检工程已在用且已验证） |

## 2. 引脚/外设映射（旧 -> 新）

| 功能 | 旧板 (F103) | 新板 (G474) | 备注 |
|---|---|---|---|
| 循迹 8 路 | PA4,PA5,PC4,PC5,PB0,PB1,PE7,PE8 | **PA0–PA7 = huidu1–8** | 主用改 I2C 模块；数字线留备用，正好 8 路 |
| 电机 PWM | TIM8_CH1/CH2 = PC6/PC7 + 方向 PD6/PD7、PB4/PB5 | **PC6/PC7=电机A、PC8/PC9=电机B（TIM8_CH1–4）→ 2×AT8236** | 新板**无独立方向脚**：每电机两根输入，一根 PWM、一根恒低即一向 |
| 舵机 | TIM1_CH3/CH4 = PE13/PE14 | **PA8–PA11 = TIM1_CH1–4** | 现有 4 路 |
| 按键 | PC1/PC2/PC3（3 键） | **PC13/PC14/PC15/PC2/PC3（五向键）** | KK_UI 要"上/下/确定" → 上=PC13、下=PC14、确定=PC3(中) |
| 蜂鸣器 | PD10 | **PD2** | 高有效 |
| RGB | 若干 GPIO | **无（不做 RGB）** | `rgb.c` 保留空实现，`RGB_blink()` 变空函数 |
| 编码器 | 代码里整段注释掉了 | **TIM3=PB4/PB5（电机A）、TIM4=PB6/PB7（电机B）** | 新板可用；旧代码没用，先保持不用 |
| 陀螺仪 | USART3 PB10/PB11 | **USART3 PB10/PB11（一样）** | 新板已验证自环 OK |
| OLED | I2C1 PB8/PB9（软件 I2C） | **I2C1 PA15=SCL / PB9=SDA（硬件 I2C）+ SSD1315 裸屏** | 新板已验证 0x3C 应答且点亮 |
| 定时节拍 | TIM2/TIM4 中断 | **TIM6/TIM7**（TIM3/TIM4 已给编码器） | 旧代码 TIM4 跑陀螺仪任务、TIM2 跑循迹 PID |

## 3. KK_OLED / KK_UI 接入

- 运行时装在 `firmware/Middlewares/kk_oled` 与 `firmware/Middlewares/kk_ui`（MIT，可改；**不从 .agents/skills 资产目录参与构建**）。
- **驱动已按本板重写**：`kk_oled/driver/kk_oled_driver.c` 由上游 CH1116 版改成 **SSD1315**：
  地址 0x3C、列偏移 0、初始化序列取屏厂 GME12864-41 手册（`8D 14` 开内部电荷泵、`AD 30` 用内部 IREF）。
- KK_UI 要求应用提供 5 个自定义页适配符号：
  `KK_UI_CustomOnEnter / OnLeave / OnInput / OnTick / OnDraw`。
  旧的"循迹测试 / 任务A / 任务B"实时画面就放在**自定义页**里画。
- 字体是 **u8g2 格式**，KK_OLED 只带解码器不带字模 → 用 `kk-oled-font` 流程在线生成字模子集。

## 4. 菜单设计（"对应"旧固件的功能）

旧固件只有两屏：`循迹测试`（8 路 + 偏航角）和 `任务选择 1.A 2.B`。用 KK_UI 重新组织：

```
首页（大图标横滑选择器）
 ├─ 循迹测试   → 自定义页：8 路二进制条 + 偏航角 + 电机占空比
 ├─ 任务 A     → 自定义页：实时状态，按键启动/停止
 ├─ 任务 B     → 自定义页：同上
 └─ 设置       → 普通菜单
      ├─ PID 参数   → Kp / Ki / Kd / 目标速度（整数编辑）
      ├─ 陀螺仪     → 只读信息页：Roll/Pitch/Yaw、零偏
      ├─ 舵机测试   → 菜单：收起/准备/打下1/打下2…（对应 Servo_Move 的 1–14）
      ├─ 蜂鸣器     → 开关编辑
      └─ 关于       → 信息页
```

动效来自 KK_UI 本身：首页图标滑动选择、页面转场、焦点框、Toast、确认框。

## 5. 阶段

| 阶段 | 内容 | 现在能否验证 |
|---|---|---|
| **P1** | KK_OLED+KK_UI 在新板点亮，显示首页/菜单 | ✅ 现在就能（3.3V 够） |
| **P2** | 生成中文字模、菜单项换成中文 | ✅ |
| **P3** | 旧上层 `renwu/tracing/gyro/pid` 编译进来，实时数据进自定义页 | ⚠️ 显示可验，电机/陀螺仪要等电源件 |
| **P4** | 底层 BSP 重写：motor/servo/key/buzzer/jy61p 按新板引脚 | ⚠️ 等 5V/12V |
| **P5** | 整机联调（循迹、任务 A/B） | ⛔ 等电源件 + 上场地 |
