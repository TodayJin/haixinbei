# PROJECT

> 这个项目**现在**是什么。就地编辑，控制在一屏内。
> 这里写的内容和代码冲突时，以代码为准，并在同一次改动里把这个文件改对。

## 这是什么

**全地形机器人**参赛项目。赛项：**海信杯 · 第七届吉林省大学生人工智能创新大赛 · 十、全地形机器人自主创新设计比赛**——规则、场地图与 11 个障碍尺寸见 `docs/全地形机器人比赛规则与场地.md`（原文 PDF 在 `docs/`）。

> **命名（用户 2026-09-24 明确）**：**海信杯**是**比赛**的名字，**不是**车的名字。
> 车/板就叫「全地形机器人」，全仓库的"海芯"字样已清掉。目录 `Haixin/`（旧板固件）
> 与产物名 `haixin-app.bin` 是历史文件名，未动。

场地 450×450cm 宝丽布 + 黑线引导（线宽 3.8cm），**11 个障碍**，5 分钟内按「**单次运行的最高障碍得分**」计分（满分 190 = 障碍 185 + 计时 5）。

**赛道硬约束（做机械/结构前必看）**：隧道净高 **20cm**（整车含舵机臂总高必须 < 20cm）、窄桥桥面宽 **25cm**、要爬 16.2cm 窄桥坡 / 10.8cm 小型阶梯 / 约 20.5cm 高台（45 分，**只需登上不必通过**）。

`Haixin/` 是上一版能完赛的参考固件（STM32F103VET6、HAL+FreeRTOS、Keil MDK-ARM）；**旧固件 `renwu.c` 里的注释（2级台阶/木刺/栅栏/减速带/草/方形通道/UUUUU/超级大坡）就是这条赛道**，可逐条对上官方 11 个障碍。

本工程的目标（用户原话）是**画一块新的 PCB 主板来完成比赛**：主控 STM32G474，新增 AB 相编码器测速、升级 IMU、无线通信、电源监测；12V DC 供电；PCB 用立创EDA绘制、嘉立创打板。旧固件仅作参考。

## 怎么跑和怎么测

构建 / 烧录：**Makefile 在 `firmware/` 下（仓库根没有 Makefile）** ⇒ `make -C firmware`（产物 `firmware/build-app/haixin-app.elf`）、`make -C firmware flash`（走 `tools/flash.ps1`，该脚本必须存成 UTF-8 with BOM）。变体：`SELFTEST=1` / `DIAG` / `MOTOR_BENCH=1`；`firmware/test/` 不进固件构建。

界面取证（要连着板）：`out/_lcd.ps1 -Png <文件>`（抓屏 + page-major 解包；它现在会先删旧文件——之前不删导致 savebin 失败时拿旧数据报假结论）、`out/_wiz.ps1`（标定向导按键注入）、`docs/灰度标定与检验.md` + `out/_gray.ps1`。

**离线（不接板）检查**：
- 主机侧单元测试：`pwsh -File firmware/test/run_tests.ps1`（可 `-Only <正则>`）。测试编 PC 上跑，带 `-fsanitize=undefined -fsanitize-undefined-trap-on-error`（MinGW 没有 asan/ubsan 运行库，trap 模式不需要）。当前 **9 个测试**（t_jy61p / t_pid / t_tracing / t_hostlink / t_speedctrl / t_gyrotest / t_motion / t_bushealth / t_tracecal）合计约 12.3 万项检查，脚本汇总“通过 9 / 失败 0”。新测试的源码依赖写在脚本的 `$deps` 表里。
- 静态分析：`pwsh -File tools/check_static.ps1`（逐文件 `-fanalyzer -Wall`，必须 `-c -o`，`-fsyntax-only` 时分析器不跑）。基线：49 文件、只有 8 处故意 `while(1)` 的 `-Wanalyzer-infinite-loop`、0 error。
- 更严告警集：`pwsh -File tools/check_warnings.ps1 [-Out 文件] [-List]`（比正式构建多 `-Wextra -Wswitch-enum -Wshadow …`，43 个文件；白名单 28 条框架必需告警，待处理必须为 0；**它靠“.o 是否真的生成”判断编译是否真跑了**，第一版只拿纯文件名 ⇒ 假“0 告警”）。
- 比赛流程状态机检查：`$env:PYTHONIOENCODING='utf-8'; python tools/renwu_states.py [-Strict] [-Out out/renwu_states.txt]`（纯文本画 `renwu.c` 的 `black_num` 状态图；当前 21 可达、死分支 8/12/14/15/16/20/22，未改）。

**上板（连着板、用 J-Link + RTT）**：
- RTT：`JLinkRTTLogger.exe -Device STM32G474RE -If SWD -Speed 4000 -RTTChannel 0 <日志>`（**不复位目标**，没有启动横幅就是没复位）；1Hz 只有 4 种行：`FRAME` / `HOST` / `ISR`（尾带 `i2c recover=N`）/ `MOTION`（尾带 `fault=N`）。
- J-Link 命令行：`JLink.exe -CommanderScript <临时.jlink> -ExitOnError 0`，脚本含 `si SWD`/`speed 4000`/`device STM32G474RE`/`connect`/`halt` + 命令 + `go`/`qc`。**最后必须有 `go` 或 `qc`**，否则掉进交互模式永远等 stdin（实测卡死过 12 分钟）；`connect` 不会复位正在跑的目标；`mem32/mem8/w1/w4/savebin` 可用，数字参数按十六进制解析。
- 已写好的台架脚本（都在 `out/`，符号地址**从 `firmware/build-app/*.o` 取大小、从 `haixin-app.elf` 取地址**，重名按 size 消歧）：`_steptest.ps1`（角度环台阶，`-Deg/-Samples/-Ramp/-RateKp/-RateKd/-CorrSlew/-CorrLimit/-Decel/-Apply`，RAM 调参收尾写回原值）、`_tracetest.ps1`（循迹 pivot，`-Mode read|pivot`、`-Step/-Setpoint/-Kp/-Ki/-Kd/-Offset/-OffsetMs/-SettleMs/-Switches`）、`_grayread.ps1`（只读正交/重心/丢线/模式）、`_losttest.ps1`（丢线衰减直测）、`_lcd.ps1`。
- **跑 pivot/循迹实验前先 `pwsh -File out/_grayread.ps1` 确认 `LIT≥2`（线在探头下）**，否则整轮白跑（已踩两次）。

字体子集（改了界面文案必须跑）：`node tools/font/check_charset.mjs firmware/App/Fonts/kk_font_body.c "字符串"`。

## 东西都在哪

**工程根 = 仓库根** `D:/DATE/STM32/G474RBT6/haixinber`（2026-09-30 起：原本多套一层 `haixinbei/` 子目录，工程内容已上移到根，`haixinbei/AGENTS.md` 随之删除）。下文所有相对路径（`firmware/`、`tools/`、`docs/`、`memory/`…）都相对这个根；`firmware/Makefile` 里的 `../tools/flash.ps1` 也因此在两种布局下都成立。

（`firmware/` 下现有 `build-app/`（正式固件）、`build-bench/`（`MOTOR_BENCH=1` 台架）、`build-local/` 三套构建目录；`build-app/` 只留 `.o/.d` 时说明被 clean 过，烧录前先 `make -j8`。）

- `固件接口规格.md` — **硬件接口权威文档**（引脚/外设/寄存器/坑清单）。
- `docs/全地形机器人比赛规则与场地.md` — **赛项规则摘要 + 场地图 + 11 个障碍尺寸 + 对机械/固件的硬约束**（原文 PDF 在 `docs/`）。地图只在这份文档里，**上位机没有任何地图/赛道仿真**。
- `firmware/` — **固件工程（双目标）**：`Core/`(G474)、`App/`(`Svc/` = renwu/tracing/gyro/pid/**host_link**/**host_ext**；`Bsp/`；`Src/app_ui.c`；`Fonts/`)、`Middlewares/`。
  - **上位机链路已实现并编译通过**：`App/Svc/host_link.c` + `board.c` 的 LPUART1（PC0=RX/PC1=TX，**AF8** 查手册确认）+ 512B 环形缓冲 + `LPUART1_IRQHandler`，主循环 `host_link_poll()`。
  - **未做**：参数写 Flash（回未就绪）、编码器测速（遥测 v_l/v_r 恒 0）、陀螺在线标志、以及**把 `pid.c`/`gyro_control.c` 的 `#define` 换成 `host_param_get()` —— 在这之前改参数不影响控制律**。验证步骤见协议文档 §10。
- `tools/console/` — **上位机（新，唯一的一套；旧 Qt 上位机 `tools/host/` 已于 2026-10-03 删除，需要时从 git 提交 `5051a4d` 取回）**：
  - `bridge/` = 纯标准库 HTTP/SSE 桥（`protocol.py` 编解码、`params.py` 参数镜像 43 项、`state.py` 链路状态、`sim.py` 仿真车、`server.py` 路由、`link.py` 串口、`main.py` 入口）；`web/` = React+TS+Vite+Tailwind+uPlot 前端（`web/dist/` 是构建产物，**要进版本库**）；`docs/API.md` = HTTP/SSE 契约（SSE 的 `data` 就是载荷本体）；`tests/` = 47 项 pytest。
  - 跑：`python -m bridge.main --sim`（无硬件）/ `--serial COM<n>`（真车）；前端改动后 `npm run build`（在 `web/` 下）。
- `docs/上位机通信协议.md` — 通信协议（帧/CRC16/32B 遥测/参数表/**§8.1 固件实现状态**/**§10 硬件验证步骤**）。位姿帧 T_POSE 已随地图删除。
- `docs/控制算法升级方案.md` — 分段惯性导航 + 状态机改造方案（P0→P3）。
- `.agents/skills/`；`pcb/`；`docs/datasheets/`；`tools/`；`out/`(可删产物)
- `Haixin/` — 旧板固件（F103，Keil，GBK）；`memory/` — 记忆文件。

根目录**不放**新生成的文件，规则见 `AGENTS.md`。

## 现状

固件已完成十一轮（`docs/自主优化记录.md` 第 1~17 节）。上板实测过的：ISR 9/12µs、陀螺看门狗、I2C 自愈、软启动 A/B、循迹丢线收回、运动中不写 Flash、pivot PID（`PID_PIVOT_KP/KI/KD = 900/0/405`，经 `PID_SetPivot()` 在 `motor_tracing_pid_speed()` 里按 base_speed==0 切换，已实测生效且比旧的 2700 组好）。三级串级 `motor_tracing_cascade()` 已写好但**仍是死代码**（没人选 `MOTION_TRACE_GYRO`），`renwu.c` 直行 97 处仍走旧两级 `motor_tracing_pid(4.5)`；pivot 场景实测 **`s_angle_per_unit = 10.0f` 保持不动**（14 次受控实验否证了“往 5~7 调”的推演），接线未做、直行未放开。界面侧最新：`PAGE_CUSTOM_SERVO 17`（舵机测试：两路角度 + 「收回」0/0 + 14 号「动作预设」占两行显示动作名 + 返回保持角度）；动作表/名字在 `firmware/App/Bsp/servo.c` 的 `k_action_pose[14][2]` / `k_action_name[14]` 单一真值来源。**注意 `bsp_servo_init()`（`bsp_board.c:227`）以前全工程没人调、舵机一直没初始化，第十一轮才修**。最新尺寸：text 111012 / data 368 / bss 15264，烧录回读一致 111388 字节。

## 坑

- **移植 `Haixin/` 里的 PWM 数值前必须换算：旧板满量程只有 1000，新板是 7200（差 7.2 倍）**。旧板 `Haixin/Core/Src/tim.c:203-205` 的 TIM8 是 `Prescaler=71 / Period=1000-1`（72MHz/72=1MHz → 1kHz，满量程 1000），而 `Haixin/Hardware/motor.c:35,46` 的 `Load()` 把 `abs(moto)` **直接写进 CCR** —— 所以 `Haixin/Hardware/motor.c:3` 那句 `#define PWM_MAX 7200` **是够不着的**（CCR>999 硬件就顶到 100% 占空比；它只在 `Limit()`（motor.c:51-57）里用）。旧码的真实含义：循迹设定速度 `1000`（`Haixin/Hardware/PID_tracing.c:45`）= **满油门**、慢速档 `200`（`Haixin/Hardware/PID_tracing.c:94`）= 20%、任务流程那串 `Load(900,900)`/`Load(920,900)`/`Load(900,935)`（`Haixin/Hardware/renwu.c:82,109,124,261,276,288,439,465,598,615,632,647`）= **90% 占空比**、原地转 `±500~700`（`Haixin/Hardware/renwu.c:131,601,618,634,653`）= 50~70%。换算到新板（`firmware/App/Bsp/bsp_board.h:28` `MOTOR_PWM_MAX 7200`、`firmware/App/Bsp/bsp_board.c:176` `htim8.Init.Period = MOTOR_PWM_MAX - 1U` = 7199，PSC=0 @170MHz → 23.6kHz）：旧 1000 → 7200、旧 900 → 6480、旧 500 → 3600、旧 200 → 1440。**`firmware/App/Svc/renwu.c` 里那串 900/920/-700 是从旧板原样抄来的数**（连行号都对应），在新板只有 12.5% 占空比，低于 35~40% 的静摩擦死区 → 任务流程按旧的数**根本推不动这辆车**。「循迹速度」默认 `6480`（`firmware/App/Src/app_ui.c:27` 的 `.trace_speed = 6480`，由 `push_pid_params()` 推给 `HP_BASE_SPEED`）= 90% 占空比 ≈ 旧码刻度 900。
  **2026-09-30 已按此同步（判据与清单见 `固件接口规格.md` §5.1）**：`firmware/App/Svc/renwu.c` 的 45 处 `Load(...)`/`Toggle_Gyro_Control(1,...)` 数值全部包成 `RW(v)` = `v × HP_TASK_SPEED / 900`（新参数 `HP_TASK_SPEED` = 0x0304，默认 **6480** = 旧板 900 = 90% 占空比，改这一个参数就能整体缩放比赛流程）；`firmware/App/Svc/PID_tracing.c` 的旧接口 `motor_tracing_pid/_0/_0_task` 从硬编码 `1000`/`200` 改成读 `HP_BASE_SPEED`（默认 **6480**，原 1000）/`HP_BASE_SPEED_SLOW`（默认 **2500**，原 200 —— 200 在新刻度只有 2.8% 占空比，取 2500 = 死区下沿）。
  **增益类参数已按 7.2× 重标（2026-09-30 定案，不再是待办；但仅限循迹环）**：`line_kp/ki/kd`（0x0201-0203）的量纲是「PWM 刻度 / 误差单位」，且循迹环的输出**直接加在左右轮 PWM 偏置**上（`PID_tracing.c` 的 `l = base - pid_output; r = base + pid_output`）⇒ 旧板 1000 刻度上标定的值照抄过来只有 1/7.2 力度。现默认值 `line_kp 2700 / line_ki 10.8 / line_kd 360`；**积分限幅不折算**（`line_ilimit 50`、`gyro_ilimit 1000` 不变，量纲是误差本身）。默认值三处必须同时改（`host_link.c` SPECS、`app_ui.c` `g_cfg`+`pull_pid_params()` 兜底、`pid.c`/`gyro_control.c` 的 `*_FALLBACK`）+ 上位机镜像 `tools/host/protocol.py`；核对已自动化：`python tools/host/check_param_table.py`（解析固件源码，**不要**再手抄一份表 —— 手抄的 out/_param_crc_check.py 就自己撒谎过一次）。
  **角度环是 ×7.2 的例外（2026-10-01 原地台架重定）**：旧刻度换算值 `gyro_kp 720 / gyro_kd 72` 在本板**自激**（Δ=3°~20° 全振荡，尾晃 20~38°、峰值 400~600°/s、原地来回甩），换算出 `GYRO_CORR_LIMIT 2500` 也压不住。机理：角度环输出是**轮速目标**（不是 PWM 偏置），plant 增益 k_v≈0.06 (°/s)/PWM、有效滞后 τ≈30~50ms ⇒ 环路增益 Kp·k_v = 43/s（Kp=720）远超 1/τ。现默认 **`gyro_kp 150 / gyro_ki 10.8 / gyro_kd 15`**（≈Kp/10 的 D 压过冲），实测 Δ=20° 过冲 6.4°/稳态误差 0.64°/尾晃 0.01°、3° 阶跃 0.44s 到位误差 0.6°、6×15° 转 90° 累计误差 0.4°。**改这两项要连 `app_ui.c` 里的一次性旧值迁移一起改**（`CFG_GYRO_KP_OLD_X100 72000`/`CFG_GYRO_KD_OLD_X100 7200` → 15000/1500，写在 `app_ui_settings_loaded()` 开头、`push_pid_params()` 之前；`make flash` 不带 `-MassErase` ⇒ 存档保留，正好靠它验迁移）。调参台架 `out/_angletest.ps1`（只写 RAM，不动固件）。
- **“速度”在本工程里只有一个单位：PWM 刻度 ±7200**（TIM8：PSC=0 / ARR=`MOTOR_PWM_MAX-1`=7199 @170MHz → 23.6kHz；7200 = 100% 占空比，1 刻度 = 0.0139%）。`Load()` / `Set_Pwm()` / 速度环目标 / 速度环输出 / 循迹与陀螺的输出，**全都是这把尺子** —— 不是 0–10000、不是 rpm。**唯一例外是编码器**：`Encoder_ReadDelta()` / `SpeedControl_Counts()` / `SpeedControl_Mmps()` 的单位是「**计数 / 5ms**」（TIM6 每 5ms 调一次 `SpeedControl_Task()`）；1980 计数 = 输出轴一圈（11PPR × 4 倍频 × 45 减速），电机轴自己一圈只有 44。另外 **`host_link.h` 的 `HP_PWM_MAX` 是参数 ID `0x0303`，不是数值 771** —— 默认值 7200.0f 在 `host_link.c` 的参数表里。
- **速度环使能时 `Load()` 是无效的（会被每 5ms 盖掉）**：`SpeedControl_Task()` 末尾用环的输出写 PWM（`speed_control.c:298`）；只有在 `SpeedControl_Stop()` 之后（`s_enabled = 0`）裸 `Load()` 才是主人（`renwu.c` 全流程、旧开环代码都靠这个）。所以「模块测试 → 电机」页的滑条走 `SpeedControl_Enable(1U) + SpeedControl_SetTarget()`（**闭环速度目标，用户 m03300 定**，`app_ui.c` 的 `motor_test_apply()`），并且**两路都划到 0 时必须 `SpeedControl_Stop()`**：只 `SetTarget(0,0)` 会带着热积分按 `ki·∫err`（限幅后最多 ±6000 PWM）反向踹一脚。同一份判断在 `gyro_control.c:281` / `PID_tracing.c:66` 也有（只在 `!SpeedControl_IsEnabled()` 时回退 `Load()`）。
- **速度环的输出 = 摩擦前馈 + PI，前馈常数是「硬件标定量」不是「调参量」**：`out = SPEED_FF_SLOPE×目标 + SPEED_FF_FRIC + kp·err + ki·∫err`，现用 `0.662` / `2327`（= 这台 JGB37-3865-520 在 12V 下的静摩擦 + 电压-转速斜率）。**换电机、换减速比、换电池电压都必须重标**，重标方法是台架 BENCH_MODE 0 的闭环扫表，不是凭手感拧 kp/ki。校验标准：目标 5000 时前馈算出的值应该≈闭环实际稳定输出（现为 5637）。
- **台架做前馈扫表时，每个窗口之间必须 `SpeedControl_Stop()`**：不复位会把上一窗口前馈攒下的积分叠到新前馈上，输出直接顶到 7200 饱和，整窗都在退积分、数据全假（`out/bench_ff1.log` 就是这么作废的）。
- **台架的闭环是自己 `HAL_Delay(1)` + `next+=5` 人工喂 `SpeedControl_Task()`**，实际拍长在 5/6ms 之间抖；生产是 TIM6 精确 5ms。台架会**高估**低速的不稳。
- **低速段（<1500）的偏差是静摩擦粘滑极限环，调参消不掉**：实测整窗有 15~17% 的拍增量为 0、PWM 指令峰峰 1300 在来回抽；目标 1000 偏差 −8%、1250 +5%。这是物理下限，靠上层限速规避。
- **KK_UI 的自定义页没有内置“返回”** —— 必须在应用自己实现的 `KK_UI_CustomOnInput()` 里处理，否则**进去就出不来**（"系统状态"页曾因此卡死，按键只能改屏上那个二进制数）。本工程约定 **按【上】= 返回**（`KK_UI_CustomRequestClose()`），标题栏右上角画了 `<` 提示。注意 KK_UI 的信息页是 **OK 返回**，自定义页的 OK 要留给页面动作（任务页启停）。**例外**：「模块测试 → 电机」页的返回是标题行上的一项（左上「返回」/右上「清零」），上/下（或左/右在两项间）选到它再按中键确认 —— 页面本身把这两个标签画在屏上，以屏幕为准。
- **`#ifdef X` 配 `#define X 0` 仍然为真** —— `#ifdef` 只看“有没有定义”。台架模式开关踩过一次（连烧两版都是同一个模式，一度误判成 RTT 缓冲没刷新）。**选模式一律用 `#if (X == n)`**；定位靠对 bin 做字节搜索。
- **极性/方向类的改动必须成对**：`motor.c` 的 `Load()` 和 `encoder.c` 的 `Encoder_ReadDelta()` 只有一起翻才保持「正 = 前进」。只翻一处 → 速度环变正反馈 → 输出顶到 7200 饱和。
- **编码器只知道“转了多少、往哪个符号走”，不知道车轮相对车头是前是后** —— 方向这一条物理上只有眼睛/手能定（手推轮子看两路符号 + 目视两轮转向），纯软件推不出来。
- **这类减速电机有巨大的静摩擦死区**（实测 7200 满量程里占 35–40%）：起转要 ~2600，但转起来后转速近似正比于 PWM。所以**低速不可控**，PI 会进粘滑极限环。`speed_control.c` 的 `SPEED_MIN_ACTIVE 500` 比真实死区低一个数量级，等于没补（但把它改成 2600 也不会变好，只是从“不转”变“转太快”）；现在真正把工作点抬起来的是摩擦前馈 `SPEED_FF_FRIC`。
- **两轮的静摩擦起转阈值不一样，右轮明显更高**（开环实测右轮 3000 完全不动、3500 才动，且这个阈值本身会在 3400~3550 之间漂）。所以低速段前馈给不够「越过右轮阈值」时，右轮整窗有 8~20% 的拍在停、稳态均值被拉低 5% 以上（这就是 ff6 里 R@1250 是 −6.2% 的原因）。
- **运动控制节拍只有一处可改：`bsp_board.c` 的 `BSP_TRACE_PERIOD_MS`（现 `5U`）—— 改之前先知道 `gyro_control.c` 的 `GYRO_DT 0.005f` 是写死的**（喂给 `PID_Compute_z`），改节拍会让角度环**静默**按错的 dt 算；`speed_control.c` 的 `SPEED_PERIOD_MS`/`SPEED_FULL_COUNTS`、`PID_tracing.c` 的消抖窗口、以及 `integral += err`（**没乘 dt，所以 ki 要跟着反比改**）都得一起动，全链路目标刻度 0–7200 也保不住。2026-09-30 的结论是**不动节拍**，只把测速改成 20ms 滑动平均（`speed_control.c` 的 `SPEED_AVG_TICKS 4`，反馈仍归一化成「计数/5ms」，所以增益一个都不用重调）。注意口径：**`SpeedControl_Counts()` 报的是本拍原始增量（未平滑）**（`main_app.c` 的 RTT MOTION 行打的就是它），滑过的是 PI 反馈和 `SpeedControl_Mmps()`。
- **不要用 `cd`**：pwsh 工具用 `workdir` 参数。`cd` 失败时 make 会在错的 cwd 跑，把错误伪装成 `No rule to make target`。
- **`firmware/Makefile` 是 UTF-8（中文注释），改它别碰 GBK**：2026-09-30 一次「按 GBK 读入 → ASCII 替换 → 按 GBK 写回」的改动把它写成混合编码 + 十来个字节被 GBK 解码器吞成 `?`（`read` 工具直接报 invalid UTF-8），已按行修复回纯 UTF-8（替换表 `out/_makefile_lines.txt` + 脚本 `out/_fix_makefile.ps1`，改前备份 `out/Makefile.before_fix.bak`）。要改就用 `edit` 工具，或「严格 UTF-8 读入 → 纯 ASCII `.Replace()` → UTF-8 写回」（见 `tools/_makefile_add_motor_bench.py`，每个锚点带 `count != 1` 断言）。注意 `edit` 工具的锚点比 `read`/`grep` 的显示**少一个前导空格**（显示会多印一列）。“按 latin-1 字节读入 → 只做纯 ASCII 替换 → 原样写回”（见 `tools/_makefile_add_motor_bench.py`，每个锚点带 `count != 1` 断言）。
- **旧固件源码编码是混的**：部分 GBK、部分 UTF-8；统一按 GBK 转会吞换行把结构体成员注释掉。必须**逐文件自动识别**（先严格 UTF-8，失败再用 GBK）。
- **STM32G4 的 DMAMUX1 时钟必须显式开**（`__HAL_RCC_DMAMUX1_CLK_ENABLE()`），否则异步 DMA **静默永不传输**，而阻塞式初始化照常成功，极具误导性。
- **显示刷新**：I2C1 400kHz + **中断(IT)** 已验证可靠；`400kHz+DMA` 实测**偶发黑屏**（非阈值，是 HAL "中断发命令→DMA 发数据" 的交接竞争），已弃用。改 Makefile 的 `KK_UI_REFRESH_MODE` 可切回。
- **写自定义页文案前先知道两条硬约束**（2026-10-01 给"灰度原值"排末行时踩的）：①`kk_font_body` 是 12px 字，**一屏竖直只放得下 3 行**（标题 y=1 / 正文 y=16、29 / 末行 y=44），y=41 与 y=53 会跟上下行**压字**（行距要 ≥13）；②本工程链的是 **newlib-nano，`snprintf` 的 `%f` 静默输出空串**（数字整个不画、编译无警告），小数必须自己拆成整数打印（`(uint32_t)(x*10.0f+0.5f)` 再打 `%u.%u`）；改对了还会顺带掉几 KB float printf 代码。
- **判定异步有没有真搬数据**：测 `OLED_IsBusy()` 时长 —— 8 页×128B，100kHz 应≈92ms、400kHz 应≈23ms；对不上就是"假完成"。比只看 `err=0` 可靠。
- **STM32G4 的 BOOT0 就在 PB8**；**AT8236 与 AT8236N 第 4 脚定义不同**（VREF vs nFAULT），本板必须买 AT8236（C2827823）。
- **只从调试口接 3.3V 供电时 5V 灯会微亮**（U9 体二极管倒灌，正常）；RT9013 输入上限只有 6V，放在 5V 轨上只有 1V 余量，**别快速反复开关机**（buck 过冲会打穿它）。
- 网表里"多出来的引脚号"是**散热焊盘**（AT8236 的 ESOP-8 有 9 号焊盘）和**连接器固定脚**（SH1.0-4P 有 5/6 脚），不是错误。
- 查 STM32G4 引脚/复用/电平：`docs/datasheets/stm32g474re.pdf` + `tools/extract_pins.py`，不要靠记忆猜。
- **memory/SESSIONS-archive.md 记的是另一个项目**（夹爪 RS485↔CAN 转接板 interface-adapter：Dobot Nova 末端 / DM-J3507 / KP_APR 那些条目）。它是本工作区早期遗留；因为 `memory_search` 会把归档一起搜，这些条目会混进结果，别当本工程事实。（2026-09-30 核对：三个活文件里 0 次提及它，归档里 77 处。）
- **烧录 `device` 字段两处记录互相冲突（未定论，先信回读）**：本文件上文「另一条烧录路径」那条要求 `STM32G474RB`；而 DECISIONS.md 2026-09-30 的实测记录是反向的——本机 J-Link DLL 6.82c 下 `-device STM32G474RB` **静默假成功**（打印 O.K. 却一个字节没写、回读 0x0801F800 全 FF），换 `STM32G474RE` 才真写进去（81920 字节 + savebin 回读一致）。`tools/flash.ps1` 现在用 RE，2026-09-30 多次 `make flash` 都以「回读一致」收尾。下次动烧录**一律以 savebin 回读为结论**，别只看屏幕上的 O.K.。
- **NVRAM 页擦除必须 BKER=1 + PNB=31，别用 `HAL_FLASHEx_Erase()`**（2026-09-30 修 m03645 的「自动保存反复弹提示」时定案）：本板 Flash 实测是 **2 bank × 32 页 × 2KB**，`0x0801F800` = **bank2 页 31**；而 `FLASH_OPTR` 的 `DBANK` 位（`FLASH_OPTR_DBANK_Pos=22`）读出来是 **0**（与硬件行为不符）⇒ HAL 的 `FLASH_PageErase()` 会清掉 BKER、用 `PNB=63` 去擦，**PNB 越界被硬件静默忽略**（`FLASH->SR` 不置错、不置 EOP、页不变）⇒ 擦除永远无效 → 在没擦的地方编程得 `PROGERR` → 记录写不进去 → 每 5s 重试一次并反复弹「正在保存」。`nvram.c` 现在用本地 `nvram_page_erase()`（`CLEAR_BIT(CR, PG|PER|PNB|BKER|MER1|MER2)` → `SET BKER` → `PNB=31` → `PER` → `STRT` → 轮询 `FLASH_SR_BSY` 50ms 超时）。判据（J-Link 手写寄存器，CPU 可先 `halt`，KEYR 用 `0x45670123`/`0xCDEF89AB` 解锁）：`w4 0x40022014, 0x108FA`（BKER|PNB=31|PER|STRT）⇒ 读 `0x0801F800` 立刻变 `FFFFFFFF`；而 PNB=62/63 连擦三次都是 SR=`0`、页内容不变。另：**别把 `PER` 写成 bit0 的 `PG`**（正确 `FLASH_CR_PER_Pos = 1`），写错会得到 `PGSERR(0x80)` 的假证据（2026-09-30 在这上面绕了三轮）。
- **J-Link Commander 的 `erase` 是整片擦除**（`make erase` 用的就是它；带不带地址范围都一样）：2026-09-30 想只清 NVRAM 页（`erase 0x0801F800 0x0801F800`）结果连 app 一起擦了（读 0x08000000 全 FF、板子死），只能重新 `make flash`。要清单个扇区用 CubeProgrammer，别用 J-Link；清 NVRAM 走固件自己的写盘路径（`stm32-dev.py write` 写默认值）。
- **`tools/flash.ps1` 必须存成「UTF-8 with BOM」**（`EF BB BF` 开头）：无 BOM 时 Windows PowerShell 5.1 按 GBK 读中文注释 → `ParserError: UnexpectedToken` → `make: *** [Makefile:175: flash] Error 1`。`edit`/`write` 工具会剥掉 BOM，改完这个脚本要把 BOM 补回来（`[System.IO.File]::WriteAllText($p,$t,(New-Object System.Text.UTF8Encoding($true)))`）；`firmware/Makefile` 现在优先调 `pwsh`（`PSFLASH := $(if $(shell where pwsh),pwsh,powershell)`），PS7 不吃这个坑。
- **别在 MSYS `sh` 里写 `>nul`**：`$(shell where pwsh >nul 2>nul ...)` 会在 `firmware/` 生成一个真名叫 `nul` 的文件（`rg`/`grep` 会因它报 `os error 1`）；删它必须用长路径前缀（`\\?\D:\DATE\STM32\G474RBT6\haixinber\firmware\nul`），`Remove-Item firmware\nul` 是打在空设备上、静默无效。
- **`draw_text()` 内部已经加了过渡位移 `s_dx`，自己画的图元（`OLED_DrawHLine`/`OLED_DrawBox`/`RFrame`）要自己加**（2026-10-02 改主页电池布局时踩的）：把已经含 `s_dx` 的坐标再喂给 `draw_text()`，在页面滑动过渡的那几帧里会**双倍位移**（静止帧完全正常，只有滑动时露馅）；正确写法是 `x0` 不含 `s_dx`，交给 `draw_text()` 去加。
- **改完固件再用 `.jlink` 脚本 poke RAM 之前，地址必须重新从符号表取**（2026-10-02 又踩一次：加了几个静态量后 `s_set` 从 `0x20001334` 挪到 `0x2000134c`，手写的老地址读出来是别的变量 `0x6E`，差点当成"套号坏了"）。`out/_wiz.ps1` 已经改成全部走 `nm` 查表；自己临时写的 `.jlink` 也要 `arm-none-eabi-nm firmware/build-app/haixin-app.elf` 现查。同名符号有多个（`s_state`、`s_base`、`s_set` 都有重名），要么看地址唯一性、要么 `objdump -d --disassemble=<函数名>` 看它 `ldr` 的字面量。
- **`draw_text()` 的 y 是 12px 字框的顶边，数字/汉字的墨迹从 y+4 才开始**（2026-10-02 排主页「电池图标 + 百分比」时踩的）：图标只占 y=5..13 时，`draw_text(..., y=6, ...)` 的墨迹落在 **10..17**，屏上明显"沉"下去 4 行；要让文字中心和图标同心，y 得**再减 3~4**（`app_ui.c` 的 `draw_battery_block()` 写的是 `BAT_ROW1_Y - 3`）。另外 `OLED_GetUTF8Width()` 比实际墨迹**宽约 1px**，"右对齐到 x=122" 之后最右像素仍可能顶到外框，留 2px 余量。量法：`python out/_lcd.py -i out/_lcd.bin --text`（或按 `page*128+x` 自己解包后取 numpy 非零包围盒），别靠目测。
