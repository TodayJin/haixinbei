# firmware/ — 全地形机器人自检固件

给这块 STM32G474RBT6 主板做**上电自检**的工程：一次烧录，RTT 里直接看每一项 PASS/FAIL。
不是最终固件，是**焊接体检 + 外设点亮**用的。

## 依赖

- 🧰 `arm-none-eabi-gcc`（GNU Tools for STM32 14.3，已在 PATH）
- 🧰 `make` / `cmake`（来自 STM32CubeCLT 1.22）
- 🧰 本地 STM32Cube G4 包：`C:/Users/EDY/STM32Cube/Repository/STM32Cube_FW_G4_V1.6.3`
  （只引用，不复制；改路径就改 Makefile 里的 `CUBE_G4`）
- 🧰 SEGGER J-Link（`C:/Program Files/SEGGER/JLink_V974`）
- 🔌 调试口：U12（SH1.0-6P）—— 1=3V3、2=SWDIO、3=SWCLK、4=GND、5/6=LPUART1

## 常用命令

§§§bash
make                 # 编译 -> build/haixin-main.elf / .hex / .bin
make RESCUE=1        # 编译"救援固件" -> build-rescue/haixin-rescue.bin（只跑时钟+RTT，不碰外设）
make LOOPBACK=1      # 编译"UART 自环"固件 -> build-loop/（USART3 每 2s 自发一帧假角度）
make PA15TEST=4      # 编译"PA15 判决实验"固件 -> build-pa15/
make flash           # J-Link 烧录 + 逐字节校验
make clean           # 清 build/
§§§

观测用 **RTT**（不用串口、不用额外接线，J-Link 一条 SWD 就够）：

§§§bash
python "C:/Users/EDY/.dsh/skills/stm32-dev/scripts/stm32-dev.py" rtt --elf build/haixin-main.elf --seconds 6
§§§

> RTT 与 GDB server 互斥（同一台 J-Link）。抓之前先确认没有别的 J-Link 进程；卡住就跑技能的 `cleanup`。

## 目录

| 路径 | 内容 |
|---|---|
| `Core/Inc/board.h` | **引脚与地址总表**（按《固件接口规格.md》/网表 v6）+ 各测试函数原型 |
| `Core/Src/board.c` | 时钟、GPIO、I2C1、INA226、USART3 的板级初始化 |
| `Core/Src/selftest.c` | 自检项：I2C 扫描、INA226……（后续：OLED、按键、JY61P） |
| `Core/Src/stm32g4xx_it.c` | 异常处理（HardFault 会打印 PC/CFSR/HFSR/BFAR 到 RTT） |
| `Middlewares/SEGGER_RTT/` | SEGGER RTT（BSD，取自 github.com/SEGGERMicro/RTT）+ 本项目的 `SEGGER_RTT_Conf.h` |
| `STM32G474RBTx_FLASH.ld` | 128K Flash / 96K SRAM / 32K CCM（**RB 版**，不是模板里的 RET6） |

## 引脚（与网表 v6 一致，详见 `Core/Inc/board.h`）

| 功能 | 引脚 |
|---|---|
| I2C1（OLED 0x3C + INA226 0x40 + 循迹座） | **PA15 = SCL，PB9 = SDA** |
| USART3（JY61P，115200） | PB10 = TX，PB11 = RX |
| LPUART1（无线下载器） | PC1 = TX，PC0 = RX |
| 五向键（低有效，内部上拉） | PC13=上、PC14=下、PC15=左、PC2=右、PC3=中 |
| 蜂鸣器（高有效） | PD2 |
| 电机 PWM → AT8236 | PC6/PC7（电机A）、PC8/PC9（电机B），TIM8_CH1–4 |
| 编码器 | TIM3 = PB4/PB5（电机A）、TIM4 = PB6/PB7（电机B） |
| SWD | PA13 = SWDIO，PA14 = SWCLK |

## 自检结果（每次烧录后更新）

| 项 | 状态 | 备注 |
|---|---|---|
| 时钟 HSE 25MHz + PLL 170MHz | ✅ PASS | `HSE=OK(25MHz) PLL=OK(170MHz) SYSCLK=170000000` |
| RTT 探针通道 | ✅ PASS | 心跳 `[ 1001 ms]` 稳定 |
| I2C1 总线扫描 | ✅ PASS | 扫到 **0x3C(OLED) + 0x40(INA226)** 两个设备 |
| INA226 ID / 母线电压 | ✅ PASS | `MFG_ID=0x5449("TI") DIE_ID=0x2260`；VBUS≈60mV、I=0（12V 未上，正常） |
| OLED SSD1315 初始化 | ✅ PASS | 37 条初始化命令全被接受，8 页显存写入无错 |
| **OLED 实际点亮** | ✅ PASS | **用户肉眼确认有画面**（竖条+棋盘+四角标记） |
| **USART3 收发**（PB10/PB11） | ✅ PASS | `make LOOPBACK=1` + 短接 U1 的 2/3 脚：`bytes` 每帧 +11、`frames` 每帧 +1、`bad=0`、`resync=0`，解出 `roll=99 pitch=-200 yaw=900`（0.1°），与发出的 10°/−20°/90° 一致 |
| JY61P 模块本身 | ⏳ 无模块 | 解析器已被上面的自环验证；模块到了直接插 U1 即可 |
| 五向键 | ⚠️ **部分确认** | **上(PC13)/下(PC14)/右(PC2) 已确认**；**左(PC15)、中(PC3) 尚未按到** |
| 蜂鸣器 / 编码器 / 循迹 / 电机 | ⛔ 暂缓 | 依赖 5V/12V 轨，板上电源件未到 |

> ⚠️ **按键计数曾经有 bug**：`last_keys` 若初始化成 `0xFF`，循环第一次读到空闲态 `0x00` 会被算成"五个键同时刚按下"（`cnt=1/1/1/1/1`）。已改成**进循环前先读一次**。看到 `cnt` 里有怀疑的 1，先确认不是这个。

## ⚠️ 已知问题：烧了带 I2C 的固件后，J-Link 连不上

**现象**：里程碑1（只跑时钟+RTT）随便连；里程碑2（初始化 I2C1）烧完后，J-Link 一律
`Failed to initialize DAP`（4000k/1000k/200k/100k 全试过，JTAG/cJTAG 也失败），VTref 正常 3.29V。

**怀疑**：PA15 = LQFP64 **第 51 脚**，紧挨着 SWCLK PA14 = **第 50 脚**。
- 里程碑1 时 PA15 还是复位默认的 JTDI（高阻+内部上拉）→ 不影响 SWCLK → 能连；
- 里程碑2 把 PA15 配成 I2C1_SCL（开漏），SCL 一拉低就把 SWCLK 一起拽下去，调试口就死了。

**排查顺序**：① 断电量 MCU 第 50/51 脚是否导通（连锡）；② 按住 SW1 进复位态再连；
③ 用 `make RESCUE=1` 烧救援固件（不碰 I2C）验证是否与 I2C 配置相关。
