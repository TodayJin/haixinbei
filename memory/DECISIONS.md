# DECISIONS

> 已经定下来的选择，以及为什么。**只追加，最新在最上。**
> 定下来的选择要靠**商量**重开，不能悄悄绕过去。
>
> 条目形状：
>
> ```
> ## YYYY-MM-DD — <选择，一行>
> 选择：决定了什么
> 放弃：被否决的替代，以及为什么
> 因为：逼出这个决定的约束
> ```

## 2026-10-04 — 车上这颗 J-Link 的 CDC VCOM 上限是 230400，于是把固件 firmware/Core/Src/board.c:561 的 #define DL_BAUD 降成 230400u 重烧，桥用 --baud 230400；921600 留到以后换普通 USB-TTL 接 U12 时再用。
选择：车上这颗 J-Link 的 CDC VCOM 上限是 230400，于是把固件 firmware/Core/Src/board.c:561 的 #define DL_BAUD 降成 230400u 重烧，桥用 --baud 230400；921600 留到以后换普通 USB-TTL 接 U12 时再用。
放弃：继续在探针 VCOM 上凑 921600（每试一次≥460800 就把探针打进 PID_0101 引导态、只能物理拔插恢复）；或按 460800 折中（SET_LINE_CODING 直接超时，根本打不开）。
因为：实测 115200/230400 能开能读、460800 OSError 121、921600 PermissionError 31；用户 m22035 已授权降档，本轮又明确选了 A。

## 2026-10-04 — T_CMD 帧载荷统一发满 5 字节 = cmd(u8) + arg(i32 小端)，桥侧新增 protocol.cmd_payload()/pack_i32()/decode_cmd_arg() 并让 frame_cmd、state.Console.cmd、sim 全走它。
选择：T_CMD 帧载荷统一发满 5 字节 = cmd(u8) + arg(i32 小端)，桥侧新增 protocol.cmd_payload()/pack_i32()/decode_cmd_arg() 并让 frame_cmd、state.Console.cmd、sim 全走它。
放弃：保持老的 2 字节载荷（cmd + arg 低字节）只在文档里警告；或把固件读 arg 的条件从 len>=5 放宽成 len>=2（会废掉 32 位 arg、以后加不了负值/大值）。
因为：固件 host_link.c:776-782 只在 len>=5 时读 arg，老桥发 2 字节 ⇒ arg 恒 0 ⇒ CMD_SET_TELEM_HZ 被 arg<1→1 钳成 1Hz，真车实测从 75 帧/s 掉到 1~2 帧/s。

## 2026-10-04 — 固件遥测节拍改成「累加 period + 落后超过 2 个 period 才把 next_tx 重对齐到现在」，不再用 next_tx = now + period。
选择：固件遥测节拍改成「累加 period + 落后超过 2 个 period 才把 next_tx 重对齐到现在」，不再用 next_tx = now + period。
放弃：保留用 now 重算的写法（主循环一拍约 3.3ms 会把它量化到 13.3ms，100Hz 只出 75.5 帧/s）；或把遥测挪到定时器中断里发。
因为：真车实测要 100Hz 只出 75.5 帧/s，与 bridge/sim.py 里早就写对并注释过的写法同因；累加式不引入中断、改动只在 host_link_poll 一处。

## 2026-10-04 — 桥本地 state.Console 的 telem_hz 与固件同口径钳 max(1, min(200, arg))，/api/link 报钳过的值。
选择：桥本地 state.Console 的 telem_hz 与固件同口径钳 max(1, min(200, arg))，/api/link 报钳过的值。
放弃：直接存请求值（请求 300 时 /api/link 报 300、网页滑块显示 300，而车实际跑 200）。
因为：固件 host_link.c:800-803 本来就钳 1..200；上位机报一个车没在跑的速率会让用户拿它当参照调参。

## 2026-10-03 — J-Link 的 CDC VCOM 口上，工具自动跳过 >230400 的波特率档（`bridge/serial_safety.py`：`cap_bauds()` 扫档前过滤、`check_baud()` 给 `--serial` 用，`--force-baud` 可越过）
选择：J-Link 的 CDC VCOM 口上，工具自动跳过 >230400 的波特率档（`bridge/serial_safety.py`：`cap_bauds()` 扫档前过滤、`check_baud()` 给 `--serial` 用，`--force-baud` 可越过）
放弃：①只在文档/注释里警告 —— 下次还是会忘；②把 `DEFAULT_BAUDS` 改成从低往高扫 —— 破坏"先试用户真正想要的 921600"，还会撞 `test_default_ladder_is_the_fallback_order`；③对所有口一律拒绝高波特率 —— 会误伤普通 USB-TTL（CH340 能跑 921600）
因为：实测 ≥460800 会把探针打进 PID_0101 bootloader 态、只能物理拔插恢复；而 `probe` 默认梯子从 921600 往下扫，等于每次体检都拿探针去撞墙

## 2026-10-03 — 控制台编码加固放共享模块 `bridge/console_io.py`（stdout 被重定向→改 UTF-8；直接对终端→只 `errors=\"replace\"`），`probe/bench/main` 的 `main()` 开头都调一次
选择：控制台编码加固放共享模块 `bridge/console_io.py`（stdout 被重定向→改 UTF-8；直接对终端→只 `errors=\"replace\"`），`probe/bench/main` 的 `main()` 开头都调一次
放弃：①把 `⇒` 全换成 `=>` —— 只治一处、下次写中文输出还会踩；②不管三七二十一全改 UTF-8 —— 直接对着 GBK 终端会变成满屏乱码
因为：结论行里的 `⇒`（U+21D2）在 cp936/GBK 下编不出来，`UnicodeEncodeError` 崩在最后一行，等于前面量到的数字全白量；而用 `⇒` 的地方不止 probe（bench 的判定行也用）

## 2026-10-03 — 桥侧 SSE 攒批放在桥里做（`--sse-batch-ms` 默认 20ms，`0` 关），而不是只靠前端 50ms 合并
选择：桥侧 SSE 攒批放在桥里做（`--sse-batch-ms` 默认 20ms，`0` 关），而不是只靠前端 50ms 合并
放弃：只在前端合并（改动前的状态）—— 报文数与浏览器唤醒数一个没少，目标里写的「桥支持 SSE 合并」等于空的
因为：实测（`python -m bridge.bench --replay --hz 100,200 --seconds 4`）：100Hz 报文 402→135、200Hz 803→134，而帧率不变（100.4→100.2 / 199.3→199.8 帧/s）；代价是单帧最坏晚一个攒批窗口（报文到达间隔 p95 31.9ms / max 32.4ms）—— 这是有意的延迟交换

## 2026-10-03 — 热渲染挂到 `requestAnimationFrame`（`store.renderHot()`；Node 里没有 window 时同步兜底）
选择：热渲染挂到 `requestAnimationFrame`（`store.renderHot()`；Node 里没有 window 时同步兜底）
放弃：只用 50ms 定时器通知 —— 通知频率对了，但渲染不在绘制帧节拍上，标签页切到后台时定时器照样醒
因为：目标原文就是「网页按 rAF 合并渲染」；rAF 让同一帧内的多次通知合并成一次渲染，后台标签页 rAF 不触发 ⇒ 不白醒、不白算波形

## 2026-10-03 — 攒批开着时 `bridge.bench` 把「间隔」标成**报文级**并单列一行说明
选择：攒批开着时 `bridge.bench` 把「间隔」标成**报文级**并单列一行说明
放弃：照旧只印「间隔 p50 0.0ms」—— 看着像抖动没了，实际是同一条报文里的帧同刻到达
因为：攒批后的 p50 0.0ms 会误导人；20ms 攒批的真实代价恰好就是报文到达间隔 p95≈32ms，必须让人看懂这是报文口径

## 2026-10-03 — `bridge.bench` 的 CPU 数字只报**窗口内均值**（`100*(c1-c0)/dt`），不报分片取中位数。
选择：`bridge.bench` 的 CPU 数字只报**窗口内均值**（`100*(c1-c0)/dt`），不报分片取中位数。
放弃：按 0.5s 分片算瞬时 CPU 再取中位数（头一版就是这么写的）。
因为：实测同一组参数下分片中位数给出 0.0% / 6.1% / 9.1% 三个结果（采样被启动尖峰与 GC 带偏），写进报告就是噪声；改均值后 100Hz 回放两遍都是 **1.6%**。

## 2026-10-03 — 把"烧录后实测帧率/crc/延迟"做成**可发布、可单测**的 `tools/console/bridge/bench.py`（`python -m bridge.bench`），与 `probe.py` 分工：**probe 管串口那半步**（字节/合法帧/CRC/重同步/波特率），**bench 管桥→网页这一步**（实收帧率/间隔分位/事件率/KB/s/CPU/丢帧）。
选择：把"烧录后实测帧率/crc/延迟"做成**可发布、可单测**的 `tools/console/bridge/bench.py`（`python -m bridge.bench`），与 `probe.py` 分工：**probe 管串口那半步**（字节/合法帧/CRC/重同步/波特率），**bench 管桥→网页这一步**（实收帧率/间隔分位/事件率/KB/s/CPU/丢帧）。
放弃：继续用 `out/_bench_host.py`（gitignore 草稿、无测试、口径有两个 bug：rx 基线与计时窗口不齐导致喂帧率虚高 (1+seconds)/seconds 倍；CPU 采样无定义）。
因为：量测口径错一次，后面的"实测报告"就全是假的；纯函数（parse_sse/pct/summarize/format_report/serial_verdict）拆出来能被 pytest 钉住，`--replay` 又让没硬件时也能出承载报告。

## 2026-10-03 — 对车上这颗 J-Link（VID_1366/PID_0105，S/N 000602711039）的 VCOM 波特率上限按 **230400** 认定；要真正跑到 921600 只能换普通 USB-TTL 适配器接 U12 脚4(GND)/脚5(PC1)/脚6(PC0)，不再在探针 VCOM 上试 460800/921600。
选择：对车上这颗 J-Link（VID_1366/PID_0105，S/N 000602711039）的 VCOM 波特率上限按 **230400** 认定；要真正跑到 921600 只能换普通 USB-TTL 适配器接 U12 脚4(GND)/脚5(PC1)/脚6(PC0)，不再在探针 VCOM 上试 460800/921600。
放弃：继续在探针 VCOM 上试 921600（甚至 460800）来凑目标波特率。每试一次就把探针的 USB 栈打进 bootloader 态（枚举成 PID_0101，JLink.exe 报 "Replacing firmware: J-Link V9 compiled Oct 12 2012 BTL" + "Communication timed out"），且这块 CDC 面死掉后软重启救不回来。
因为：`python -m bridge.probe --port COM26 --bauds 115200,230400,460800,921600` 实测：115200 能开能读（1423 B/2s）、230400 能开能读（2791 B/2s）、**460800 打不开**（OSError(22,'信号灯超时时间已到',121)=ERROR_SEM_TIMEOUT，卡在 SET_LINE_CODING）、随后 921600 也打不开（PermissionError(13,31)=ERROR_GEN_FAILURE，是上一步把 CDC 搞死后的连锁）。

## 2026-10-03 — 探针从崩溃态恢复的唯一办法 = **物理拔 USB、等 ~10 秒再插**；插回来后若设备管理器里 `USB\VID_1366&PID_0105\000602711039` 报问题码 22（CM_PROB_DISABLED），就 `Enable-PnpDevice`（会回一句"常规故障"但设备随即可用，COM26 + 调试面都回来）。
选择：探针从崩溃态恢复的唯一办法 = **物理拔 USB、等 ~10 秒再插**；插回来后若设备管理器里 `USB\VID_1366&PID_0105\000602711039` 报问题码 22（CM_PROB_DISABLED），就 `Enable-PnpDevice`（会回一句"常规故障"但设备随即可用，COM26 + 调试面都回来）。
放弃：用软件重枚举去救：`pnputil /restart-device` 对"被禁用"的设备无效（返回成功但状态不变）；对本已崩成 PID_0101 的节点重启会把整个 VID_1366 从设备树里抹掉、更糟。
因为：本轮三次实操：Enable-PnpDevice 后 COM26 立刻回来；对 PID_0101 节点 restart 后设备彻底消失，只剩物理拔插。

## 2026-10-03 — LPUART1 提速到 921600：收 = DMA1_Ch2 循环 2048B（主循环按 CNDTR 取），发 = DMA1_Ch3 槽位队列 8×144B（非阻塞、队满丢帧并计数）
选择：LPUART1 提速到 921600：收 = DMA1_Ch2 循环 2048B（主循环按 CNDTR 取），发 = DMA1_Ch3 槽位队列 8×144B（非阻塞、队满丢帧并计数）
放弃：保留 HAL_UART_Transmit 阻塞发 + 512B 收环形缓冲（旧的 115200 方案）—— 它在 921600 下依然要等队列，且一帧帧等仍会卡控制环
因为：用户 m20705 要「通讯速率快一点、刷新快、下发快」；100Hz×64B 遥测 + 一次 4096B 任务读回（65 帧）在阻塞写法下会把主循环按住几百 ms（遥测断、OLED 不刷、可能触发 500ms 看门狗）

## 2026-10-03 — 网页去卡顿走「meta 快照 + 单通道快照」：新增 store.useMeta()（Omit<ConsoleSnapshot,'telem'|'ringFill'|'version'>，只在非热 emit 里重建）、ring.snapshotField(windowMs, fieldIndex)，热通知节流 100 → 50ms
选择：网页去卡顿走「meta 快照 + 单通道快照」：新增 store.useMeta()（Omit<ConsoleSnapshot,'telem'|'ringFill'|'version'>，只在非热 emit 里重建）、ring.snapshotField(windowMs, fieldIndex)，热通知节流 100 → 50ms
放弃：只把节流从 100ms 调大（更卡）或给每个组件单独做细粒度选择器（改动面大、收益不如这两处）
因为：所有组件都用 useStore() 订阅整份快照 ⇒ 每次热 emit 全部重渲染：Params 的 43 行表、Control 的日志/ACK 大列表本来跟 telem 无关却每 50ms 重建；ScopeChart/Trend 只画一个通道，snapshot() 却一次拷 21 个通道（每条泳道白拷 20 份）

## 2026-10-03 — 删掉桥里那条 {"n":"frame", "src":{type,seq,len}} SSE 事件（原 bridge/state.py:402-404 的 else 分支）
选择：删掉桥里那条 {"n":"frame", "src":{type,seq,len}} SSE 事件（原 bridge/state.py:402-404 的 else 分支）
放弃：留着不用（白占 SSE 带宽，还会加剧网页卡顿）
因为：全仓库 grep onFrame|'frame'|"frame" 只有这一处生产、没有任何消费方，等于每收到一帧非遥测帧就多推一条 JSON；帧数统计看每秒一次的 stats 就够

## 2026-10-03 — 上位机桥、网页设置页、协议文档的波特率默认值全部从 115200 改成 921600（bridge/link.py、bridge/main.py --baud、bridge/server.py、state.py 的 link_payload、Settings.tsx、Overview.tsx、api.ts）
选择：上位机桥、网页设置页、协议文档的波特率默认值全部从 115200 改成 921600（bridge/link.py、bridge/main.py --baud、bridge/server.py、state.py 的 link_payload、Settings.tsx、Overview.tsx、api.ts）
放弃：只改固件、桥继续默认 115200（刷完固件车就「掉线」，会误判成固件坏了）
因为：固件 DL_BAUD 是常量，桥上不一致就直接链路不通；用户 m20705 明确要用 921600

## 2026-10-03 — 界面上的英文缩写一律换成中文（收到/丢弃/校验错/实时推送/车上控制占用/校验和…），技术名词靠组件 `title` 悬停解释，Settings 末尾加一张「页面上的缩写、数字都是什么意思」对照卡
选择：界面上的英文缩写一律换成中文（收到/丢弃/校验错/实时推送/车上控制占用/校验和…），技术名词靠组件 `title` 悬停解释，Settings 末尾加一张「页面上的缩写、数字都是什么意思」对照卡
放弃：保持 rx / drop / crc / SSE / CRC16 / jsonl 这些裸英文原样
因为：用户 m20545 明确说看不懂并把 SSE 读成「FFE」、jsonl 读成「GSON」，要求不要用英文表示

## 2026-10-03 — 桥在动旧链路之前先校验串口名形态（`Bridge._serial_port_ok()`：Windows `^COM\d+$`、POSIX `^/dev/`），不合法直接 SystemExit 保住旧链路；前端 Settings 只在 `link.mode === 'serial'` 时把 `link.port` 同步进端口栏
选择：桥在动旧链路之前先校验串口名形态（`Bridge._serial_port_ok()`：Windows `^COM\d+$`、POSIX `^/dev/`），不合法直接 SystemExit 保住旧链路；前端 Settings 只在 `link.mode === 'serial'` 时把 `link.port` 同步进端口栏
放弃：只在 runner.start() 之后回滚 —— 更复杂且 SerialLink 打不开是把错误吞进 note、runner 自己不抛异常
因为：回放残留的录制文件名被当成串口名发过来时 SerialLink 静默失败、而 switch() 已把好的旧链路 stop() 掉，两边都没了（用户报的「突然操控不了车、遥测不动」）

## 2026-10-03 — 操控页速度改用固件速度刻度 0..7200（不是 mm/s），并加「跳过静摩擦死区」开关（默认开，滑块从 1000 起步，存 settings.driveSkipDead）
选择：操控页速度改用固件速度刻度 0..7200（不是 mm/s），并加「跳过静摩擦死区」开关（默认开，滑块从 1000 起步，存 settings.driveSkipDead）
放弃：保留 ±3000 上限（就是 bug 本身）；或只把上限放到 7200 而不处理死区（滑块 0..1000 那一段车根本不动，用户明确要求「把死区那部分去掉」）
因为：固件 SpeedControl_SetTarget()/Motion_SetSpeed()/GyroControl_SetBaseSpeed() 吃的就是 PWM 目标刻度；旧前端把它当 mm/s 夹在 ±3000 只有 42% 满量程 —— 用户 m20267 报「满速太低」的根因

## 2026-10-03 — 倒车时在 stepDrive 里把角速度取反（const yawRate = intent.base < 0 ? -intent.turnRate : intent.turnRate）
选择：倒车时在 stepDrive 里把角速度取反（const yawRate = intent.base < 0 ? -intent.turnRate : intent.turnRate）
放弃：改固件 gyro_control.c 的符号（会把前进/循迹/角度环全带翻）；或在 keys.ts 的按键映射里翻（那里拿不到「当前是否倒车」之外的信息也不是问题点，但语义上方向翻号属于「意图→航向」这一层，和积分放一起才自洽）
因为：turn 是目标航向偏移（正 = 左），而倒车时行进方向 = 航向 + 180°，不取反按 S+D 会让车尾往左摆 —— 用户 m20267 报的方向反了

## 2026-10-03 — 真车链路走 **J-Link 的 CDC 串口 COM26**（`JLink CDC UART Port`，VID:PID 1366:0105）——用户把 U12 的那两根串口线「反过来」接之后就通了
选择：真车链路走 **J-Link 的 CDC 串口 COM26**（`JLink CDC UART Port`，VID:PID 1366:0105）——用户把 U12 的那两根串口线「反过来」接之后就通了
放弃：放弃「J-Link 20 脚排线里没有 UART 线、COM26 与 U12 无电气通路」这个早先结论（memory/SESSIONS.md:147/155/158 那几条）：线其实在，只是 TX/RX 接反了，先前按「探针 TXD → 板侧网名 LPUART1_RX(U12.5)」这种按网名对号入座的方式接就是反的，正确的是探针 TX → U12 脚 6（PC0=车收）
因为：这块板原理图两处网名与芯片功能相反（`LPUART1_TX`=PC0、`LPUART1_RX`=PC1；网表 pcb/Netlist_Schematic1_2026-09-29.tel:430-431），按网名接线必然接反；反过来后 PC 侧静听 0.5s 收到 1684 字节遥测、发 3 帧 HELLO_REQ 收到车回的 type=0x40 hello 帧

## 2026-10-03 — 烧录失败时先判定「探针还在不在 USB 上」，而不是改 tools/flash.ps1：JLink.exe 卡在 `JLINK_OpenEx(...)` 就是 USB/探针层的问题，脚本此前多轮都烧成功过。
选择：烧录失败时先判定「探针还在不在 USB 上」，而不是改 tools/flash.ps1：JLink.exe 卡在 `JLINK_OpenEx(...)` 就是 USB/探针层的问题，脚本此前多轮都烧成功过。
放弃：改 flash.ps1（加超时/换 CubeProgrammer）、或反复重跑脚本 —— 都不解决探针本身掉线，只会重复卡死。
因为：本次卡死根因是一个 2026-10-03 09:07 启动、跑了 11.5 小时满 CPU 的孤儿 JLink 会话（`-CommanderScript C:\Users\Today\AppData\Local\Temp\_jl_banner.jlink`，RTT banner 检查）长期占着探针，杀掉它之后探针 USB 直接不再枚举。

## 2026-10-03 — 探针掉线后尝试软件恢复：`pnputil /restart-device` 重启该 USB 设备、以及它挂载的 Realtek 集线器（`USB\VID_0BDA&PID_5411\7&22af457e&0&4`）。
选择：探针掉线后尝试软件恢复：`pnputil /restart-device` 重启该 USB 设备、以及它挂载的 Realtek 集线器（`USB\VID_0BDA&PID_5411\7&22af457e&0&4`）。
放弃：直接让用户物理拔插 —— 软件重启能证实并部分复位 USB 栈，但探针仍未重新枚举，最终仍需物理拔插。
因为：`Restart-PnpDevice` 在这台机器上不存在；pnputil 是唯一可用的软件重启手段，能把「JLINK_OpenEx 永久卡死」变成 37 秒后干净返回 `Cannot connect to J-Link.`。

## 2026-10-03 — 删掉固件屏上「任务A / 任务B」两个残留自定义页与「任务」二级菜单（k_task_items），首页「任务」直进 PAGE_CUSTOM_TASK，页上显示解释器真状态（TaskRun_IsRunning/Id/Block，空闲显示车上 N 个）；自定义页下标顺移（CUSTOM_COUNT 9→8）、PAGE_COUNT 17→15（舵机页 15）；删掉 app_live_t.task_state 字段
选择：删掉固件屏上「任务A / 任务B」两个残留自定义页与「任务」二级菜单（k_task_items），首页「任务」直进 PAGE_CUSTOM_TASK，页上显示解释器真状态（TaskRun_IsRunning/Id/Block，空闲显示车上 N 个）；自定义页下标顺移（CUSTOM_COUNT 9→8）、PAGE_COUNT 17→15（舵机页 15）；删掉 app_live_t.task_state 字段
放弃：保留那两页（用户已把它当成「车上有两条任务」的假象）；也否决让中键启动任务——误按一下车就按流程跑起来太危险，启动只走上位机 0x67 RUN 或开机自启
因为：用户 m19865 直接问「为什么现在固件上还有AB两个任务，我不是没有上传吗」；g_live.task_state 全工程只有那两页读写（写 app_ui.c:2416 / 读 :1029），内置流程 renwu.c 已删、CMD_START_A/B/C 已回 ST_UNSUPPORTED，屏上那个「待机」是纯残留；车上「有几个任务」的唯一真值是 Flash 页 59 的目录

## 2026-10-03 — 单任务上限 2048 → 4096（`TASK_RUN_MAX_PROG` / `TASK_STORE_CAP` 两处 + 主机断言 + 桥 `TASK_SLOT_BYTES` + 假后端 + 文档）
选择：单任务上限 2048 → 4096（`TASK_RUN_MAX_PROG` / `TASK_STORE_CAP` 两处 + 主机断言 + 桥 `TASK_SLOT_BYTES` + 假后端 + 文档）
放弃：保持 2048，靠砍状态/瘦身把流程压进 2048 —— 会丢掉旧流程里的动作段，不忠实
因为：忠实转换旧全程流程要 2127 B，2048 装不下；镜像字节格式不变（v1、10 B 一块），只是长度上限变大，代价是 RAM +4 KB（bss 26944 → 31040）

## 2026-10-03 — 旧长流程交付成仓库文件 `tools/console/web/src/lib/tasks/renwu-lap.json`，用户页面 `#/tasks` 点「导入」进编辑器（我把任务烧进仿真车槽 0，flags=1，可用「读目录 → 读回」）
选择：旧长流程交付成仓库文件 `tools/console/web/src/lib/tasks/renwu-lap.json`，用户页面 `#/tasks` 点「导入」进编辑器（我把任务烧进仿真车槽 0，flags=1，可用「读目录 → 读回」）
放弃：让页面默认就带这条 12 障碍流程 —— 会把一条比赛专用流程强加给每个打开任务页的人，也会污染 SSR 快照
因为：任务列表存在浏览器 localStorage 里，服务端没法替用户导入；页面本来就有「导入」这条既有路径，而且它可被 vitest 钉住（`renwu-lap.test.ts`）

## 2026-10-03 — 把「两轮直给」（`MODE.WHEELS`）放回模式下拉（停车/角度环/循迹/两轮直给四项）
选择：把「两轮直给」（`MODE.WHEELS`）放回模式下拉（停车/角度环/循迹/两轮直给四项）
放弃：继续只留 停车/角度环/循迹三项（2026-10-02 那次改的）—— 旧流程的倒车段只能退化成开环两轮直给之外的别的东西，实际上表达不了
因为：它是唯一能左右分开给速、也是唯一能倒车的模式；旧流程里 `Load(900,950)` 这类左右不对称段和 `Load(-900,-900)` 倒车段只有它能表达（SET_SPEED 的 p0 是 u16，角度环给不出负速）

## 2026-10-03 — 转换时「丢线」判据用 `WAIT_COND(cond=2, p1=0)`（阈值取固件默认 60），不用 `p1=1`
选择：转换时「丢线」判据用 `WAIT_COND(cond=2, p1=0)`（阈值取固件默认 60），不用 `p1=1`
放弃：`thr=1`（严格 8 路都 <1）—— 与旧代码字面一致，但对反光/边缘更脆
因为：旧代码的判据是「8 路灰度全 0」，而灰度是 0..255、数字口只回落 0/255，取 60 时「8 路全部 <60」与旧判据等价但抗噪（线在边缘时不会漏触发）

## 2026-10-03 — 旧 `GyroControl_TurnLeftFixed()` / `TurnRightFixed()` 一律按**实际效果**映射（即 LeftFixed 转成 −90°、RightFixed 转成 +90°），不按名字映射
选择：旧 `GyroControl_TurnLeftFixed()` / `TurnRightFixed()` 一律按**实际效果**映射（即 LeftFixed 转成 −90°、RightFixed 转成 +90°），不按名字映射
放弃：按函数名映射 —— 看着对，实际每个固定转向都反 180°
因为：`firmware/App/Svc/gyro_control.c:829-840` 的注释与实现相反（LeftFixed 真做的是 `TurnRight(90)`），按名字转会把每个固定转向都转反

## 2026-10-03 — 「跳到状态」块的目标下拉只列真实状态（0..N-1），不再提供「继续往下跑」（0xFFFF）；这个选项只保留在「等条件」的「超时后」
选择：「跳到状态」块的目标下拉只列真实状态（0..N-1），不再提供「继续往下跑」（0xFFFF）；这个选项只保留在「等条件」的「超时后」
放弃：两处都用同一个 state 渲染器硬编码一条 0xFFFF 选项（原实现）。否决理由：用户 m19327 看到跳转块里的「继续往下跑」直接问「这是啥玩意、要这个干嘛」；固件对 GOTO 的 0xFFFF 语义是「不跳、跳过这一块」（task_run.c:214）——加一个跳转块却什么都不做没意义；而 WAIT_COND.p3 的 0xFFFF = 超时也不改道，是真实语义。
因为：0xFFFF 在两处含义不同：GOTO.p0 是空操作（占位），WAIT_COND.p3 是默认值。用 FieldSpec.allowNoState 区分，并加 stateChoices(cur,stateCount,allowContinue) 纯函数生成选项，老文件的 0xFFFF / 越界值临时显示为「（旧）继续往下跑」/「状态 N（不存在）」。

## 2026-10-03 — removeState 修引用时只修真正指向某个状态的号，0xFFFF 与越界占位值原样留着
选择：removeState 修引用时只修真正指向某个状态的号，0xFFFF 与越界占位值原样留着
放弃：对 GOTO/WAIT_COND 一律套 fix(ref)（原实现）。否决理由：fix(0xFFFF) 会得到 0xFFFE，越修越偏、把一个「空操作占位」改成一个看起来像真状态的号。
因为：合法目标号范围是 0..N-1（删之前共 N 个状态），超出这个范围的一律是占位/垃圾值，改动它们只会让校验报错更难懂。

## 2026-10-03 — 上位机「切换模式」下拉只暴露 3 项：停车(IDLE 0) / 角度环(GYRO 1) / 循迹(TRACE_GYRO 3)；「循迹」= 三层串级
选择：上位机「切换模式」下拉只暴露 3 项：停车(IDLE 0) / 角度环(GYRO 1) / 循迹(TRACE_GYRO 3)；「循迹」= 三层串级
放弃：保留 5 项（含 MOTION_TRACE(2) 的「循迹环+速度环」两级与「两轮直给」）。否决理由：用户 m12856 已定案「正常循迹 = 三级 PID 串级」、「循迹环+速度环」那条是旧板遗留、不伦不类；用户 m19122 又抱怨下拉选项太多。值 2 与两轮直给降级成 MODE_LEGACY，只在打开老文件时以「（旧）」出现（modeLabel/modeChoices），固件与镜像格式一律不动、老镜像照旧能跑。
因为：固件 motion.c:149-161 的语义是 2=两级 motor_tracing_pid_speed()、3=三层 motor_tracing_cascade()，而 taskedit.ts 旧 MODE_OPTIONS 把两者的标签写反了（值 2 写「三环循迹」、值 3 写「三级串级」），车上一贯走的是 host_ext.c:136 → MOTION_TRACE_GYRO(3)。

## 2026-10-03 — 任务页状态机改成一屏只看一个状态：主页排标签条第一位，其余状态用标签切换，卡片区只渲染当前选中的那个 StateCard
选择：任务页状态机改成一屏只看一个状态：主页排标签条第一位，其余状态用标签切换，卡片区只渲染当前选中的那个 StateCard
放弃：把所有状态竖排成一摞卡片全渲染（旧样子，状态一多就滚不到头）。否决理由：用户 m19122「这个界面跟我想的不一样」并明确选择「一屏一个状态：任务主页在最上面，其他状态分栏/切页」。
因为：任务镜像头第 4 字节 main_state 就是「主页」（task_run.c:294 seq_begin(s_prog[4])），把它提到最上面符合执行顺序直觉；标签条还能当拖放落点。

## 2026-10-03 — 遥测 TELEM[27](state_id) 与 TELEM2[51](task_id) 都用「解释器在跑」TaskRun_IsRunning() 作判据，没在跑双双 0xFF
选择：遥测 TELEM[27](state_id) 与 TELEM2[51](task_id) 都用「解释器在跑」TaskRun_IsRunning() 作判据，没在跑双双 0xFF
放弃：曾用 TaskRun_Loaded()（载入即算），需要在上位机里靠 0x69 状态帧的 running 字节自行消歧，仿真车没有 loaded 概念
因为：任务跑完 END 或被 STOP 后还报旧状态号/任务号会误导上位机；而且仿真车只认 running，改固件两行就能让桥/仿真/固件同一口径

## 2026-10-03 — 仿真车与假后端复刻固件的擦写守卫：解释器在跑时 T_TASK_COMMIT / DEL / RUN 回 ST_BUSY，分片 PUT 不受限
选择：仿真车与假后端复刻固件的擦写守卫：解释器在跑时 T_TASK_COMMIT / DEL / RUN 回 ST_BUSY，分片 PUT 不受限
放弃：仿真车原先无守卫（D2 验收 FAIL 才暴露），假后端原先还会顺手把 running 置 false
因为：擦一页会把 CPU 按住几十毫秒、正在跑的控制环失控；仿真车若「悄悄帮你停掉再写/删」，上位机练熟的操作上车就被拒，安全规则只在真车上成立等于没测

## 2026-10-03 — 删掉内置任务流程 firmware/App/Svc/renwu.c + renwu.h（32036+284 B）与解析它的 tools/renwu_states.py
选择：删掉内置任务流程 firmware/App/Svc/renwu.c + renwu.h（32036+284 B）与解析它的 tools/renwu_states.py
放弃：保留当参考（原注释写着「保留不删当参考」）——git 历史随时可取，且留着会持续干扰重命名/静态检查
因为：任务改由 Flash 程序 + 解释器驱动后它是死代码；-ffunction-sections --gc-sections 早就把它丢出镜像（删了尺寸几乎不变，text 114216→114232 的 +16B 来自 ST_UNSUPPORTED 分支文本）

## 2026-10-03 — HP_TASK_SPEED(0x0304, 默认 6480) 保留，改作解释器启动时的基准速度（TaskRun_Start() 里 Motion_SetSpeed(host_param_get(...))）
选择：HP_TASK_SPEED(0x0304, 默认 6480) 保留，改作解释器启动时的基准速度（TaskRun_Start() 里 Motion_SetSpeed(host_param_get(...))）
放弃：把参数一并删除（会让上位机参数表少一项、老存档失去意义）
因为：参数表改一处就能整体缩放比赛流程的便利值得保留；原先只有 renwu.c 用它，删掉 renwu.c 后不接上就变成无人使用的死参数

## 2026-10-03 — COMMIT 帧（T_TASK_COMMIT 0x63）载荷冻结为 u16 id + u16 len + u16 crc + u8 flags，可选再跟 u8 name_len + name（len==6 只认前三者、len==7 只带 flags、len>=8 带名字）
选择：COMMIT 帧（T_TASK_COMMIT 0x63）载荷冻结为 u16 id + u16 len + u16 crc + u8 flags，可选再跟 u8 name_len + name（len==6 只认前三者、len==7 只带 flags、len>=8 带名字）
放弃：否决「flags 放在名字之后」——任务帧是本次新加的，没有已部署的旧固件要兼容，干净布局更可读；旧布局（name_len 在 p[6]）在新固件里会被当 flags 读、随后因 name_len 越界回 ST_BAD_PARAM，不会静默写错
因为：TASK_STORE_FLAG_AUTO（开机自启）早就在固件里，但 COMMIT 没有 flags 字节 ⇒ 上位机永远打不开它；加这一字节要固件 host_link.c + bridge/{protocol,state,server}.py + sim.py + taskapi.ts + Tasks.tsx + mock 一起改

## 2026-10-03 — TASK_PUT 的单帧数据上限按协议取 124 B（= MAX_PAYLOAD(128) − 4），页面按 120 B 分片
选择：TASK_PUT 的单帧数据上限按协议取 124 B（= MAX_PAYLOAD(128) − 4），页面按 120 B 分片
放弃：文档原来写的「hex ≤ 240 字符 = 120 B」当成协议上限——那是页面分片尺寸，桥侧不拦截 120
因为：bridge/protocol.py 的 TASK_PUT_MAX = MAX_PAYLOAD − 4 = 124；把页面分片尺寸写成协议上限会让别的前端照抄成硬限制

## 2026-10-03 — 任务帧的目录回读（LIST_R）用「一帧空载荷」收尾、程序读回（DATA）用「一帧空数据」收尾，且 GET/LIST 成功都不回 ACK
选择：任务帧的目录回读（LIST_R）用「一帧空载荷」收尾、程序读回（DATA）用「一帧空数据」收尾，且 GET/LIST 成功都不回 ACK
放弃：另加一个 has_more 标志位 —— 要改帧语法且老桥不认
因为：§5 的帧语法允许 n=0，但文档没有「还有更多」标志位；桥必须要一个无歧义的结束标志，而 ACK 会被误当成数据帧到达

## 2026-10-03 — 任务帧回包每拍最多一帧、DATA 每帧 64B
选择：任务帧回包每拍最多一帧、DATA 每帧 64B
放弃：收到请求就一次性把整份镜像发完
因为：board_dl_send() 是同步阻塞的（64B 数据帧 ≈6.5ms @115200，与一帧 T_TELEM2 同量级）；若在 handle_frame 里连发，一次 GET 2008B ≈32 帧 ≈200ms 会把主循环按住，遥测断流、OLED 不刷，还可能被 500ms 驾驶看门狗误判成掉线

## 2026-10-03 — COMMIT 的 flags 只能是 0（TASK_STORE_FLAG_AUTO 目前没有下发通路）
选择：COMMIT 的 flags 只能是 0（TASK_STORE_FLAG_AUTO 目前没有下发通路）
放弃：在桥里偷偷默认置 AUTO —— 那会让每次烧写都变成自启，无法取消
因为：docs/API.md §3.6 的 commit 只带 {id,len,crc,name}，前端 taskapi.ts 也没发 flags；要做开机自启必须两边一起给 COMMIT 加一个 flags 字节

## 2026-10-03 — 上位机↔真固件对拍管道（tools/console/tests/test_firmware_pipe.py）链**真** task_store.c/task_prog.c/task_run.c，不用 host_taskstub.c 替身
选择：上位机↔真固件对拍管道（tools/console/tests/test_firmware_pipe.py）链**真** task_store.c/task_prog.c/task_run.c，不用 host_taskstub.c 替身
放弃：链 firmware/test/stub/host_taskstub.c（那是 t_hostlink 的替身，只验协议不验 Flash）
因为：host_link.c 的任务帧要调这些模块，缺了整条管道链接失败；用真的还能顺带让任务用例走真 Flash 语义（擦页/双字编程/目录页重排都过 host_hal 的假 Flash）

## 2026-10-03 — firmware/STM32G474RBTx_FLASH.ld 的 FLASH 保持 118K（0x08000000..0x0801D7FF）
选择：firmware/STM32G474RBTx_FLASH.ld 的 FLASH 保持 118K（0x08000000..0x0801D7FF）
放弃：126K（原值）/120K —— 都覆盖到任务存储页
因为：页 59（0x0801D800）是任务目录页、页 60..62 是程序区、页 63 是 nvram，全归运行期自己擦写；改成 120K/126K 会让链接器把代码或常量静默摆进这些页，而 TaskStore_Commit 每次写任务都擦它们

## 2026-10-03 — 任务存储定为「页 59 目录 + 页 60..62 程序区」：目录页 = 8B 头（magic 0x5453/ver 2/count/seq）+ 8×40B 记录（id/off/len/crc16/flags/name_len/name[30]），程序区 6144B，最多 8 条、单条 ≤2048B（= 解释器 RAM 上限）；`.ld` FLASH 收到 118K，页 63 仍是 nvram。
选择：任务存储定为「页 59 目录 + 页 60..62 程序区」：目录页 = 8B 头（magic 0x5453/ver 2/count/seq）+ 8×40B 记录（id/off/len/crc16/flags/name_len/name[30]），程序区 6144B，最多 8 条、单条 ≤2048B（= 解释器 RAM 上限）；`.ld` FLASH 收到 118K，页 63 仍是 nvram。
放弃：本会话早先写过的「一槽一页 × 3 槽（页 60..62）、40B 记录头放页首」已作废并重写 —— 单条上限只有 2008B、槽间要各自擦页；另外否决把 `.ld` 改回 120K/126K 腾地方。
因为：并发写者已把 host_link 的 T_TASK_* 帧与上位机契约写成按目录页寻址，而且可视化编辑器要 8 个任务；`.ld` 保持 118K 是因为 TaskStore_Commit 每次写任务都要擦页 59..62，允许链接器把代码摆进去等于静默擦掉正在跑的代码/`.data` 初值。

## 2026-10-03 — TaskStore_Commit 的落盘顺序 = 校验（槽号/长度/暂存覆盖 → 运动中拒写 → CRC16 → TaskProg_Validate → 重排布局）→ 擦目录页与程序区 3 页 → 逐页复验页首字回到 0xFFFFFFFF → 编程 → memcmp 回读 → 才更新缓存。
选择：TaskStore_Commit 的落盘顺序 = 校验（槽号/长度/暂存覆盖 → 运动中拒写 → CRC16 → TaskProg_Validate → 重排布局）→ 擦目录页与程序区 3 页 → 逐页复验页首字回到 0xFFFFFFFF → 编程 → memcmp 回读 → 才更新缓存。
放弃：先写数据再写目录、或用 HAL_FLASHEx_Erase() 擦页。
因为：本板 HAL_FLASHEx_Erase() 是静默失败的（nvram.c 早就因此手写 FLASH->CR 序列），而擦完不复验、写完不回读的话，掉电/失败会留下一个「CRC 对但内容半新半旧」的槽。

## 2026-10-03 — main_app 主循环里旧 renwu() 的调用整段摘掉，只保留「清 renwu_flag + 需要时 renwu_stop()」；renwu.c 保留不删，文件头加废弃注释。
选择：main_app 主循环里旧 renwu() 的调用整段摘掉，只保留「清 renwu_flag + 需要时 renwu_stop()」；renwu.c 保留不删，文件头加废弃注释。
放弃：保留「renwu_flag == 0U 时才 TaskRun_Tick()」的互斥写法；也否决了直接把 renwu.c 删掉。
因为：host_link 把 renwu_flag != 0 当成「车在动」的判据（COMMIT/DEL 回 ST_BUSY），只删调用不清标志会让一个老式 CMD_START_A 把任务存储区永久锁成 BUSY；renwu.c 里的 RW(v) 刻度换算与 11 个障碍的旧板时序仍然有用。

## 2026-10-03 — 坏记录的测试必须包含「镜像完好、但目录里那条记录自己的 crc16 被写坏」这一形态（改 dir_ent_w(1U)[6]），不能只靠「翻镜像一位」。
选择：坏记录的测试必须包含「镜像完好、但目录里那条记录自己的 crc16 被写坏」这一形态（改 dir_ent_w(1U)[6]），不能只靠「翻镜像一位」。
放弃：只用「镜像翻位」用例覆盖扫描期校验。
因为：记录头 crc16 与镜像自带 crc16 盖的是同一段 [12,len)，镜像一坏两层同时红 ⇒ 把「扫描时镜像 CRC 校验」变异成恒真时测试仍然全绿（实测存活）；而记录头字段不在镜像 CRC 覆盖内，只有这条用例能单独咬住它。同理镜像头（magic/ver/n_states/main_state）不在任何 CRC 覆盖内，只有 TaskProg_Validate 挡得住。

## 2026-10-03 — 任务程序格式以父代理的 A 格式（web/src/lib/task.ts：magic 0x4B54、ver 1、12B 头 + 8B 状态表 + 10B 块、CRC16-CCITT、帧由固件 task_prog.c 解析）为唯一标准
选择：任务程序格式以父代理的 A 格式（web/src/lib/task.ts：magic 0x4B54、ver 1、12B 头 + 8B 状态表 + 10B 块、CRC16-CCITT、帧由固件 task_prog.c 解析）为唯一标准
放弃：我先前自创的 B 格式（tools/console/bridge/tasks.py + tasksim.py + docs/任务程序格式.md，magic 0x31545848）：整包移到 out/_archive_taskfmt_b/ 归档不删
因为：父代理已经把它接进 firmware/Core/Src/main_app.c 主循环并有 t_task.c 130 项测试与文档，同时存在两套格式必然分叉

## 2026-10-03 — 上位机的任务页自己做编译（encodeTask）并把 CRC 一起发给桥，烧写后立刻回读逐字节比对
选择：上位机的任务页自己做编译（encodeTask）并把 CRC 一起发给桥，烧写后立刻回读逐字节比对
放弃：把二进制编码只放在桥/Python 端、前端只发 JSON
因为：烧半个程序让车上跑错轨迹这类事故只能在校验+回读环节拦住；桥端只做透传不做第二种编码实现

## 2026-10-03 — 桥的 /api/tasks* 端点未实现期间，由 web/mock/server.mjs 按同一份契约实现（内存假 Flash），页面即可整条演示
选择：桥的 /api/tasks* 端点未实现期间，由 web/mock/server.mjs 按同一份契约实现（内存假 Flash），页面即可整条演示
放弃：等真桥实现后再做界面
因为：用户要看到效果，而真桥端点与固件帧要等父代理那侧；契约冻结在 tools/console/docs/API.md §3.6，假后端与真桥同契约

## 2026-10-03 — 任务程序的跨语言金样本只走生成链：tools/console/tests/c/taskgen.c（链真 firmware/App/Svc/task_prog.c）→ tools/console/tests/fixtures/task.golden.json → tools/console/web/src/lib/task.golden.ts → task.golden.test.ts，新鲜度由 tools/console/tests/test_task_bytes.py 每次重编 C 比对
选择：任务程序的跨语言金样本只走生成链：tools/console/tests/c/taskgen.c（链真 firmware/App/Svc/task_prog.c）→ tools/console/tests/fixtures/task.golden.json → tools/console/web/src/lib/task.golden.ts → task.golden.test.ts，新鲜度由 tools/console/tests/test_task_bytes.py 每次重编 C 比对
放弃：两侧各手抄一份 278 字符十六进制字面量（t_task.c 的 kGoldenHex + task.test.ts 的 GOLDEN_HEX）—— 已删掉
因为：手抄表是确定性漂移源：两边同时错就会一起绿，没人会发现（旧参数表就自己撒谎过一次）

## 2026-10-03 — 任务程序解释器全非阻塞（TaskRun_Tick(now_ms) 每 5ms 推进，单拍最多 32 块），且「末尾必须是 跳到状态/结束/带超时的等待条件」这条规则只约束 body 序列，enter 序列允许空跑完
选择：任务程序解释器全非阻塞（TaskRun_Tick(now_ms) 每 5ms 推进，单拍最多 32 块），且「末尾必须是 跳到状态/结束/带超时的等待条件」这条规则只约束 body 序列，enter 序列允许空跑完
放弃：照搬旧 App/Svc/renwu.c 的流程（210 处 HAL_Delay + 36 处阻塞 Servo_Move，跑起来会卡死遥测与看门狗）
因为：任务要和 5ms 控制环、链路看门狗共存；第一版把末尾规则套在 enter 上，t_task 挂了 42 处

## 2026-10-03 — 任务程序存储区定页 60..62（0x0801E800..0x0801F7FF，6KB），页 63 继续留给 nvram
选择：任务程序存储区定页 60..62（0x0801E800..0x0801F7FF，6KB），页 63 继续留给 nvram
放弃：挤链接脚本剩的 ~13.6KB 或整片擦 Flash
因为：tools/flash.ps1 默认按镜像范围擦、不整片擦，所以页 60..62 掉电保留且升级固件不会清掉已存的任务（实现还没做）

## 2026-10-03 — IMU 页 3D 车模型用纯手写 CSS 3D（preserve-3d 叠长方体），不引 three.js、不用 Canvas 等轴测、不加模型文件
选择：IMU 页 3D 车模型用纯手写 CSS 3D（preserve-3d 叠长方体），不引 three.js、不用 Canvas 等轴测、不加模型文件
放弃：three.js——多一个运行时依赖且仍然截图不了；Canvas 自己画等轴测——要手写投影，还丢了 CSS 3D 的透视/背面剔除
因为：本沙箱跑不了浏览器（Chrome/Edge 无头都无输出），3D 是唯一没法像素自证的东西，所以把能自证的部分最大化：符号/相机/罗盘全放纯函数 lib/attitude3d.ts 用 vitest 钉死，DOM 层（components/Car3D.tsx）只拼盒子

## 2026-10-03 — 上位机 3D 模型和读数都用遥测原始帧，不套固件给 OLED 用的 M1..M8 轴序交换
选择：上位机 3D 模型和读数都用遥测原始帧，不套固件给 OLED 用的 M1..M8 轴序交换
放弃：直接复用 app_ui.c:681 的 gyro_angles()（*roll=-pitch_x10、*pitch=-roll_x10）——把 OLED 的经验值当成物理约定
因为：那八档（app_ui.c s_gyro_axis_mode，默认 M3）是用户 m02505 对着 OLED 视角经验挑的，跟着它走会出现"数字说横滚、车却在点头"；真车方向不对只改 lib/attitude3d.ts 里一行符号

## 2026-10-03 — Sparkline 的高度改由 className 决定（viewBox 的 height 只是坐标刻度），并新增 zeroScale 让纵轴以 0 为中心
选择：Sparkline 的高度改由 className 决定（viewBox 的 height 只是坐标刻度），并新增 zeroScale 让纵轴以 0 为中心
放弃：继续用 height 数字当 CSS 高度（改不了渲染尺寸），以及给波形页的 uPlot 换一套缩放——波形页不动
因为：姿态角用自动缩放时，0 附近几度的抖动会被拉满整屏，看着像车在剧烈晃；高度靠类名给才能让三张波形卡等高

## 2026-10-03 — IMU 页的「朝向控制」直接用已有 `/api/drive {mode:"base_turn", base:0, turn:度}`，不新增固件命令、不实现 CMD_CALIB_GYRO
选择：IMU 页的「朝向控制」直接用已有 `/api/drive {mode:"base_turn", base:0, turn:度}`，不新增固件命令、不实现 CMD_CALIB_GYRO
放弃：实现 CMD_CALIB_GYRO(0x05)/新增陀螺命令：固件那三个 case 明确回 ST_NOT_READY，桥的 CMD_NAMES 里也没有 gyro_cal/gyro_zero/gyro_turn（只有 mock 认），做了也是假成功
因为：固件 `firmware/App/Svc/host_ext.c` HostExt_DriveSet 的 mode 2 已经做完这套语义：`s_base_yaw = Yaw; Motion_SetSpeed(a); Motion_SetMode(MOTION_GYRO); GyroControl_SetTargetAngle(NormalizeAngle(s_base_yaw + b))`，base=0 就是原地按角度环转到并保持该朝向；而 yaw 是 JY61P 模块自己融合的、MCU 只读，MCU 侧能动的只有 Gyro_Z 零偏

## 2026-10-03 — 操控页的转向语义从「转向 mm/s 直给 turn」改成「转向角速度 °/s，前端每 100ms 积分成目标航向偏移（最多 ±540°）再下发」
选择：操控页的转向语义从「转向 mm/s 直给 turn」改成「转向角速度 °/s，前端每 100ms 积分成目标航向偏移（最多 ±540°）再下发」
放弃：保留 mm/s 转向（语义错）或把 turn 硬钳到 ±30°（丢功能）
因为：固件/仿真的 `b` 单位是**度**（相对进入模式时的航向），直接给一个 mm/s 数值等于命令一个固定航向（按一下 A 变成转到 500°≈140°），不是柔和转向

## 2026-10-03 — 键盘 A（左转）发**正** turn；`keys.ts` 用 `turnRateOf(left, right)` → 左 = +
选择：键盘 A（左转）发**正** turn；`keys.ts` 用 `turnRateOf(left, right)` → 左 = +
放弃：沿用旧符号并在桥里翻一下（会把真车/仿真两条路的口径搞岔）
因为：固件 `gyro_control.c` 注释与 `GyroControl_TurnLeft` 都是「偏航角增大 = 向左」，仿真 `sim.py` 也是 b>0 使 yaw 增大；原先 `(right - left)` 把左转发成了负值（实际右转）

## 2026-10-03 — 仿真桥 `sim.py` 补上角度环闭环（P 律 + 限幅 120°/s + 死区 0.6°），且基准航向只在**进入** base 模式时锁存
选择：仿真桥 `sim.py` 补上角度环闭环（P 律 + 限幅 120°/s + 死区 0.6°），且基准航向只在**进入** base 模式时锁存
放弃：把角度环留到真车再验（用户没有硬件可随时试，且仿真与固件行为会长期不一致）
因为：原先 mode 2 只锁存 yaw_ref 不闭环，仿真里朝向指令看起来毫无作用（端到端没法验证）；每帧重锁基准会让桥 200ms 的保活帧把目标拖着跟车跑，角度环永远追不上。锁存规则与固件 `if (s_mode != HOST_DRIVE_BASE) s_base_yaw = Yaw;` 完全对齐

## 2026-10-03 — 操控页只发 `base_turn`/`off` 两种模式，且只在「指令变化」时 POST 一次 `/api/drive`
选择：操控页只发 `base_turn`/`off` 两种模式，且只在「指令变化」时 POST 一次 `/api/drive`
放弃：原来的三种驾驶模式（wheels 左右轮速直给 / base_turn / trace）、3×3 快捷动作按钮、预设速度档全删
因为：用户 m16866 说「太复杂了多了很多用不上的」，要的就是 WASD + 组合键；保活由桥的 200ms `drive_keepalive` 负责，页面重复下发只会互相盖掉

## 2026-10-03 — A+D 与 W+S 都判定为「抵消」（turn/base 归零），两个轴都为零时返回 `mode=off`；空格/ESC 优先级最高
选择：A+D 与 W+S 都判定为「抵消」（turn/base 归零），两个轴都为零时返回 `mode=off`；空格/ESC 优先级最高
放弃：让后按下的键赢（last-wins），或锁定先按下的那个键
因为：用户明确说「若果按 ad 就是左右抵消了」；对称抵消比「后按赢」更可预期，也避免手抖瞬间打到满舵

## 2026-10-03 — 页面自己再做一层本地兜底：窗口失焦、切页、松开所有键都立刻交还 `{mode:"off"}`
选择：页面自己再做一层本地兜底：窗口失焦、切页、松开所有键都立刻交还 `{mode:"off"}`
放弃：只靠桥的 200ms 保活 + 车上 500ms 看门狗
因为：看门狗是「上位机不在场」的兜底，防不了「页面还在、键已经松开/焦点丢了」这种半路状态

## 2026-10-03 — `contract.test.ts` 里「回放不带文件必须失败」改成二者皆可（明说失败 / 复用 lastReplay 连上），并补一条确定性的 `mode=bogus` 必须被拒
选择：`contract.test.ts` 里「回放不带文件必须失败」改成二者皆可（明说失败 / 复用 lastReplay 连上），并补一条确定性的 `mode=bogus` 必须被拒
放弃：继续断言一定 400
因为：mock 与桥都实现了「不给文件就复用上次那个文件」，长跑的 mock 进程会让这条断言随用例执行顺序翻脸（第二次 `npm test` 就红了一把）

## 2026-10-03 — 预设列表在桥和前端各归一一次：bridge/state.py::_normalize_preset() + web/src/lib/api.ts::normalizePresets()
选择：预设列表在桥和前端各归一一次：bridge/state.py::_normalize_preset() + web/src/lib/api.ts::normalizePresets()
放弃：只在桥归一并信任契约（手写/旧 json 仍能打黑页面）；或只在前端归一（preset_apply 等其它消费者仍拿到脏数据）
因为：preset_dir 里同时住着 preset_save() 写的预设和 _save_action_preset() 写的动作表存档 actions.json（只有 actions 列表、没有 kind/keys），缺字段会让读 p.keys 的页面抛 TypeError 并把整棵 React 树卸掉（2026-10-03 用户看到的舵机页/设置页白屏）

## 2026-10-03 — 页面渲染异常用 PageBoundary（class error boundary，Shell 里 key={page}）兜住，显示红卡+错误原文+重试按钮
选择：页面渲染异常用 PageBoundary（class error boundary，Shell 里 key={page}）兜住，显示红卡+错误原文+重试按钮
放弃：让 React 默认行为把整棵树卸掉（就是本次白屏），或只在数据层做防御
因为：单个页面的数据形状问题不该让整个控制台变白：用户看不到错误原文就无法反馈，切页也恢复不了（key={page} 让错误状态随切页复位）

## 2026-10-03 — 假遥测 web/mock/server.mjs 的 gray 改成 u8 0..255（30 + ink*195，丢线 6..16）与协议对齐
选择：假遥测 web/mock/server.mjs 的 gray 改成 u8 0..255（30 + ink*195，丢线 6..16）与协议对齐
放弃：保留 ADC 量纲（3150 - ink*2600）并在前端兼容两种量纲
因为：protocol.py:310 解出来就是 u8、host_ext.h:80 写明 255 = 压黑线；量纲不一致会让 SSR 预览里柱高全被钉在 100%，把 SSR 快照这份"像素替代证据"变成误导

## 2026-10-03 — 管道对拍构建链真 App/Svc/nvram.c（`-DHOST_PIPE_REAL_NVRAM`），并用 `--flash/--dump-flash` 交换 Flash 页 63 的 2048B 镜像来模拟掉电重启
选择：管道对拍构建链真 App/Svc/nvram.c（`-DHOST_PIPE_REAL_NVRAM`），并用 `--flash/--dump-flash` 交换 Flash 页 63 的 2048B 镜像来模拟掉电重启
放弃：只靠 t_nvram.c 的模块级测试、或让管道构建继续用 host_appstub.c 的假 nvram_touch
因为：需求要证的是「上位机点固化 → 落到车上的 Flash」，只有真协议字节 + 真 nvram.c 才能发现管道里的 "固化" 其实只是假桩计数器 ++

## 2026-10-03 — 管道程序 EOF 收尾只把**已排队**的固化跑完（nvram_state()==DONE 或连续两拍 IDLE 就退出），不空转到 1.5s 防抖到期
选择：管道程序 EOF 收尾只把**已排队**的固化跑完（nvram_state()==DONE 或连续两拍 IDLE 就退出），不空转到 1.5s 防抖到期
放弃：EOF 后固定空转 1500ms 等固化写完
因为：否则 nvram_poll 的 1.5s 自动防抖会替「固化」把记录写掉，用例就抓不住 "点了固化但没生效" —— 第一次变异验证（拿掉 host_ext.c:226 的 nvram_touch()）正是因此漏过

## 2026-10-03 — 变异验证要还原被改的文件时，先用 Copy-Item 备份再拷回，绝不用 `git checkout -- <文件>`
选择：变异验证要还原被改的文件时，先用 Copy-Item 备份再拷回，绝不用 `git checkout -- <文件>`
放弃：用 `git checkout --` 把变异文件恢复成 HEAD 版本
因为：它会连该文件**未提交**的整轮改动一起抹掉 —— 这次真的把 pipe_hostlink.c 的 nvram 改造整段抹掉了，只能重建并重跑全套验证

## 2026-10-03 — 把 firmware/test/stub/host_hal.c 的假 Flash 用 VirtualAlloc 真映射到 0x08000000（失败则退回 RAM 数组，并用 host_flash_uses_real_addr() 暴露），从而能在主机上测真 nvram.c
选择：把 firmware/test/stub/host_hal.c 的假 Flash 用 VirtualAlloc 真映射到 0x08000000（失败则退回 RAM 数组，并用 host_flash_uses_real_addr() 暴露），从而能在主机上测真 nvram.c
放弃：改 nvram.c 的读写走 HAL/宏（动产品代码，且失去'真代码原样被测'的意义），或在测试里复制一份 nvram.c 改地址（两份代码必然漂移）
因为：nvram.c 是按地址直读 Flash 的（const nvram_record_t *rec = (const nvram_record_t *)0x0801F800、memcmp((const void*)NVRAM_PAGE_ADDR,...)），不走 HAL_FLASH_*；不真映射就一读一崩（gdb 定位到 nvram.c:399）

## 2026-10-03 — 在 host_hal.c 的 HAL_GetTick() 里兑现 FLASH->CR 的擦除请求（见 PER|STRT 就按 PNB/BKER 把那页抹成 0xFF 并清 STRT）
选择：在 host_hal.c 的 HAL_GetTick() 里兑现 FLASH->CR 的擦除请求（见 PER|STRT 就按 PNB/BKER 把那页抹成 0xFF 并清 STRT）
放弃：让测试自己调 host_flash_reset()/FLASH_PageErase() 来模拟擦除（等于替固件把活干了，测不出'写前必须擦'）；也否决了改用 HAL_FLASHEx_Erase（nvram.c 的注释明确说本板 FLASH_OPTR.DBANK 读出为 0、必须绕开它）
因为：真代码 nvram_page_erase() 是手写 CR=BKER|PNB=31|PER|STRT 后立刻 t0 = HAL_GetTick()，再查 SR——主机上 HAL_GetTick() 是唯一能观察'擦除已发出'的时刻；不仿真它，页不擦、第二次保存必然卡在 nvram.c:367 的 ==0xFFFFFFFF 复验上

## 2026-10-03 — firmware/test/t_nvram.c 只链真 App/Svc/nvram.c + 真 App/Svc/trace_cal.c，其余外部依赖（app_settings_t g_cfg、app_ui_gyro_axis_mode/set/loaded、trace_read_channel、Motion_GetMode、SpeedControl_IsEnabled、GyroControl_IsEnabled、14 项动作表、13 个 v5 标定量的 host_param_get/set_local/bounds）全部在本文件内做最小替身
选择：firmware/test/t_nvram.c 只链真 App/Svc/nvram.c + 真 App/Svc/trace_cal.c，其余外部依赖（app_settings_t g_cfg、app_ui_gyro_axis_mode/set/loaded、trace_read_channel、Motion_GetMode、SpeedControl_IsEnabled、GyroControl_IsEnabled、14 项动作表、13 个 v5 标定量的 host_param_get/set_local/bounds）全部在本文件内做最小替身
放弃：再给 host_appstub.c 加一组 #ifndef HOST_REAL_NVRAM 开关（改动共享桩，10 个既有测试都要重跑回归）
因为：共享桩 host_appstub.c 里已经有假 nvram_touch（t_hostext 靠它计数），把它链进来必然与真 nvram.c 符号冲突；自包含替身既不动共享桩、又不影响既有 10 个测试

## 2026-10-03 — 用「把真固件协议层编成字节管道进程（firmware/test/pipe_hostlink.c）+ bridge/protocol.py 直接对话」来验协议一致性
选择：用「把真固件协议层编成字节管道进程（firmware/test/pipe_hostlink.c）+ bridge/protocol.py 直接对话」来验协议一致性
放弃：再写一份字段对照文档、或只做源码解析（解析到的是源码文本，不是运行时字节）
因为：C 侧 t_hostlink.c 与 Python 侧 test_protocol.py 各自手抄了一份 T_TELEM2/ACK/DRIVE_REPORT 偏移，没有任何测试证明两份抄写一致；管道对拍用的是真二进制，能抓出源码解析抓不到的字节序/帧交错/magic 问题

## 2026-10-03 — 管道构建用 -DHOST_PIPE_REAL_SERVO 链真 firmware/App/Bsp/servo.c，桩里的假舵机表与假 Servo_*、假 Servo_Task 用 #ifndef 关掉
选择：管道构建用 -DHOST_PIPE_REAL_SERVO 链真 firmware/App/Bsp/servo.c，桩里的假舵机表与假 Servo_*、假 Servo_Task 用 #ifndef 关掉
放弃：把桩的 k_def[] 改成出厂值去追平（桩仍是第二个真值来源，迟早再漂）
因为：host_appstub.c 的 k_def[] 是旧台架值 ≠ servo.c 的 k_action_default[]，用桩测永远测不出「固件运行值 == 源码出厂表」

## 2026-10-03 — 给假 HAL 头 firmware/test/stub/stm32g4xx_hal.h 补 __HAL_TIM_SET_COMPARE 与旧拼法 __HAL_TIM_SetCompare/__HAL_TIM_GetCompare
选择：给假 HAL 头 firmware/test/stub/stm32g4xx_hal.h 补 __HAL_TIM_SET_COMPARE 与旧拼法 __HAL_TIM_SetCompare/__HAL_TIM_GetCompare
放弃：改真 servo.c 去迁就桩、或用 -D 命令行宏映射（要在每个 TU 可见，更脏）
因为：真 ST HAL 的 stm32_hal_legacy.h 就提供旧拼法，本工程 servo.c/motor.c 用的也是旧拼法；假头缺这组宏属于假 HAL 不忠实，补上是纯新增、对既有 10 个主机测试无行为影响（已回归）

## 2026-10-03 — 在 tools/console/bridge/protocol.py 新增 param_table_crc(items)，按调用者给的顺序打包 <u16 id><f32 def> 算 CRC16
选择：在 tools/console/bridge/protocol.py 新增 param_table_crc(items)，按调用者给的顺序打包 <u16 id><f32 def> 算 CRC16
放弃：删掉固件的注释/把校准值写成常量（那样表一改就静默失配）
因为：固件 T_HELLO 一直在报这个 CRC、host_link.c 的注释也声称「与上位机 protocol.py 的 param_table_crc 同构」，但上位机侧从来没实现；补上才能发现参数表不一致，且它顺序敏感（固件按 SPECS[]，界面 PARAMS 顺序不同 ⇒ 必须按固件 dump 顺序传）

## 2026-10-03 — 把「回放录制」做成真正可用的功能（设置页选 data/logs/rec_*.jsonl + 倍速播放），而不是把 replay 从契约里删掉
选择：把「回放录制」做成真正可用的功能（设置页选 data/logs/rec_*.jsonl + 倍速播放），而不是把 replay 从契约里删掉
放弃：从契约里删掉 replay（省事但丢掉已有能力，且 SSR/mock 已经把它当既有特性）
因为：前端 LinkMode/api.connect/router 与桥 link.py 的 ReplayLink、main.py 的 --replay、server.py 的 _make_link 都已经支持 replay，只有设置页没接；用户要看波形复现，录了不能放等于白录

## 2026-10-03 — 录制文件用 JSONL（data/logs/rec_%Y%m%d_%H%M%S.jsonl，逐行 {"n":"telem","src":…,"t_host":<绝对 epoch ms>}），回放按 t_host 重放
选择：录制文件用 JSONL（data/logs/rec_%Y%m%d_%H%M%S.jsonl，逐行 {"n":"telem","src":…,"t_host":<绝对 epoch ms>}），回放按 t_host 重放
放弃：CSV（docs/API.md 与 store 的提示文案曾写 CSV，已一并更正）
因为：与 ReplayLink 既有读取约定一致，也便于回放时按原时间戳/倍速；CSV 需要额外解析且丢嵌套字段

## 2026-10-03 — 真车只读串口链路记为「待硬件」，软件侧不再继续追（用户明确选择「先放着，把这条记为待硬件」）
选择：真车只读串口链路记为「待硬件」，软件侧不再继续追（用户明确选择「先放着，把这条记为待硬件」）
放弃：保留两条现场恢复路径（杜邦线短接排针 pin5↔pin17 跑 out\_vcomloop.py 判定；USB-TTL 直连 U12 的 4=GND/5=PC0/6=PC1），否决了「改固件或换协议绕开串口」
因为：本机实测两个方向都没有一个字节：板子 s_stats rx=0 / tx=1318894 / telem_hz=50 且 LPUART1 配置正确（CR1=0x0000002D、BRR=0x0005C3B2、PC0/PC1=AF8+上拉、IDR 都=1），而 COM26（JLink CDC UART Port，hwid SER=000602711039 与探针 S/N 一致）静听 2.0 s 收 0 字节、发 3 帧 HELLO 板子 rx 也不动 ⇒ 缺 J-Link 20 脚排针 pin5/pin17 到 U12 第 5/6 脚那两根线，或这台探针的 VCOM 没真正工作；用户不愿现在动线

## 2026-10-03 — 参数编辑的四个端点严格按契约分工：`/api/params/set` 只改桥这边缓存并按「与车上值是否相同」标脏，`/api/params/push` 才把脏项下发到车并清脏，`/api/params/reload` 是唯一「从车上读回全表」的动作，`/api/params/defaults` 只改本机表并标脏（等用户点下发）。
选择：参数编辑的四个端点严格按契约分工：`/api/params/set` 只改桥这边缓存并按「与车上值是否相同」标脏，`/api/params/push` 才把脏项下发到车并清脏，`/api/params/reload` 是唯一「从车上读回全表」的动作，`/api/params/defaults` 只改本机表并标脏（等用户点下发）。
放弃：否决了「set 后自动下发」与「push 就是读回」这两种旧实现（前者让脏标记永远不出现、后者等于把 reload 当 push）。
因为：契约 `tools/console/docs/API.md:32-36` 和假后端 `tools/console/web/mock/server.mjs` 都是这么定义的；前端「全部下发（N）」按钮的 N 就是脏项数，set 若自作主张下发，界面上「改了没下发」的状态就不存在了。

## 2026-10-03 — 上位机连真车走**无线下载器插 U12 后出现的虚拟串口**（板侧 = U12 脚 5 = 网名 LPUART1_RX 但实为 **PC1（车发）**、脚 6 = 网名 LPUART1_TX 但实为 **PC0（车收）**、脚 4 = GND），或临时用 USB-TTL 交叉接同样三根线
选择：上位机连真车走**无线下载器插 U12 后出现的虚拟串口**（板侧 = U12 脚 5 = 网名 LPUART1_RX 但实为 **PC1（车发）**、脚 6 = 网名 LPUART1_TX 但实为 **PC0（车收）**、脚 4 = GND），或临时用 USB-TTL 交叉接同样三根线（适配器 TX → 脚 6、RX → 脚 5）
更正（2026-10-03 晚，用户 m20177 问「PC0不应该是KX吗」时核对网表）：旧写法把网名当成芯片功能，写成「脚 5 = LPUART1_RX/PC0、脚 6 = LPUART1_TX/PC1」，**脚↔芯片对应是反的**。网表 `pcb/Netlist_Schematic1_2026-09-29.tel:430-431` = `'LPUART1_RX' ; U2.9 U12.5`、`'LPUART1_TX' ; U2.8 U12.6`，U2.8 = PC0、U2.9 = PC1。
放弃：把 J-Link 自带的 "JLink CDC UART Port"(COM26) 当串口用 —— 被否决：实际测到 PC 侧发 3 帧、PC 收到 0 字节、车侧 rx/crc/resync 全 0，J-Link VCOM 的 UART 引脚根本不在去 U12 的那条排线上
因为：板子没有任何板载 USB 串口桥（连接器只有 U12 一处引出 LPUART1），PC 与 PC0/PC1 之间必须有物理线
放弃：把 J-Link 自带的 "JLink CDC UART Port"(COM26) 当串口用 —— 被否决：实际测到 PC 侧发 3 帧、PC 收到 0 字节、车侧 rx/crc/resync 全 0，J-Link VCOM 的 UART 引脚根本不在去 U12 的那条排线上
因为：板子没有任何板载 USB 串口桥（连接器只有 U12 一处引出 LPUART1），PC 与 PC0/PC1 之间必须有物理线

## 2026-10-03 — 「动作预设」做成同一项的 2 行高（上行标题+编号、下行动作名），滚动窗口按“行数”算（servo_view_rows/rows_before/view_first）
选择：「动作预设」做成同一项的 2 行高（上行标题+编号、下行动作名），滚动窗口按“行数”算（servo_view_rows/rows_before/view_first）
放弃：在列表里再插一个独立项显示名字（滚动时会与标题分离，正是用户反对的效果）
因为：用户 m13356 要求名字与标题必须**同时**滚动出现，不能“名字那行还没滚出来”

## 2026-10-03 — 动作名与角度表都放 firmware/App/Bsp/servo.c（k_action_pose[14][2] + k_action_name[14]），界面只通过 Servo_ApplyAction/ActionPose/ActionName/GetAngle 读
选择：动作名与角度表都放 firmware/App/Bsp/servo.c（k_action_pose[14][2] + k_action_name[14]），界面只通过 Servo_ApplyAction/ActionPose/ActionName/GetAngle 读
放弃：在 app_ui.c 里再抄一份动作角度/名字（旧写法），或在两个文件各维护一份
因为：界面要显示与下发同一批角度，抄一份必然走样；顺带避免改名时改两处

## 2026-10-03 — 舵机页「返回」后保持最后角度、不回零；中键在「收回」上才两路回 0°
选择：舵机页「返回」后保持最后角度、不回零；中键在「收回」上才两路回 0°
放弃：退出页面自动回 0°（会把用户刚摆好的姿态冲掉）
因为：用户 m13266 明确 0° 就是收回位、现场拿它认线，不希望一退出就弹回去

## 2026-10-03 — 把比赛动作表抽成单一真值来源：`firmware/App/Bsp/servo.c` 的 `static const uint8_t k_action_pose[SERVO_ACTION_N][2]`，`Servo_Move()`（比赛流程，含 5/9 号中间步骤与延时）与新增的 `Servo_ApplyAction()`（界面实时预览，只走终点）都从它取
选择：把比赛动作表抽成单一真值来源：`firmware/App/Bsp/servo.c` 的 `static const uint8_t k_action_pose[SERVO_ACTION_N][2]`，`Servo_Move()`（比赛流程，含 5/9 号中间步骤与延时）与新增的 `Servo_ApplyAction()`（界面实时预览，只走终点）都从它取
放弃：在 app_ui.c 里另建一份 14 项角度表（会重），或者让界面直接调 `Servo_Move()`（5/9 号里的 `HAL_Delay(400)+HAL_Delay(100)` 会把界面卡住 0.5s，而且左右连发滚动作时会卡成一串）
因为：舵机测试页要“按号选动作”且要能把上面舵机1/舵机2 两行同步成该动作的终点姿态；若在 app_ui.c 再拄一份角度表，以后调角度必然两边不同步

## 2026-10-03 — 新增 API：`Servo_ActionCount()` / `Servo_ActionPose(x,&a1,&a2)` / `Servo_ApplyAction(x)` / `Servo_GetAngle(x)`（`firmware/App/Bsp/servo.h`）
选择：新增 API：`Servo_ActionCount()` / `Servo_ActionPose(x,&a1,&a2)` / `Servo_ApplyAction(x)` / `Servo_GetAngle(x)`（`firmware/App/Bsp/servo.h`）
放弃：只加一个“取角度”接口，“中键完整播放”就会把上面两行数字与舵机实际位置弄反
因为：界面需要：①枚举动作号 ②取终点姿态不改硬件 ③只下发终点（不阻塞） ④中键完整播放后回读舵机真实角度（复合动作的中间步骤会改变 s_cur）

## 2026-10-03 — 把 TIM1/舵机的真正初始化（bsp_servo_init()）补进 Servo_Init() 里，而不是在 main_app.c 里加一行调用
选择：把 TIM1/舵机的真正初始化（bsp_servo_init()）补进 Servo_Init() 里，而不是在 main_app.c 里加一行调用
放弃：不动 servo.c、只改 main_app.c 的调用序列（能修好，但以后再有人只看 Servo_Init() 还会以为它已自足）
因为：对照已有的 Motor_Init() -> bsp_motor_init() 套路，保持“子系统的 Init() 自己负责底层初始化”的同一形状；bsp_servo_init() 内部已把四路 PWM 都 Start，所以顺手删掉 Servo_Init() 里那两句多余的 HAL_TIM_PWM_Start()

## 2026-10-03 — 舵机不动的原因定位：TIM1 从未被初始化（htim1.Instance == NULL）
选择：舵机不动的原因定位：TIM1 从未被初始化（htim1.Instance == NULL）
放弃：先怀疑 MOE/BDTR 没使能、或舵机电源轨（硬件）——都已排除（MOE 由 HAL 开；电源问题用户可现场看）
因为：真正配 PA8–PA11(AF6) + TIM1(PSC=169/ARR=19999) + 四路 PWM 通道并 Start 的是 bsp_board.c:227 的 bsp_servo_init()，而它**全工程无任何调用者**（grep 确认）；Servo_Init() 原来只调 HAL_TIM_PWM_Start(&htim1,…)，面对零初始化的句柄直接返回 HAL_ERROR，__HAL_TIM_SetCompare() 写在空指针上 ⇒ 舵机零反应。已确认 HAL 的 HAL_TIM_PWM_Start() 对 TIM1 会自己使能 MOE（stm32g4xx_hal_tim.c），不是 MOE 问题。

## 2026-10-03 — 三级串级的外环增益 s_angle_per_unit 保持 10.0f 不动（编译默认值一行未改）
选择：三级串级的外环增益 s_angle_per_unit 保持 10.0f 不动（编译默认值一行未改）
放弃：按几何推演把它往 5~7°/格 调（几何实测 0.148~0.19 单位/度 ⇒ 5.3~6.8°/格，推演说 10 过量 1.5~2 倍会振荡）
因为：14 次受控台架实验（偏置 ±0.8、每档 2 次、正负交替）：4°/格 末误差 0.523/−0.256、6°/格 0.184/−0.277（都收不干净），而 10 是 0.040/0.079、12 是 0.091/−0.088、15 是 −0.087/−0.119、20 是 0.048/−0.074（互相等价）⇒ 外环还要对抗内环滞后与静摩擦死区，增益大反而有利，推演被否证

## 2026-10-03 — 把三级串级的两个标定量（TRACE_ANGLE_PER_UNIT / TRACE_ANGLE_MAX）从宏改成运行期变量，并提供 PID_Tracing_SetCascadeGains()/GetCascadeGains()（默认值不变）
选择：把三级串级的两个标定量（TRACE_ANGLE_PER_UNIT / TRACE_ANGLE_MAX）从宏改成运行期变量，并提供 PID_Tracing_SetCascadeGains()/GetCascadeGains()（默认值不变）
放弃：每换一档增益就重编+烧录一次（一次约 2 分钟，且每次烧录都会让所有写死地址的辅助脚本作废）
因为：台架扫参需要快；默认值不变所以不影响正式行为；getter/setter 内部对 0/NaN/越界一律回落出厂值，防 SWD 写坏值把车带飞

## 2026-10-03 — 台架判陀螺活性一律看 s_frames(jy61p) 是否增长，不看 Yaw
选择：台架判陀螺活性一律看 s_frames(jy61p) 是否增长，不看 Yaw
放弃：连读两次 Yaw 看它变不变
因为：车不动时 Yaw 本来就不变；而 poke s_mode=3 + 使能角度环会立刻武装看门狗（没有 Motion_SetMode 给的宽限期），陀螺一旦静默整轮实验会静默作废

## 2026-10-03 — pivot（原地转圈）的后续标定改在**三级串级** `motor_tracing_cascade()`（循迹→角度→速度）上做，而不是我现在用的 `motor_tracing_pid_speed()`（循迹+速度、无角度环）
选择：pivot（原地转圈）的后续标定改在**三级串级** `motor_tracing_cascade()`（循迹→角度→速度）上做，而不是我现在用的 `motor_tracing_pid_speed()`（循迹+速度、无角度环）
放弃：继续在 `MOTION_TRACE`（纯循迹+速度）那一条上把 pivot 参数调到底。
因为：用户 m12856 明确说最终只有两种形态：正常循迹 = 三级 PID 串级；进入障碍后的固定流程 = 只有速度环+角度环（两层），循迹环那时只当“退出的触发”。所以他指“寻迹环+速度环”是“不伦不类”的——那条是旧板遗留、现在却恰好被 renwu.c 的直行段（97 处 `motor_tracing_pid(4.5)`）在用。

## 2026-10-03 — pivot（base_speed == 0）单独一组编译常量增益 Kp900/Ki0/Kd405，放在 `firmware/App/Svc/pid.c`，由 `PID_SetPivot(int)` 切换；前进循迹仍走 host 参数表（本次一行未动）
选择：pivot（base_speed == 0）单独一组编译常量增益 Kp900/Ki0/Kd405，放在 `firmware/App/Svc/pid.c`，由 `PID_SetPivot(int)` 切换；前进循迹仍走 host 参数表（本次一行未动）
放弃：直接把参数表的 LINE_KP/KI/KD 默认值改成实测值（那样前进循迹也会被改到，而直线行走还没条件测）；也没有新增 HP_* 参数 id（要同时改协议文档、SPECS 与上位机参数表，代价大）。
因为：用户 m12763 明确说那套是旧代码抄来的、单位与硬件都变了、不能再当基准；但这一轮只能测原地转圈，前进循迹的增益必须留到能直行时重新标。

## 2026-10-03 — pivot 增益 Ki 取 0
选择：pivot 增益 Ki 取 0
放弃：用小 Ki（如 10.8）去消除最后 0.2~0.3 单位的静摩擦死区。
因为：第 12 节实测：偏置 0.9 单位 + 3000ms 时 Ki=10.8 会积分 windup，把车甩飞 -131.9°；pivot 的偏差小、但不定时出现大偏差（切换设定值时），Ki 的风险大于收益。

## 2026-10-03 — `PID_SetPivot()` 只在模式真的变化时清积分/微分状态（同模式重复调用必须是空操作）
选择：`PID_SetPivot()` 只在模式真的变化时清积分/微分状态（同模式重复调用必须是空操作）
放弃：每次调用都 PID_Reset()。
因为：这个函数从 5ms 控制中断里每拍被调一次；如果每次都清，`s_has_last` 永远是 0，微分项永远出不来（Kd=405 完全失效）。已用主机测试钉死（同模式重复调用后误差变化仍会产生 405×Δe）。

## 2026-10-02 — pivot（循迹原地转）不用低增益：可用区间是 Kp 600~900、Kd≈0.15·Kp，且只放 RAM 不动全局默认 LINE_KP=2700
选择：pivot（循迹原地转）不用低增益：可用区间是 Kp 600~900、Kd≈0.15·Kp，且只放 RAM 不动全局默认 LINE_KP=2700
放弃：按“PID 从小往大慢慢调”的低增益（Kp 200~450）。
因为：实测 Kp 300/450 的命令只有 90~140mm/s，落在静摩擦死区里，车停在离中心 0.23~0.32 个单位收不回去（低增益不是更安全，而是可能停偏）；而 LINE_KP=2700 是直线循迹标定的，改它会动到比赛流程。

## 2026-10-02 — 台架验证“运动中不写 Flash”时，poke 的是 nvram 的 RAM 镜像 `s_saved` 的第 0 字节 + 把 `s_change_ms` 置成“现在”，而不是去改 `s_live` 或真标定
选择：台架验证“运动中不写 Flash”时，poke 的是 nvram 的 RAM 镜像 `s_saved` 的第 0 字节 + 把 `s_change_ms` 置成“现在”，而不是去改 `s_live` 或真标定
放弃：改 `s_live`（更直接地制造“值变了”）。
因为：`nvram_service()` 真正写 Flash 用的是 `write_record(&s_live)`（nvram.c:461）⇒ poke `s_saved` 永远不会污染 Flash 里的标定；而且保存成功后 `s_saved = s_live`（:462）会把我 poke 的字节自然抹掉，留下一份干净的取证。

## 2026-10-02 — pivot 的 A1/A2/A3 三组实验全部只改 RAM 里的 `s_param`（`out/_tracetest.ps1 -Kp/-Ki/-Kd`），固件编译默认值一行不动
选择：pivot 的 A1/A2/A3 三组实验全部只改 RAM 里的 `s_param`（`out/_tracetest.ps1 -Kp/-Ki/-Kd`），固件编译默认值一行不动
放弃：把 `LINE_KP/KI/KD` 默认值改成 600/0/90（台架看起来更稳）。
因为：默认值是给直线循迹用的（上一个决定），台架指标不能拿去换比赛行为；改完还要同步 host_link.c SPECS / app_ui.c g_cfg / *_FALLBACK 三处 + 上位机镜像，风险与收益不成比例。

## 2026-10-02 — 循迹丢线保护：把丢线分支从“永速保持 last_pid_output”改成“超过 TRACE_LOST_MS(300ms) 后在 TRACE_LOST_DECAY_MS(500ms) 内线性收到 0”（短暂丢线仍保持转向）
选择：循迹丢线保护：把丢线分支从“永速保持 last_pid_output”改成“超过 TRACE_LOST_MS(300ms) 后在 TRACE_LOST_DECAY_MS(500ms) 内线性收到 0”（短暂丢线仍保持转向）
放弃：不改（保持原行为），或丢线就直接置 0：前者实测会飞车，后者过细缝时会丢掉寻线所需的小转向。
因为：实测：设定值从 4.5 切到 2.0 时命令满舵 7200 追击，线滑出探头后 reqL 卡在 7200、车身绕了 360°+ 停了不下来——保持上一次转向的初衷是“过缝/过十字”，但没有衰减就变成永远满舵。

## 2026-10-02 — 验证丢线衰减不能靠 SWD 直接“造丢线”，只能跑真实场景（在线上把设定值切到大偏差）
选择：验证丢线衰减不能靠 SWD 直接“造丢线”，只能跑真实场景（在线上把设定值切到大偏差）
因为：实测把车停在路面白底上时 all_sensors_lost 在 900 个样本里全是 0——固件的白底基线逻辑把地面当成“线”在跟，根本不进丢线分支；而 forced poke 又会被每 5ms 重新计算掉。

## 2026-10-02 — 所有 J-Link Commander 脚本最后必须带 go 或 qc
选择：所有 J-Link Commander 脚本最后必须带 go 或 qc
因为：漏了就直接掉进交互模式永远等 stdin（实测卡死 ~12 分钟、job 只能杀掉）；这类脚本是本项目上板实验的主要形式。

## 2026-10-02 — 循迹 pivot 台架实测后，**不改全局 LINE 增益默认值**（`HP_LINE_KP/KI/KD` 仍 2700/10.8/360），建议改成“pivot 单独一组增益”（`s_base==0` 时切温和值）
选择：循迹 pivot 台架实测后，**不改全局 LINE 增益默认值**（`HP_LINE_KP/KI/KD` 仍 2700/10.8/360），建议改成“pivot 单独一组增益”（`s_base==0` 时切温和值）
放弃：按 pivot 表现直接把编译默认值改成 Kp600/Kd90 或加长偏置静置时间。
因为：pivot（base=0 + 一小段打印线）与前进循迹（base=6480 + 长线）不是同一个被控对象；出厂增益是给后者调的，改全局默认等于拿比赛那套换台架指标。而且 pivot 下重心↔转角强非线性（实测单位/度在 0.016~0.216 间乱跳），调 PID 收益本来就有限。

## 2026-10-02 — “先偏置、再启动环”的台架协议必须包含“断电静置”那一步（`s_mode=0/s_enabled=0/s_req=0` + TIM8 四个 CCR 写 0）
选择：“先偏置、再启动环”的台架协议必须包含“断电静置”那一步（`s_mode=0/s_enabled=0/s_req=0` + TIM8 四个 CCR 写 0）
放弃：偏置建好后直接改设定值启动环。
因为：带着角速度/带着速度环存量启动会直接引发极限环：out/_trs2 里 data 在 1.15↔8.05 之间扫、reqL ±7200 打满、车净转 -34° 不收敛；而同一套增益在“车静止”协议下（_trs1）是稳的。真实固件里 Motion_SetMode 会把 s_req 清 0，台架必须照做。

## 2026-10-02 — 循迹 pivot 的读数以 `g_live.trace[]/gray[]` 为活性证据，不看 `data`/`trace_pos_frames`/`all_sensors_lost`
选择：循迹 pivot 的读数以 `g_live.trace[]/gray[]` 为活性证据，不看 `data`/`trace_pos_frames`/`all_sensors_lost`
放弃：把 data 当活性/收敛证据。
因为：循迹环关着（mode=0）时这三个量都是冻结值（只有 PID 路径会刷新），本轮多次因此误判；每次跑 pivot 前先用 `out/_grayread.ps1` 确认线在探头下（已因此白跑过一整轮）。

## 2026-10-02 — 角度环旋钮扫描（RateKp/RateKd/CorrSlew/CorrLimit）后**一个编译默认值都不改**，保持 `s_rate_kp=250`、`s_rate_kd=4`、`s_corr_slew=300000`、`s_corr_limit=2000`
选择：角度环旋钮扫描（RateKp/RateKd/CorrSlew/CorrLimit）后**一个编译默认值都不改**，保持 `s_rate_kp=250`、`s_rate_kd=4`、`s_corr_slew=300000`、`s_corr_limit=2000`
放弃：把 corr 斜率限幅调小（slew 20k/40k）让中段更平滑，或把 Kd 提到 8/12 让整定更快。
因为：事先定的判据是“两头都赢才改”：slew 20k 把中段翻转从 9 降到 5，但到位后回摆从 0.13~1.00° 涨到 5.91°（差 3~45 倍）；Kd 8/12 最多快 10%、残差反而变大；中段打满是结构性的（速率误差 > 8°/s 就撞限幅 2000，巡航段误差 30~110°/s），降 Kp 到 100 翻转次数反而从 9 涨到 10。

## 2026-10-02 — “把 `s_corr_limit` 从 2000 提到 3600 来治卡死”这个显而易见的修法被**否决**
选择：“把 `s_corr_limit` 从 2000 提到 3600 来治卡死”这个显而易见的修法被**否决**
放弃：用提高限幅让比例项自己破开静摩擦（实测起转阀值 ≈3450 > 2000）。
因为：实测两头都没赢：起步时间 318ms 仍在默认斜坡的 233~348ms 区间内（`out/_rise.py` 的 t10/t50/t90），而到位残差从 0.4~1.0° 变成 1.4~2.4°（3 次 `out/_L_lim3600_*.txt`）。

## 2026-10-02 — 台架手柄 `out/_steptest.ps1` 新增的 RAM 调参（`-RateKp -RateKd -CorrSlew -CorrLimit -Decel`）**开始前先把原值读出来、收尾原样写回**
选择：台架手柄 `out/_steptest.ps1` 新增的 RAM 调参（`-RateKp -RateKd -CorrSlew -CorrLimit -Decel`）**开始前先把原值读出来、收尾原样写回**
放弃：直接写 RAM、实验结束不再管（反正一复位就恢复）。
因为：板子是用户手边能直接按开始键跑的真车；如果忘了写回，下一次“跑一下看看”用的就是实验中的异常增益（比如 Kp=18 那个会一直转的配置）。

## 2026-10-02 — 台阶测试一律 ±交替（或每 3 次同向就用 -60° 回绕），把它当成硬规则
选择：台阶测试一律 ±交替（或每 3 次同向就用 -60° 回绕），把它当成硬规则
放弃：为了省时间连着跑同方向台阶。
因为：本轮扫描 7 次全用 -Deg 60 ⇒ 车实际累计转了 ≈+472°，把线缠到一整个圈——这正是用户 m05072 提过的“台阶测试改交替方向防缠线”，结果又踩了一次。

## 2026-10-02 — 角度环台阶 A/B 用“从 RAM 把 `s_ramp`（0x200000C4）写成 300000”来复现第一轮修掉的那个 `<=` 缺陷，而不是回退代码重新烧录
选择：角度环台阶 A/B 用“从 RAM 把 `s_ramp`（0x200000C4）写成 300000”来复现第一轮修掉的那个 `<=` 缺陷，而不是回退代码重新烧录
放弃：改回 `speed_control.c:306` 的 `<=` 再烧一版固件做对照。
因为：同一份固件、同一个台面、相隔几分钟就能做完 2×2×3，混淆因素最少；而且不用为了实验把已知有缺陷的代码再炸回板上。

## 2026-10-02 — 新采的高采样率台阶数据（1.4ms/点）先 `--down 24` 降到 24ms 再用 `out/_angle_stats.py` 算指标
选择：新采的高采样率台阶数据（1.4ms/点）先 `--down 24` 降到 24ms 再用 `out/_angle_stats.py` 算指标
放弃：直接拿 1.4ms 数据算，或者另写一套指标代码。
因为：用 1.4ms 算出来的 peak（374~961°/s）、nBack（16~26）全是被量化台阶放大的假值；而指标代码只留一份 `_angle_stats.py`，才能和 10-02 凌晨那批出厂基线日志直接对比。

## 2026-10-02 — 台架台阶实验只把 `gyro_control_enabled` 置 1，**不动** `s_mode`
选择：台架台阶实验只把 `gyro_control_enabled` 置 1，**不动** `s_mode`
放弃：把 `s_mode`（0x20000D04）写成 1（MOTION_GYRO）。
因为：`firmware/App/Svc/motion.c:171-173` 的 MOTION_IDLE 分支本来就在 `GyroControl_IsEnabled()` 时调 `GyroControl_Task()` —— 比赛流程的转弯也走这条路，照它驱才和真实路径一致。

## 2026-10-02 — 主板接上后，前三轮标“未验证”的条目全部用 SWD 在线“动手脚”验证，而不是等用户配合拔插模块
选择：主板接上后，前三轮标“未验证”的条目全部用 SWD 在线“动手脚”验证，而不是等用户配合拔插模块
放弃：让用户拔掉灰度模块/屏来触发 I2C 失败，或者等下次台架排障时再看。
因为：J-Link 的 `connect` 不复位运行中的目标，可以直接改 `GPIOB->MODER` 把 SDA 拉死、屏蔽 USART3 中断冻住陀螺帧，既能量到真实现象，又不会把车开起来。

## 2026-10-02 — I2C 总线健康看门狗的决策逻辑单独放 App/Svc/bus_health.c（纯逻辑、不碰 HAL），board.c 只留三个薄封装（board_i2c_report/board_i2c_watchdog/board_i2c_recover_count），真正恢复仍调老的 board_i2c_bus_recover()
选择：I2C 总线健康看门狗的决策逻辑单独放 App/Svc/bus_health.c（纯逻辑、不碰 HAL），board.c 只留三个薄封装（board_i2c_report/board_i2c_watchdog/board_i2c_recover_count），真正恢复仍调老的 board_i2c_bus_recover()
放弃：在 board.c 里直接写计数+恢复（第一版就是那样，34 行静态状态）；换到 bus_health.c 是为了能主机侧单测（board.c 一 include HAL 就没法进主机测试）。
因为：判据是三条件：连续失败 ≥ BUS_HEALTH_FAIL_LIMIT(5) + 距上次恢复 ≥ BUS_HEALTH_RECOVER_MIN_MS(1000ms) + 总线空闲（OLED 不忙）；这三个都是纯逻辑，能断言。

## 2026-10-02 — 总线恢复不做成“失败就重配”，而是攒够 5 次连败才请求、且两次之间至少 1s、且必须在总线空闲时才执行
选择：总线恢复不做成“失败就重配”，而是攒够 5 次连败才请求、且两次之间至少 1s、且必须在总线空闲时才执行
放弃：每帧读失败就调 board_i2c_bus_recover()。
因为：board_i2c_bus_recover() 里有 9 ×(1ms 低 + 1ms 高)+STOP ≈ 阻塞 12ms；没插灰度模块时每帧触发会把主循环拖垮，还会打断屏的 DMA 搬运。

## 2026-10-02 — 主机测试桩给灰度串口加“模块会应答”的机制（host_uart4_reply_set + host_uart4_reply_wobble），而不是往接收缓冲里预先喂帧
选择：主机测试桩给灰度串口加“模块会应答”的机制（host_uart4_reply_set + host_uart4_reply_wobble），而不是往接收缓冲里预先喂帧
放弃：复用已有的 host_uart4_feed() 预喂一帧。
因为：trace_cal 的 uart_read_analog() 每次读之前会先冲刷残留字节（while board_uart4_read_byte 丢掉），预喂的帧会被冲掉 —— 必须模拟“发 0x57 请求才回帧”的从机行为；wobble 用来造抖动，覆盖“采到的点整体在抖就整点作废”。

## 2026-10-02 — 比赛的 renwu.c A() 里那 7 个不可达分支只做报告，不改代码
选择：比赛的 renwu.c A() 里那 7 个不可达分支只做报告，不改代码
放弃：直接把 renwu.c:778 的 black_num=23 改成 =22 恢复状态 22 —— 否决，属于未经测试的行为改动。
因为：改 black_num 跳转 = 改变整车动作顺序，主板未连无法验证；且 21/22 是同一路口保留的两种走法、文件里还有大量注释掉的备选动作，单看代码看不出最终意图。

## 2026-10-02 — 陀螺失联看门狗的判据用 GyroControl_IsEnabled() 而不是 Motion 模式
选择：陀螺失联看门狗的判据用 GyroControl_IsEnabled() 而不是 Motion 模式
放弃：继续只盯 Motion_GetMode() —— 否决，等于任务流程没保护。
因为：renwu.c 的转弯是直接 GyroControl_Start()/Toggle_Gyro_Control()，不经过 Motion_SetMode ⇒ 原来的判据看不到比赛流程里的角度环，失联时会拿着冻结角速度一直转。

## 2026-10-02 — 陀螺仪失联（500ms 收不到合法帧）时让车**停车并置故障位**，而不是用最后一帧数据继续跑
选择：陀螺仪失联（500ms 收不到合法帧）时让车**停车并置故障位**，而不是用最后一帧数据继续跑
放弃：①继续用冻结值跑（危险）②在 5ms 中断里发串口/RTT 报警（`host_link_send_text` 阻塞 ~3.4ms，会自伤中断）——故障位改为由主循环每秒打进 RTT
因为：JY61P 一断，`Yaw`/`Gyro_Z` 冻结成常数 ⇒ 角度环会误判"已到位"（s_law=3 的到位锁存还会把修正清零），车会笔直冲出去；停车是唯一安全的降级

## 2026-10-02 — 运动中一律不擦写 nvram（推迟到停车后），不做超时兜底
选择：运动中一律不擦写 nvram（推迟到停车后），不做超时兜底
放弃：边跑边存（会顿）／加超时强制存（宁可丢失一次设置也不能打断控制）
因为：记录页与代码同处 Flash bank2，擦/写期间从同 bank 取指被 stall 几十毫秒 ⇒ TIM6 的 5ms 控制中断被拖住，车顿一下冲出去

## 2026-10-02 — 主机侧单测用 `-fsanitize=undefined -fsanitize-undefined-trap-on-error`
选择：主机侧单测用 `-fsanitize=undefined -fsanitize-undefined-trap-on-error`
放弃：只靠断言（漏掉 UB）／装 clang（宿主没有）
因为：MinGW 没有 asan/ubsan 运行库，只有 trap 模式可用；越界/有符号溢出/除零/空指针会变成非法指令，进程以 0xC000001D 结束即判失败

## 2026-10-02 — `NormalizeAngle()` 用 `fmodf` + 非有限值返回 0，取代无界 `while` 归一
选择：`NormalizeAngle()` 用 `fmodf` + 非有限值返回 0，取代无界 `while` 归一
放弃：给 while 加迭代上限（仍随角度线性变慢）
因为：无界 while 在 5ms 中断里被喂 ±Inf 就是永久死循环（中断永不返回、电机保持上一拍输出）；fmodf 是常数时间

## 2026-10-02 — 非对称斜坡做在 `speed_control.c` 内部（减速/换向 300000 PWM/s、加速 20000），而不是只给角度环开快斜坡
选择：非对称斜坡做在 `speed_control.c` 内部（减速/换向 300000 PWM/s、加速 20000），而不是只给角度环开快斜坡
放弃：把参数暴露成 gyro 侧调用 SpeedControl_SetRamp 的耦合方案（否决）；全局把斜坡都改成 300000（否决：起步会变陡、失去防打滑）
因为：刹车/换向只是**降**驱动，不会引起电流尖峰（比加速更安全），而且避免 gyro 模块和速度环跨模块耦合；起步防打滑照旧

## 2026-10-02 — `s_rate_kd` 默认 0 → 4.0f（PWM/(°/s²)）
选择：`s_rate_kd` 默认 0 → 4.0f（PWM/(°/s²)）
放弃：Kd=0（旧默认）、Kd 5/6（实测更差）、以及早先试的 500/800（量级错，直接欠转 25~32°）
因为：台架扫 0/3/4/5 各 3 次，Kd=4 的 backDeg 最小（1.17~3.11° vs Kd0 的 4.0~9.2°）；Kd 5/6 反而更差（饱和时阻尼项被同一个 ±s_corr_limit 吃掉）

## 2026-10-02 — 保留 `SPEED_RAMP_DEFAULT 20000` 不动，只加"朝 0/反号"的快斜坡 `s_ramp_brake`
选择：保留 `SPEED_RAMP_DEFAULT 20000` 不动，只加"朝 0/反号"的快斜坡 `s_ramp_brake`
放弃：取消斜坡（s_ramp=0，等价全局瞬时）
因为：加速方向的慢斜坡是防打滑/防电流冲击的，实测没坏处；只有刹车/换向那段 200ms 滞后在害角度环

## 2026-10-02 — 角度环 s_law=3 的"到点停死"用三件事实现：①新增 `s_settle_rate`（默认 30°/s）——停车带内且 |v| < 它 ⇒ corr = 0 直接松手滑行，不再用 ±limit 满权限刹车来回抽（原来只在 |v| < s_ff_rate(3°/s) 才松手）；②到位锁存的速率判据从写死的 `s_ff_rate*2`(=6°/s) 改成 `s_settle_rate`；③加"漂出停车带就解锁"（已锁存且 |err| > s_settle_deg*1.5 ⇒ s_settled=0），原来 s_settled 只有"换目标"一条释放路径。
选择：角度环 s_law=3 的"到点停死"用三件事实现：①新增 `s_settle_rate`（默认 30°/s）——停车带内且 |v| < 它 ⇒ corr = 0 直接松手滑行，不再用 ±limit 满权限刹车来回抽（原来只在 |v| < s_ff_rate(3°/s) 才松手）；②到位锁存的速率判据从写死的 `s_ff_rate*2`(=6°/s) 改成 `s_settle_rate`；③加"漂出停车带就解锁"（已锁存且 |err| > s_settle_deg*1.5 ⇒ s_settled=0），原来 s_settled 只有"换目标"一条释放路径。
放弃：只抬比例 `s_rate_kp`（80→250/300）——同一配置一次死一次抽，"尾部死"取决于速率偶尔掉到 6°/s 以下够 150ms，是抛硬币；`s_rate_kd` 阻尼（量级应是 1~5 PWM/(°/s²)，试过 500/800 直接欠转 25~33°）；`s_v_floor`（早前试过 30~35，换来 ±2~3° 来回摆）。
因为：实测尾摆是速率环的饱和型极限环（|v| 常年 20~50°/s、corr 在 ±2000 之间来回翻、听得见但车不转），而 6°/s 的锁存门槛几乎永不满足 ⇒ 锁存形同不存在、"到点停死"从来没真正生效。

## 2026-10-02 — 角度环参数定版：`s_rate_kp` 250（原 80）、`s_ang_kp` 3.5（原 4.0）、`s_settle_rate` 30、`s_settle_rate` 相关带内释放逻辑如上、`s_corr_limit` 保持 2000、`s_law` 初值 3。
选择：角度环参数定版：`s_rate_kp` 250（原 80）、`s_ang_kp` 3.5（原 4.0）、`s_settle_rate` 30、`s_settle_rate` 相关带内释放逻辑如上、`s_corr_limit` 保持 2000、`s_law` 初值 3。
放弃：Kp250 配 `s_corr_limit` 4000（实测打滑、超调 18~20°、尾摆更凶）；`s_settle_rate` 40（更快但 4 次里一次冲过头 4.13°）；15~25（收尾常停在目标前 1.4~1.8°，因为松手早、摩擦只补得回 v²/(2a)）；"带内朝目标滑行就不刹"的改法（收尾误差没变小，反而多出 4.69° 冲过头，已回退并在注释里记下）。
因为：台架扫值：Kp250+限幅2000+SR30 连跑 4 次超调 ≤0.38°、进 ±2° 1.6~1.7s、收尾误差 0.06~1.52°，且 4/4 尾巴完全死（tail|v|=0、tail|corr|=0）。

## 2026-10-02 — 接受修好后收尾误差从 0.06~0.53° 放宽到 0.06~1.52°（不再追求亚度级），换来尾巴彻底静止（corr 恒 0、角速度恒 0）。
选择：接受修好后收尾误差从 0.06~0.53° 放宽到 0.06~1.52°（不再追求亚度级），换来尾巴彻底静止（corr 恒 0、角速度恒 0）。
因为：低起转阈值的死区执行器上"全程平滑 + 亚度精准 + 到点停死"三者不可兼得：要贴住目标就得在停车带里持续点着电机（就是用户听到的抽动）；用户 m05052/m05505 与 m09849 的取向明确是"停死 + 别吵"。`s_settle_rate` 就是留给现场再权衡的旋钮（调小更准但会多抽，调大更快但可能冲过头）。

## 2026-10-02 — 固件默认控制律改成 `s_law = 3`（连续速率环 + 到位锁存），写入 `firmware/App/Svc/gyro_control.c:89` 的初始值
选择：固件默认控制律改成 `s_law = 3`（连续速率环 + 到位锁存），写入 `firmware/App/Svc/gyro_control.c:89` 的初始值
放弃：保持初始值 0（老位置 PID + 陀螺阻尼）—— 那支只有台架 SWD 写 `s_law` 才活，复位就回老 PID
因为：用户 m09993：你直接烧录就行 —— 台架调好的那支必须就是上电跑的那支

## 2026-10-02 — 保持 `s_corr_limit` 的编译默认 2000（没有把台架那次的 4000 写回代码）
选择：保持 `s_corr_limit` 的编译默认 2000（没有把台架那次的 4000 写回代码）
放弃：把默认抬到 4000：峰速 110~124°/s、整定 1.1~1.5s（实测过），代价是尾摆变大（0~2° 超调、±3° 游走）
因为：4 次 60° 台阶实测：限幅 2000 下 4/4 无超调（-0.03/+0.54/+0.24/-0.01°）、到位误差 ≤0.53°、整定 1.65~2.07s（±2°）；4000 那一组是 RAM 里改的、未入代码，要不要抬待用户定

## 2026-10-02 — 角度环展示用已录日志重画图，不现场跑台阶
选择：角度环展示用已录日志重画图，不现场跑台阶
放弃：直接在架空状态跑台阶：轮子不吃反力矩，误差永远到不了目标，会一直全速空转既吵又无意义
因为：车当前被架起（轮子悬空），原地台阶没有物理意义，且用户刚抱怨过噪音

## 2026-10-02 — 新增 `draw_cal_set_name(x_right)`（右对齐写全名「普通」/「亚克力」），只在标定向导的 `WAIT_BLACK`/`WAIT_WHITE` 两个状态画在右上角 x=122；`DONE`/`FAILED` 保持单字 `普`/`亚`（x=60 起要报质量/原因，放不下）
选择：新增 `draw_cal_set_name(x_right)`（右对齐写全名「普通」/「亚克力」），只在标定向导的 `WAIT_BLACK`/`WAIT_WHITE` 两个状态画在右上角 x=122；`DONE`/`FAILED` 保持单字 `普`/`亚`（x=60 起要报质量/原因，放不下）
放弃：给标题加“普 ”前缀 —— 早验证过会顶到 DONE/FAILED 右侧 x=60 起那串字上；循迹页也换全名 —— 标题行排满（徯迹+数据源+中键标定）放不下
因为：标定已经只剩一个入口（中键），而它写进的是“正在预览的那一套”；不把目标写出来，用户没法确认这一按到底改的是哪套（用户 m09876）

## 2026-10-02 — `BAT_RUN_MIN_MA` 保持 **1500mA**（≈ 实测空转全速 426mA 的 3.5 倍），并把实测数字写进 `firmware/App/Src/app_ui.c` 那个常数旁边的注释里
选择：`BAT_RUN_MIN_MA` 保持 **1500mA**（≈ 实测空转全速 426mA 的 3.5 倍），并把实测数字写进 `firmware/App/Src/app_ui.c` 那个常数旁边的注释里
放弃：按实测空转值（426mA）当下限 —— 会算出 2H51M 这种没人信的剩余时间
因为：实测两轮全速空转稳态只有 426mA（11.43V）—— 电机本身不费电，地面上费的是顶摩擦/加速/转向，取空转值会把剩余时间算得乐观得没人信（用户 m09509 明确要保守）

## 2026-10-02 — SWD 跑电机的办法：`s_ramp`(`0x200000b4`)=0（取消斜坡）+ `s_req[0]/[1]`(`0x2000102c`/`0x20001030`) = `7200.0f`=`0x45E10000` + `s_enabled`(`0x20000feb`)=1；停：`s_req`=0 等 3s 再 `s_enabled`=0，并**必须把 TIM8 四个比较寄存器 `0x40010434/38/3C/40` 写 0**（关使能不会清断寄存器，线圈会一直通）
选择：SWD 跑电机的办法：`s_ramp`(`0x200000b4`)=0（取消斜坡）+ `s_req[0]/[1]`(`0x2000102c`/`0x20001030`) = `7200.0f`=`0x45E10000` + `s_enabled`(`0x20000feb`)=1；停：`s_req`=0 等 3s 再 `s_enabled`=0，并**必须把 TIM8 四个比较寄存器 `0x40010434/38/3C/40` 写 0**（关使能不会清断寄存器，线圈会一直通）
放弃：只关 `s_enabled`（PWM 寄存器保留最后一次值 ⇒ 轮子会一直转）
因为：SWD 调不了函数，只能推状态；已证实 Motion_Mode 停在 IDLE 时没人覆写 `s_req`/`s_enabled`（Motion_SetMode 只在换模式时写），所以推这两个量就够

## 2026-10-02 — 循迹页的「右键换套」定性为**纯预览**：只调 `trace_cal_select_set()`（只动 RAM），不写 `g_cfg`、不落 Flash、不弹「保存成功」；进页一律先把预览拉回第 0 套（普通）
选择：循迹页的「右键换套」定性为**纯预览**：只调 `trace_cal_select_set()`（只动 RAM），不写 `g_cfg`、不落 Flash、不弹「保存成功」；进页一律先把预览拉回第 0 套（普通）
放弃：原实现（用户 m07888 那版）右键 = cal_set_apply()：写 g_cfg.trace_cal_set 并立刻生效 + 存 Flash + 弹提示 —— 用户认为"都记住了为什么还要再写一下 Flash"
因为：用户 m09558：这一页只是为了对比两套标定的显示效果，不是"选哪套去跑"的开关；真跑任务时由任务程序自己调 trace_cal_select_set() 决定用哪套，所以那个标志位和掉电保存都不需要

## 2026-10-02 — 删掉设置菜单里的「标定套」项（`k_settings_items` 第 4 行、`k_int_bindings` 下标 16、`k_cal_set_names`）、删掉 `app_ui_tick` 里"标定套跟随生效"的 watcher 和 `s_set_applied`，nvram 记录的 `cal_set` 固定写 0
选择：删掉设置菜单里的「标定套」项（`k_settings_items` 第 4 行、`k_int_bindings` 下标 16、`k_cal_set_names`）、删掉 `app_ui_tick` 里"标定套跟随生效"的 watcher 和 `s_set_applied`，nvram 记录的 `cal_set` 固定写 0
放弃：保留菜单项 + watcher（看起来仍能改"开机默认用哪套"，但与"程序自己选"的模型重复且会写 Flash）
因为：那个字段不再表达任何用户选择：留着菜单项会变成"改了没反应"的假控件，而只要它跟着 s_set 变，预览一下就会让 payload 变化 ⇒ nvram_poll 白写一次 Flash + 弹提示

## 2026-10-02 — 删掉「选标定套」页 `TRACE_CAL_PICK` 与 `trace_cal_wizard_pick()`（枚举值、函数、声明、out/_wiz.ps1 的 -Case pick/-SetN 一起删）
选择：删掉「选标定套」页 `TRACE_CAL_PICK` 与 `trace_cal_wizard_pick()`（枚举值、函数、声明、out/_wiz.ps1 的 -Case pick/-SetN 一起删）
放弃：保留代码只摘入口（上一版做法）—— 结果是文档/记忆里还留着"那页还在"的说法，容易误导
因为：中键已直接进标定（用户 m09335），这页没有入口；留着死代码会与"中键标定、右键只预览"的新语义混淆

## 2026-10-02 — SWD 远程调两套标定时，**绝不允许只写 `s_set`**（`0x20001354`）；要模拟换套必须同时把 cur 与 s_other 两组数据一起对调（16 字节/数组：valid@0、pol@1、bad@9、blk@18、wht@34、thr@50、jit@66，uint16 数组，共 82B）
选择：SWD 远程调两套标定时，**绝不允许只写 `s_set`**（`0x20001354`）；要模拟换套必须同时把 cur 与 s_other 两组数据一起对调（16 字节/数组：valid@0、pol@1、bad@9、blk@18、wht@34、thr@50、jit@66，uint16 数组，共 82B）
因为：只写 s_set 会让 `trace_cal_export()` 的 cal/cal_b 换位 ⇒ payload 变化 ⇒ nvram 存下"错位"的记录（本次真发生过，已按历史读数逐字节复原：普通 blk 172/167/203/177/221/229/221/229、wht 278/312/366/327/362/363/342/314），而且 `trace_cal_data_t` 的 6 个数组都是 uint16[8]（不是 uint8[8]），按 8 字节切片只会搬一半

## 2026-10-02 — 主页“还能跑多久”按**车在跑**算，且三重保守：容量只算 6 折（`BAT_USABLE_PCT 60`，5000->3000mAh）、先扣掉 15% SoC 地板（`BAT_SOC_FLOOR 15`，放到 3.45V/节车已经跑不动）、电流取 `max(实测低通, BAT_RUN_MIN_MA=1500)`
选择：主页“还能跑多久”按**车在跑**算，且三重保守：容量只算 6 折（`BAT_USABLE_PCT 60`，5000->3000mAh）、先扣掉 15% SoC 地板（`BAT_SOC_FLOOR 15`，放到 3.45V/节车已经跑不动）、电流取 `max(实测低通, BAT_RUN_MIN_MA=1500)`
放弃：用实测静态电流算（→几小时到十几小时，不可信）；只把容量打 8 折（`BAT_USABLE_PCT 80`，就是旧版）
因为：用户 m09509：“你这个算的时间好长呀，这么长时间应该坚持不了吧？我说的是保守一点的呀”。台架静态 105mA 算出 16H39M（撞 999min 上限），没人信

## 2026-10-02 — 主页中段整体下移 2px：分隔线 y 27->29、`TERRAIN ROBOT` y 29->31、剩余时间行 `BAT_ROW2_Y` 17->19（电量两行间距 2px->4px，时间墨迹 23..30 底边仍与分隔线 29..30 齐）
选择：主页中段整体下移 2px：分隔线 y 27->29、`TERRAIN ROBOT` y 29->31、剩余时间行 `BAT_ROW2_Y` 17->19（电量两行间距 2px->4px，时间墨迹 23..30 底边仍与分隔线 29..30 齐）
放弃：只移分隔线+时间（TERRAIN 会离分隔线太近）；或把 TERRAIN 提到 29 上面（用户明确要求往下）
因为：用户 m09509：电量两行挤（2px）、TERRAIN 与分隔线要往下 1-2 格

## 2026-10-02 — 主页右列（电量）改成**与左列逐行对齐**：图标行 y=9..19 与 `HAIXINBEI` 墨迹同高，剩余时间墨迹 21..28 底边与标题下的分隔线（27..28）对齐
选择：主页右列（电量）改成**与左列逐行对齐**：图标行 y=9..19 与 `HAIXINBEI` 墨迹同高，剩余时间墨迹 21..28 底边与标题下的分隔线（27..28）对齐
放弃：各行自己居中（旧版：图标 4..14、时间 17..24）—— 左右两列基线不齐，看着乱
因为：用户 m09335："这右边的一整块儿是跟那个海信杯加它那个下划线分隔线……对齐的那种"

## 2026-10-02 — 电池图标左沿 74 -> 78（常量改用 `BAT_ICON_X 78`），百分比文字的起点拆成独立常量 `BAT_TEXT_X 97` **原地不动**
选择：电池图标左沿 74 -> 78（常量改用 `BAT_ICON_X 78`），百分比文字的起点拆成独立常量 `BAT_TEXT_X 97` **原地不动**
放弃：整块一起右移（数字会顶到右边框 126）；或动标题左沿（会破坏与箭头 x=8 的对齐）
因为：用户 m09335 说图标跟 HAIXINBEI "挨得太近了"（墨迹只差 2px）；而往左移会更挤（标题已到 x=71，右边框也只剩 6px），所以把图标右移 4px：标题→图标 2->6px，图标偏点→数字仍留 6px（≥一格，用户 m09248 第 2 条）

## 2026-10-02 — 循迹页（8 路柱状图那页）自己读五向键**原始位图**，不靠框架的三键归一化（`trace_key_scan()`，20ms 消抖、只取"新按下"边沿）
选择：循迹页（8 路柱状图那页）自己读五向键**原始位图**，不靠框架的三键归一化（`trace_key_scan()`，20ms 消抖、只取"新按下"边沿）
放弃：在 `KK_UI_CustomOnInput` 里用 UP/DOWN 区分 —— 做不到，同一个 DOWN 事件既代表右键又代表下键
因为：框架 `firmware/App/Src/app_ui.c:513-538` 把 上/左 折成 UP、下/右 折成 DOWN，分不出左右；用户 m09335 要"右键切另一套标定"与"下键换视图"并存在同一页上

## 2026-10-02 — 循迹页**中键直接进标定**（写进当前预览那套），跳过原来那页「选标定套」；切套改由该页**右键**完成（立刻生效 + 写 Flash + 弹"保存成功"）
选择：循迹页**中键直接进标定**（写进当前预览那套），跳过原来那页「选标定套」；切套改由该页**右键**完成（立刻生效 + 写 Flash + 弹"保存成功"）
放弃：保留"中键→选套页→中键→标定"两步走 —— 用户明确要中键直接进标定，两步是多余的一次按键
因为：用户 m09335："进到那个界面之后就是普通的，按右键就可以进入到这个亚克力的""在这几个界面按中键都是进入标定"；原来只能选套页切套，柱状图页看不到另一套

## 2026-10-02 — 「选标定套」那页（`TRACE_CAL_PICK`、`trace_cal_wizard_pick()`）**代码保留但没有入口**，不再从任何按键路径进入
选择：「选标定套」那页（`TRACE_CAL_PICK`、`trace_cal_wizard_pick()`）**代码保留但没有入口**，不再从任何按键路径进入
放弃：连同状态机一起删干净 —— 收益只是个死分支，不值得动已验证过的标定状态机
因为：改动只剩一处（中键分支），万一用户还想要两步走，改回一行即可；直接删要动 `firmware/App/Svc/trace_cal.c` 的状态机与 cancel 特例，风险大于收益

## 2026-10-02 — 主页电量块第 1 行**固定左沿** `BAT_X0=74`（不再按百分比宽度算 x0）；剩余时间行以 `(BAT_X0+BAT_X_END-w)/2` **居中在那块下面**
选择：主页电量块第 1 行**固定左沿** `BAT_X0=74`（不再按百分比宽度算 x0）；剩余时间行以 `(BAT_X0+BAT_X_END-w)/2` **居中在那块下面**
因为：128px 宽屏上标题（HAIXINBEI 64px，x=8..71）已经占掉一半，按宽度右对齐的话图标会跟着百分比位数左右跳（58%↔100% 差 6px）；固定 x0=74 + `%3u%%` 固定 3 字符宽（` 58%`/`100%`，前导空格不可见）让图标永远不动

## 2026-10-02 — 主页分隔线改成**只垫在标题底下**（x=8..71，即标题的 64px 宽）并且 **2px 粗**
选择：主页分隔线改成**只垫在标题底下**（x=8..71，即标题的 64px 宽）并且 **2px 粗**
因为：用户 m09248 第 4 条：„把分隔线只放在 HAIXINBEI 下面“——两列顶栏下垫一条整宽线会把右侧电量也框进去

## 2026-10-02 — 剩余时间格式带单位：`<60` 用 `45min`、`≥60` 用 `16H39M`（分钟也带 M）
选择：剩余时间格式带单位：`<60` 用 `45min`、`≥60` 用 `16H39M`（分钟也带 M）
因为：用户 m09248 第 3 条：„最后一个没有单位，而且特别靠右，应该稍微靠左一点“

## 2026-10-02 — 主页电量块的百分比/剩余时间都**右对齐到内容右边界 x=119**（而不是各自居中）
选择：主页电量块的百分比/剩余时间都**右对齐到内容右边界 x=119**（而不是各自居中）
因为：左边那列是标题，两列各自贴同一条内容边距看着才齐；居中的话两行宽度不同、右边缘会参差

## 2026-10-02 — 主页内容左右边距固定 **8px**，左右箭头内收到 x=8..15 / 112..119 与标题/电量块共用同一条边距
选择：主页内容左右边距固定 **8px**，左右箭头内收到 x=8..15 / 112..119 与标题/电量块共用同一条边距
因为：用户 m09221 说旧版「有些太靠两边」——箭头原来在 5..12/115..122，离圆角外框只有 2~3px

## 2026-10-02 — 剩余时间只用纯 ASCII 短写法（`45min` / `2H05` / `--`），不再写「约45分」「>99分」
选择：剩余时间只用纯 ASCII 短写法（`45min` / `2H05` / `--`），不再写「约45分」「>99分」
因为：用户 m09119 明确要求「直接写一个时间，不用写那么多」；`BAT_RUN_MAX` 的 99 分上限也就随之废弃（改成小时格式后 999 分能直接显示成 16H39）

## 2026-10-02 — 全黑帧（`whiteish == 0`：大黑块/路口/整车摆在全黑上）不用冻结的 ŝ，改用中性 0：`firmware/App/Svc/trace_cal.c` 的 `frame_compute_body()` 里 `shift = s_shift;` 之后加 `if (whiteish == 0U) shift = 0;`
选择：全黑帧（`whiteish == 0`：大黑块/路口/整车摆在全黑上）不用冻结的 ŝ，改用中性 0：`firmware/App/Svc/trace_cal.c` 的 `frame_compute_body()` 里 `shift = s_shift;` 之后加 `if (whiteish == 0U) shift = 0;`
放弃：继续用冻结的 ŝ（原行为）/ 给 ŝ 加幅值上限（会反过来让白路看起来发黑）/ 从源头不冻结 ŝ（无白参考时能算出什么并不确定）
因为：用户 m08812 第 2 点：全黑时第 1、8 路明显比中间低、第 8 路有时掉成 0（时好时坏）。实测：ŝ 是压在黑线边沿那几帧算出的 −58，按**原值计数**从每路「基线−原值」里减，而黑度是按**各路自己的跨度**归一化的 ⇒ 跨度只剩 85 的第 8 路被白扣 58×255/85≈174 个黑度

## 2026-10-02 — 电池百分比改用公开的锂电池静止电压-SoC 表（Battery Skills 单节表 ×3 = 21 点，5% 一档），换掉原来手写的 10 点线性表
选择：电池百分比改用公开的锂电池静止电压-SoC 表（Battery Skills 单节表 ×3 = 21 点，5% 一档），换掉原来手写的 10 点线性表
放弃：线性 9.0~12.6V 映射（第一版做法，已弃）
因为：用户 m08812 明确要求「电压→百分比不要线性，要用网上已有的固定结论」；3S 锂电在 3.7~3.9V 平台段电压变化极小、线性插值误差最大

## 2026-10-02 — 剩余时间做成保守估计：容量按 BAT_PACK_MAH 4400mAh × BAT_USABLE_PCT 80% 折算、电流用 1/32 低通（1Hz 采样 ⇒ τ≈32s）、结果向下取整到 5 分钟、电流 <50mA 或读不到就显示 `--`、超过 99 分显示 `>99分`
选择：剩余时间做成保守估计：容量按 BAT_PACK_MAH 4400mAh × BAT_USABLE_PCT 80% 折算、电流用 1/32 低通（1Hz 采样 ⇒ τ≈32s）、结果向下取整到 5 分钟、电流 <50mA 或读不到就显示 `--`、超过 99 分显示 `>99分`
放弃：按瞬时电流直算 / 按标称 4400mAh 全额算（都会高估）
因为：用户 m08812 要求「预计还能跑多久，一定要保守」；实测台架空载只有 107mA（若按此算能跑十几小时），保守折算是防止给用户虚假安心

## 2026-10-02 — 主页改成四行：电池图标+百分比整块水平居中（y=2）、下面一行剩余时间（y=16）、HAIXINBEI 居中（y=29，kk_font_home）、开始游戏+左右箭头（y=48）；删掉 TERRAIN ROBOT 那行和细分隔线
选择：主页改成四行：电池图标+百分比整块水平居中（y=2）、下面一行剩余时间（y=16）、HAIXINBEI 居中（y=29，kk_font_home）、开始游戏+左右箭头（y=48）；删掉 TERRAIN ROBOT 那行和细分隔线
放弃：保留 TERRAIN ROBOT 并把电量压到角上（那就是用户抱怨的「图标和百分比不在一起」）
因为：用户 m08812 要求电量「水平居中、上移一点、下面加剩余时间」；kk_font_body 12px 字在 64px 高的屏上只放得下 3 行正文，五行塞不下

## 2026-10-02 — 选套页改成「右键立刻切套并写 Flash、左键回进来时那一套（再按一次才退出）、中键在当前套上开标定」，不再用「下键挪光标 + 中键确认」
选择：选套页改成「右键立刻切套并写 Flash、左键回进来时那一套（再按一次才退出）、中键在当前套上开标定」，不再用「下键挪光标 + 中键确认」
放弃：光标+确认两步式（第一版做法，已弃；缺点：屏幕上的候选名 ≠ 正在用的一套，容易误判默认是哪套）
因为：用户 m08812 要的是「右键→切到亚克力、再按左键→回普通、普通再按左键→退出」的手感，并且要一个「光切套、不标定」的地方；立刻切套还保证屏上显示的名字永远等于正在用的那一套，不会看错

## 2026-10-02 — 主页电池电量按 3S 锂电开路电压表换算（满 12.6V=100%、9.0V=0%，中间 10 个点分段线性插值），取 INA226 母线电压 1s 一次 + 1/8 一阶低通
选择：主页电池电量按 3S 锂电开路电压表换算（满 12.6V=100%、9.0V=0%，中间 10 个点分段线性插值），取 INA226 母线电压 1s 一次 + 1/8 一阶低通
放弃：没有做库仑积分（精确电量计）——要外接电流积分、还得处理容量衰减，这一版只要"大概还剩多少"；也没有直接用百分比 = (V-9)/(12.6-9) 的线性式（中段会虚高 10~20 个点）
因为：用户 m08669 说"十二伏 / 6 节 18650 / 满 12.6 / 没电 9 点几伏"，即 3S；锂电中段 3.6~3.9V 是平台，纯线性插值会高估；电机一冲母线瞬掉 0.3~0.8V，不滤波百分比会跟着跳

## 2026-10-02 — 主页标题「HAIXINBEI」改左对齐 x=6,y=6，电池块放右上角（图标 74..95 + 百分比右对齐到 124）
选择：主页标题「HAIXINBEI」改左对齐 x=6,y=6，电池块放右上角（图标 74..95 + 百分比右对齐到 124）
放弃：没选"电量放左上角 + 标题右移"：标题 64px + 电量块 50px = 114px 已经顶到内框（左右各留 6px 后只剩 114px），必压字
因为：kk_font_home 的 "HAIXINBEI" 宽 64px，原来居中占 x≈32..96，右上角放不下电量；左对齐后标题占 6..70，与电量块留 4px 空档。分隔线也一起从居中(54..73)改为左对齐(6..69)

## 2026-10-02 — INA226 读失败时电池显示 "---"，不留上一次的数字
选择：INA226 读失败时电池显示 "---"，不留上一次的数字
放弃：没有沿用旧值 + 变灰之类
因为：电量是给用户做决策的（还剩多少能跑），宁可显示"不知道"也不要谎报一个旧值

## 2026-10-01 — `trace_cal_get_data()` 第一行必须 `memset(out, 0, sizeof(*out))`：结构体填充字节也是 payload 的一部分
选择：`trace_cal_get_data()` 第一行必须 `memset(out, 0, sizeof(*out))`：结构体填充字节也是 payload 的一部分
放弃：只在真正用到的字段上逐条 memcpy（原写法）—— 填充字节留着调用方的栈垃圾，被 `trace_cal_data_t` 整体赋值带进 nvram payload
因为：nvram 用 `memcmp(&cur, &s_live, sizeof(cur))` 判"设置变了没有"。填充字节每次调用都不同 ⇒ 比对恒不等 ⇒ `s_change_ms` 每轮重置、**自动保存永远不触发**（2026-10-01 实测：`s_live` 两次只差偏移 77 一个字节，就是 `bad[8]`→`blk[8]` 之间的那个填充字节）

## 2026-10-01 — 选套页里【下键】只挪光标、【中键】才真切套（切套 + 写 `g_cfg.trace_cal_set` + 起标定向导）；【上键】退出时把 `trace_cal_valid` 还回进入前的值
选择：选套页里【下键】只挪光标、【中键】才真切套（切套 + 写 `g_cfg.trace_cal_set` + 起标定向导）；【上键】退出时把 `trace_cal_valid` 还回进入前的值
放弃：进页就切套（每挪一下光标就切一次、每次都要重起白底基线并弹"保存成功"），或退出时让这次向导作废掉正在用的标定
因为：切套会清 `s_base`/`s_shift_ok` 重新起头，光标还没定就切等于把车扔在"基线重学"状态里；而且"点进去又退出"不该废掉车上在用的那份标定（`s_valid_before` 就为这个加的）

## 2026-10-01 — 屏上"当前标定套"在标题行只画**一个单字**（普 / 亚），完整名字（普通 / 亚克力）只留在设置页「标定套」项和选套页正文里
选择：屏上"当前标定套"在标题行只画**一个单字**（普 / 亚），完整名字（普通 / 亚克力）只留在设置页「标定套」项和选套页正文里
放弃：直接在标题里拼"普通"/"亚克力"整词
因为：kk_font_body 的汉字前进宽实测 ≈11.3px（不是早先记的 8.77px），一个词的宽度会撞掉「灰度原值」的「值」和右边的「好8/8」——实屏抓图确认过压字

## 2026-10-01 — 边缘外推只在"黑带被阵边界切掉"时启用（判据=最外那一路黑度≥0.9×本帧最黑），并且只单向把位置往外推、绝不往内拉。
选择：边缘外推只在"黑带被阵边界切掉"时启用（判据=最外那一路黑度≥0.9×本帧最黑），并且只单向把位置往外推、绝不往内拉。
放弃：用编码器里程去猜线心——否决：线与车身的横向关系跟纵向里程无关，且用户 m07972 也认可不需要。
因为：黑带宽度只有在黑带被完整看到时才能量准；单向外推保证"没被切"时行为与老版逐帧重心完全一致，风险最小。

## 2026-10-01 — 位置滤波用 3 帧中值，不用一阶低通。
选择：位置滤波用 3 帧中值，不用一阶低通。
放弃：一阶低通（α=0.25/0.5）——否决：同样的抑噪要付 40~80ms 相位代价。
因为：中值只滞后 1 帧（20ms），而低通 α=0.5 要滞后 2~4 帧（40~80ms）；循迹是外环，高速过弯的滞后直接变成"迟一步才转"。台架实测中值已能压掉 63% 帧间跳变，且中值天然能丢单帧脉冲。

## 2026-10-01 — 界面缺字靠重新生成字库补字形，而不是换用别的词。
选择：界面缺字靠重新生成字库补字形，而不是换用别的词。
放弃：把"亚克力/普通/标准/切换/两套"换成有字模的替代词——否决：语义变差，且以后每加一个界面词都要再查一次字库。
因为：用户 m07972 明确要求名称贴切原意（"不能说你要改别的名字"），而生成字库只是增字模、不改代码逻辑。

## 2026-10-01 — 加权重心加“去零偏”：每路黑度先减掉本帧的噪声地板（ped = noise，标定抖动折算出来的），再归一化
选择：加权重心加“去零偏”：每路黑度先减掉本帧的噪声地板（ped = noise，标定抖动折算出来的），再归一化
放弃：我先前写的一版“取本帧最小黑度当零点 + 按对比度拉伸”被废弃：每路黑度在负值处已夹到 0（trace_cal.c ② 里 lvl<0 → 0），所以 min 结构上永远是 0（实测 ped=0，无效）
因为：用户 m07888 第 2 条明确要加；低于噪声的黑度跟 0 分不出来，加进重心就是白路上的假权重

## 2026-10-01 — 标定存两套（普通 / 亚克力）：循迹页按上/下进子页选套，分别存储，运行时可切换
选择：标定存两套（普通 / 亚克力）：循迹页按上/下进子页选套，分别存储，运行时可切换
放弃：只做一套标定（现状，亚克力段只能靠单帧自适应硬扛）
因为：用户 m07888 第 3 点自己定的方案（亚克力段模拟值会乱，一份固定阈值不够用）

## 2026-10-01 — 自适应的运行参数（ŝ 限速、扰动判定、基线漂移半径/容差）改成从标定的每路跨度与抖动算出来，不再写死
选择：自适应的运行参数（ŝ 限速、扰动判定、基线漂移半径/容差）改成从标定的每路跨度与抖动算出来，不再写死
放弃：继续用写死的 60/16/60（旧版）——保留成“没标定时的保守默认值”兼降级路径
因为：用户 m07689 明确要求“参数靠标定来计算”；标定测出的 span/jit 就是这套传感器自己的尺子，比拍脑袋的常数更贴车（手特别稳时用 3×抖动当地板又不会太紧）

## 2026-10-01 — 每帧“原值->黑度”改用标定时算好的定点倒数 s_rq=(255<<12)/跨度，只剩乘+移位
选择：每帧“原值->黑度”改用标定时算好的定点倒数 s_rq=(255<<12)/跨度，只剩乘+移位
放弃：每帧现算 16 条除法（旧版）——能跑但没证据、也不随标定自适应
因为：用户 m07689 同时要求实时性；Cortex-M4 上一条整数除法 2~12 周期，8 路×2 处 = 每 20ms 16 条；实测整帧 2377 周期/14.0µs（峰值 2940/17.3µs）= 主循环 0.07%

## 2026-10-01 — frame_compute() 外面套一层 DWT 计时壳，暴露 trace_frame_cycles / _max 两个地址
选择：frame_compute() 外面套一层 DWT 计时壳，暴露 trace_frame_cycles / _max 两个地址
放弃：不计数（旧版无从查证）
因为：用户 m07689 要求“实时性不能太低”——现在这是被量化、可复现的（读两个地址就有数），而不是口头声称

## 2026-10-01 — 白底基线学习必须同时有"本帧相对白"+"相对标定白值的绝对锚"两道闸门，外加 ±跨度/4 的硬夹子
选择：白底基线学习必须同时有"本帧相对白"+"相对标定白值的绝对锚"两道闸门，外加 ±跨度/4 的硬夹子
放弃：只用一条本帧相对闸门 + 不夹基线（旧版）——已证伪，会把真实的部分覆盖当漂移学掉
因为：只用本帧相对判据（lvl*4<lvl_max）时，半压线的邻路黑度是 0.3~0.5 倍、仍然过闸，基线每帧往下挪→黑度更小→继续学，正反馈几十秒就把白底学到黑线上（m07626 实测 ch3 学掉 86% 跨度）

## 2026-10-01 — 黑线裙边上的部分权重（如 ch3=126/255）是真实信息，不能当假权重消掉
选择：黑线裙边上的部分权重（如 ch3=126/255）是真实信息，不能当假权重消掉
放弃：上一版把重心"修正"到 4.515 的做法——那是过拟合到"重心应该 4.5"的错觉
因为：车停在线上那一帧的 raw/profile 是 36%/88%/80%/27% 的梯形，和 3.8cm 线压在 8 路（≈7cm）上的光学形状一致；把它学掉会让"3~4 路压线"退化成"只认一路"（m07626）

## 2026-10-01 — 单帧自适应的整体偏置 ŝ 从「白底路残差的最大值」改成「中位数」
选择：单帧自适应的整体偏置 ŝ 从「白底路残差的最大值」改成「中位数」
放弃：保留最大值 + 事后剔除离群点（要额外判据、还得定阈值）；或改成平均值（暗路残差会把它拉低）
因为：用户 m07343 报「轻轻一晃就都变了」：最大值会被某一路的尖峰（传感器离地/反光）单独拉走，整帧参考点跟着跳、8 路黑度一起动、位置被甩出去；中位数把单路乃至半数离群点直接扔掉，而基线跟得上以后白路残差本来就都在 ±2 个计数内

## 2026-10-01 — ŝ 限速 16/帧 + 观测中位数离当前 ŝ 超过 60 的那一帧判扰动（jolt=1 → WEAK 丢线）
选择：ŝ 限速 16/帧 + 观测中位数离当前 ŝ 超过 60 的那一帧判扰动（jolt=1 → WEAK 丢线）
放弃：一次性采用新 ŝ（大跳变会把整帧甩到错位置）；或完全不清零、硬跟（恢复要几十秒，正是用户说的「需要等好久」）
因为：大跳变时宁可丢几帧线也不给错的权重：恢复帧数 ≈ (跳变−60)/16，200 计数 ≈ 9 帧 ≈ 0.2s 自动回 OK（实测 40ms 读到 jolt=1/WEAK、1.2s 后 state=OK）；小扰动 ≤60 不丢帧，按限速平滑过去

## 2026-10-01 — 自适应开关放设置菜单（名字「自动判线」，开/关，掉电保存，nvram 记录升 v3）
选择：自适应开关放设置菜单（名字「自动判线」，开/关，掉电保存，nvram 记录升 v3）
放弃：只留 SWD 写 `trace_adapt_enable` 的 A/B 开关（用户 m07343 明确要菜单开关）；名字用「循迹自适应」（字库缺「适」，会静默不画）
因为：用户要能在车上自己开关；字库是子集，只有「判/跟/能/自/动」有字形。文档名字必须查 `tools/font/check_charset.mjs`

## 2026-10-01 — nvram 老记录兼容改成「按 rec->length 用 offsetof 判断字段是否在记录里，缺的取当前默认值」，版本 1..3 都能读
选择：nvram 老记录兼容改成「按 rec->length 用 offsetof 判断字段是否在记录里，缺的取当前默认值」，版本 1..3 都能读
放弃：原来的「v1 专用分支」硬编码把两个前馈字段无条件填默认值
因为：v2 记录里已经有前馈两项，无条件覆盖会把用户调好的前馈吃掉；offsetof 判据以后再加字段不用改这段。实测：带 v2 记录启动 → 新字段取默认 1；写 0 后自动存成 v3、复位后仍是 0

## 2026-10-01 — 改标定（向导完成 / Flash 恢复）和切「自动判线」开关时，都把白底基线 `s_base_ok`/`s_shift_ok` 清零重新起头
选择：改标定（向导完成 / Flash 恢复）和切「自动判线」开关时，都把白底基线 `s_base_ok`/`s_shift_ok` 清零重新起头
放弃：让基线按 1/32 慢慢爬到新白值
因为：原来漏了这一步（只有上电第一次起头）⇒ 重标之后偏 50 个计数要约 30s 才正常，用户会觉得「标了不管用」——这正是 m07343「需要等好久」的另一半原因

## 2026-10-01 — 循迹判线从「固定中点阈值」改成**单帧自适应**：每帧用 8 路模拟量自己算一个全局偏移 ŝ（取白色路的 max(raw−基线)）+ 按本帧最黑值归一化到 255，0/1 分界恒为 128；再叠一层**每路白底基线跟踪**（τ≈0.64s，只在「本帧有真白点」且该路黑度不到半格时学）。标定值降级为「开局初值 + 极性/跨度」。
选择：循迹判线从「固定中点阈值」改成**单帧自适应**：每帧用 8 路模拟量自己算一个全局偏移 ŝ（取白色路的 max(raw−基线)）+ 按本帧最黑值归一化到 255，0/1 分界恒为 128；再叠一层**每路白底基线跟踪**（τ≈0.64s，只在「本帧有真白点」且该路黑度不到半格时学）。标定值降级为「开局初值 + 极性/跨度」。
放弃：①纯标定阈值（亚克力段读数乱、每路漂移无解，用户 m06871 也明确不想用模块自带的固定阈值）；②每帧直接用本帧 min/max 归一化（每路白底差 63 个计数、min/max 被单路噪声拉着跑）；③只在标定时算一次归一化（漂移不跟，就是原方案）。
因为：每路白值各自漂移：实测同一帧里 ch1 比标定白值高 22、ch3 低 52（ch3 的白值 362 是各路最大，亚克力/路面变化后只读到 310）。一个全局 ŝ 补不了「每路不一样」，导致物理白底的 ch3/ch6 拿到 126/107 的假黑度，加权重心 4.618 而不是 4.5（≈1.2° 常驻偏差）且随路面游走。

## 2026-10-01 — 屏上「灰度原值」检验视图的黑度/位置、`trace_cal_channel()` 给出的 0/1、循迹页柱子分界线三处**共用同一份本帧归一化黑度**（`s_fl_lvl`），柱子的分界线恒画在半高 128。
选择：屏上「灰度原值」检验视图的黑度/位置、`trace_cal_channel()` 给出的 0/1、循迹页柱子分界线三处**共用同一份本帧归一化黑度**（`s_fl_lvl`），柱子的分界线恒画在半高 128。
因为：看到眼里的数和 PID 实际用的数必须是同一个，否则「屏上看着好、车却跑飞」没法查；自适应之后如果分界线还画在旧的 `trace_cal_thr_level()` 上，界面和判线就会互相矛盾。

## 2026-10-01 — 灰度标定不再「一按就收」：按下瞬间同步采 32 帧，单点抖动 > TRACE_POINT_JIT_MAX(80) 就整点作废、状态留在原档，标题提示「不稳!重按」；结束时逐路给原因码（跨度太小/抖动太大/未标定）而不是静默接受坏数据。
选择：灰度标定不再「一按就收」：按下瞬间同步采 32 帧，单点抖动 > TRACE_POINT_JIT_MAX(80) 就整点作废、状态留在原档，标题提示「不稳!重按」；结束时逐路给原因码（跨度太小/抖动太大/未标定）而不是静默接受坏数据。
放弃：沿用原来的「按键即收下该点」：省事，但坏数据会被写进 nvram 且 trace_cal_valid=1，用户无从判断这次标定可不可信。
因为：用户 m05894 要求「标定流程我要检验一下」——静默接受坏阈值会让循迹车在赛场上以错误阈值跑，而现场看不出问题出在标定。

## 2026-10-01 — 界面文案改写以避开字体子集缺字：`现%u~%u 跨%u`→`差%u`、`抖!重按`→`不稳!重按`、`跨度太小`→`黑白太近`。
选择：界面文案改写以避开字体子集缺字：`现%u~%u 跨%u`→`差%u`、`抖!重按`→`不稳!重按`、`跨度太小`→`黑白太近`。
放弃：重新生成含这三个字的字模并集成：能保住原措辞，但要联网、要动字体 blob 与集成流程，收益只是一个提示串。
因为：kk_font_body 是子集字模，跨 U+8DE8 / 小 U+5C0F / 抖 U+6296 三个字不在子集里，KK_OLED 会静默跳过不画（实屏「标定失败 度太」）；重新生成完整字模要联网走 LEDFont API，代价大。

## 2026-10-01 — 远程检验界面只用「只改显示状态」的 pokes（写 trace_cal s_state / s_retry_ms / trace_cal_mode 再抓显存），绝不远程代按中键跑真标定。
选择：远程检验界面只用「只改显示状态」的 pokes（写 trace_cal s_state / s_retry_ms / trace_cal_mode 再抓显存），绝不远程代按中键跑真标定。
放弃：远程触发一次完整标定向导：能验到全链路，但会污染设备的标定结果。
因为：真跑一次标定会把 trace_cal_valid 置 1 并把垃圾阈值写进 nvram，用户回头还得重标；而状态提示是纯显示。

## 2026-10-01 — 「不稳!重按」这条提示串的远程取证就此打住，改为让用户现场按键/故意手抖确认。
选择：「不稳!重按」这条提示串的远程取证就此打住，改为让用户现场按键/故意手抖确认。
放弃：继续深挖（怀疑 halt 打断 OLED/I2C 提交导致抓到旧帧）：一条装饰提示不值当，已耗掉大量时间。
因为：字符串在 ELF 里、draw_trace_page 反汇编确实调 trace_cal_retry_hint()、判据表达式也与源码逐条对上，但远程拨状态时屏上始终是「全黑按中键」（4 个 SWD 陷阱都排除过）。它只是装饰性提示，不影响任何功能。

## 2026-10-01 — 角度环默认律换回 s_law=0（连续位置 PID + damp·Gyro_Z），Ki 默认 0，s_corr_limit 由 4000 收紧到 2000，到位锁存改成各律共享（带内 150ms / 带外 4° 内 900ms 两级判据）
选择：角度环默认律换回 s_law=0（连续位置 PID + damp·Gyro_Z），Ki 默认 0，s_corr_limit 由 4000 收紧到 2000，到位锁存改成各律共享（带内 150ms / 带外 4° 内 900ms 两级判据）
放弃：保留 s_law=3/s_law=2 做默认（分档命令是抖动根源）；Ki 保持 10.8 补死区（车停在带外一直抽）；s_corr_limit 保持 4000（Δ60 峰值 245°/s 冲过头）；允速下限 + 减速前馈（round 1 已全部被数据否掉）
因为：用户 m05505「继续优化」要消除运动段「一卡一卡」。用新写的判据器 out/_metric.ps1 同尺度对比三支律：law 2/3 的速率环命令天生是分档/继电器式（CV 118~127%、一拍突变>30°/s 8~13 次），law 0 命令连续（CV 47%、停顿 0、变号 0、maxΔcorr 571）。Ki=10.8 会让车悬在 2.08~2.37° 锁不上（积分被静摩擦死区喂成常驻推力），改 0 后 Δ60 末点 0.06°。CL 低于 ~1700 时连静摩擦都顶不开（速度环 FF = 0.662·target ± 2327，3450 才起转）

## 2026-10-01 — 角度环 Ki 默认值从 10.8 改为 0，并对旧存档做一次性迁移（gyro_ki_x100 == 1080 → 0）
选择：角度环 Ki 默认值从 10.8 改为 0，并对旧存档做一次性迁移（gyro_ki_x100 == 1080 → 0）
放弃：只改 fallback 不改存档迁移（老板上写死的 1080 会一直生效）
因为：Ki 非 0 时车会悬在停车带外 0.1~0.4° 处反复抽（req ±205/±246），永远进不了 2° 锁存带；迁移必须在 push_pid_params() 之前跑，否则存档里的 1080 会重新推回参数表

## 2026-10-01 — 角度环默认律定为 s_law=3 = law 2「连续速率环」原样 + 到位锁存；不用√刹车曲线、不用允许速度下限、不用加速度前馈。
选择：角度环默认律定为 s_law=3 = law 2「连续速率环」原样 + 到位锁存；不用√刹车曲线、不用允许速度下限、不用加速度前馈。
放弃：①√刹车曲线 + 满权限反刹继电器（超调 1° 但巡航段继电器抖 13 次/拍突变、最终停在 err 7.21°）；②减速度前馈 s_kacc·Δv_ref/dt（±23°、174°/s 极限环）；③允许速度下限 35°/30°（超调 5°、±2~4° 永不锁存）；④把停车带放宽到 3° 换取锁存（尾段仍然摆）。
因为：台架实测：小车小角度下的有效偏航刹车能力只有 ~150~300°/s²（编码器 mm/s 已到 ±150~200 而车身只转 40°/s ⇒ 轮胎在打滑），要停在 3° 内进场角速度必须 < √(2·a·3) ≈ 30°/s，而 30°/s 本身就在 stick-slip 区 —— 「高速进场 + 一次性刹住」物理上做不到，任何给下限/给前馈的方案都会变成 ±2~23° 的自持振荡。能收敛的只有「慢慢爬 + 静摩擦把车钉住」。而「到点之后一直小幅晃」不是车在动（尾段 Yaw 峰峰只有 0.02°），是死区门 |v_f|<s_ff_rate 在边界抖动导致电机每隔几百 ms 被点一下（尾段 CCR 仍有 0/2760/0/2708 脉冲），所以用一个到位锁存彻底松手就解决了。

## 2026-10-01 — 角度环默认控制律换成 s_law=2「外环角度→速率、内环速率比例 + 静摩擦起转助推」，原 150/15 PID 保留为 s_law=0 对照
选择：角度环默认控制律换成 s_law=2「外环角度→速率、内环速率比例 + 静摩擦起转助推」，原 150/15 PID 保留为 s_law=0 对照
放弃：继续手调 PID 参数 —— 实测 720 自激、150/15 也只是勉强稳定且 20° 超调 4~10°
因为：原地转的 plant 是死区+滞回：起转要 ≈3450（corr 口径）、滚起来只要 ≈1000，线性 PID 在这个非线性上必然推得动就刹不住

## 2026-10-01 — s_law=2 里去掉积分与摩擦前馈，只留 速率比例 + 起转助推 + 2° 到位死区 + 助推 250ms 冷却
选择：s_law=2 里去掉积分与摩擦前馈，只留 速率比例 + 起转助推 + 2° 到位死区 + 助推 250ms 冷却
放弃：摩擦前馈（按运动方向补 1300）—— 它把轮子一直推着滚，比例项压不到命令速率上，实测 0.35s/±4° 极限环；瞬时顶格助推 —— 每次过零都被重新点着，尾部 0.5s 周期来回摆
因为：四项分工：比例项负责推得动/刹得住；助推负责破静摩擦；死区负责停得安静；冷却负责不在过零处反复点火

## 2026-10-01 — 速度环目标斜坡默认 20000 只在循迹/任务路径保持，gyro 模式（Motion_SetMode 里 MOTION_GYRO/MOTION_TRACE_GYRO）写 100000
选择：速度环目标斜坡默认 20000 只在循迹/任务路径保持，gyro 模式（Motion_SetMode 里 MOTION_GYRO/MOTION_TRACE_GYRO）写 100000
放弃：全局把 SPEED_RAMP_DEFAULT 提到 100000 —— 循迹/任务起步会失去软启动
因为：角度环的修正是直接顶到左右轮目标上的，斜坡让 +4000→−4000 拖 400ms、刹车指令晚到 200ms 以上

## 2026-10-01 — Gyro_Z 改取 0x52 包的 RxBuffer[6..7]（原取 4..5）
选择：Gyro_Z 改取 0x52 包的 RxBuffer[6..7]（原取 4..5）
放弃：以为模块装歪/量程不对 —— 两种包字段序不一致才是真因，量程 ±2000 与符号都是对的
因为：抓原始包与 dYaw/dt 对比：4..5 是 wy 俯仰角速度（实测 40.89°/s，与固件旧读数逐位相同），6..7 是 wz（161.38°/s ≈ 同时刻 dYaw/dt 171.7°/s）；0x53 包的字段序又是另一套（2-3=Roll、4-5=Pitch、6-7=Yaw）

## 2026-09-30 — 角度环默认值改为 Kp=150 / Ki=10.8 / Kd=15（不再沿用旧板 ×7.2 换算的 720/72），并在 `app_ui_settings_loaded()` 里加旧值(72000/7200)→新值(15000/1500)的一次性存档迁移。
选择：角度环默认值改为 Kp=150 / Ki=10.8 / Kd=15（不再沿用旧板 ×7.2 换算的 720/72），并在 `app_ui_settings_loaded()` 里加旧值(72000/7200)→新值(15000/1500)的一次性存档迁移。
放弃：否决：① 保留 720/72 只靠 `GYRO_CORR_LIMIT` 限幅（限幅只是把振幅夹住，振荡频率/幅度仍不可用）；② 改用 `Gyro_Z` 替换 `(err-last_error)/dt` 的差分微分（怀疑是噪声放大器，但限幅后比对显示 Kd≈Kp/10 是有效的，先不动控制律）；③ 只在 RAM 里调参不落盘（用户要求「调好」，重上电就丢不算好）。
因为：原地台架实测：限幅在位时 Kp=720/Ki=10.8/Kd=72 在 Δ=3°/6°/10°/20° 全部振荡（尾晃 22~38°、峰值 400~600°/s、四路 PWM 顶在 ±4200 的饱和极限环），加 `GYRO_CORR_LIMIT 2500` 只限幅不能稳；而 Kp≤250 全稳、150/15 精度最好（3° 阶跃 0.44s、误差 0.6°、尾晃 0.04°；6×15° 转累计误差 0.4°）。机理：外环输出是轮速目标，plant 增益 k_v≈0.06 (°/s)/PWM、滞后 τ≈30~50ms ⇒ 环路增益 Kp·k_v=43/s（720）远超 1/τ，而 150 → 9/s。

## 2026-09-30 — 把迁移写进 `app_ui_settings_loaded()` 开头、`push_pid_params()` 之前，条件为 `g_cfg.gyro_kp_x100 == 72000 && g_cfg.gyro_kd_x100 == 7200`（只认旧默认值这一对，用户自己调过的值不动）。
选择：把迁移写进 `app_ui_settings_loaded()` 开头、`push_pid_params()` 之前，条件为 `g_cfg.gyro_kp_x100 == 72000 && g_cfg.gyro_kd_x100 == 7200`（只认旧默认值这一对，用户自己调过的值不动）。
放弃：否决：① 改 `nvram.c` 的版本号/结构体版本做通用迁移（动存档格式风险大，收益只是这一次）；② 让用户自己在上位机把两项改一遍（用户明确说「所有内容你都可以调」，且他板上存档还是旧值）。
因为：`make flash` 默认只擦镜像覆盖的扇区、`0x0801F800` 的 NVRAM 设置页原样保留（`tools/flash.ps1` 头部说明 + 本次实测：迁移前的旧值存档 72000/7200 在烧录后依然存在并触发迁移），所以「改编译默认值」对已有存档无效，必须显式迁移。

## 2026-09-30 — 角度环原地测试只用 SWD 改 RAM（s_enabled/s_req/gyro_control_enabled/target_angle/s_param[13..15]），base_speed_left/right 保持 0，不改固件、不烧录
选择：角度环原地测试只用 SWD 改 RAM（s_enabled/s_req/gyro_control_enabled/target_angle/s_param[13..15]），base_speed_left/right 保持 0，不改固件、不烧录
放弃：否决为测试在固件里加临时入口或烧一版测试固件（要动源码、要烧录、每次改参都要重编重烧）
因为：用户 m04020 明确「你只能原地的转圈,不能前进和后退,因为我上面插的还有线」；而 motion.c 的 MOTION_IDLE 分支里 GyroControl_Task() 用 base=0 时左右轮只做差速修正，结构上不可能前进后退，正是要的工况

## 2026-09-30 — 不再用调试器手写 FLASH_CR/FLASH_ACR 去做页擦除试验；要验证擦/写只能用 `make flash` 或应用自己的 `nvram_page_erase()`，判断成败只读 FLASH->SR 与回读内容。
选择：不再用调试器手写 FLASH_CR/FLASH_ACR 去做页擦除试验；要验证擦/写只能用 `make flash` 或应用自己的 `nvram_page_erase()`，判断成败只读 FLASH->SR 与回读内容。
放弃：继续手工扫描 PNB/BKER 组合（已经扫过 20 组，一组都没擦掉）——除了继续破坏控制器状态没有新信息。
因为：2026-09-30 审计时反复手写 CR/ACR 之后，这颗 G474 的擦除与编程对调试器和应用全部失败（FLASH->SR=0xA0=PGAERR|PGSERR），紧接着 SWD 完全失联，只能断电恢复。

## 2026-09-30 — flash.ps1 在 J-Link 编程后追加 `mem32 0x40022010, 1` 读 FLASH->SR，非 0 错误标志（掩码 0xC37A）就打印警告。
选择：flash.ps1 在 J-Link 编程后追加 `mem32 0x40022010, 1` 读 FLASH->SR，非 0 错误标志（掩码 0xC37A）就打印警告。
放弃：只靠回读比对（不够：内容本来就对时也过），或不加检查（今天这种全拒状态就无法察觉）。
因为：CubeProgrammer/J-Link 在「内容已经一样」时会打印 Download verified successfully / Skipped. Contents already match 却什么都不写，成功信息不能证明擦写在芯片上真的可行。

## 2026-09-30 — nvram.c 自己写寄存器擦页：固定 BKER=1 + PNB=31（bank2 页 31 = 0x0801F800），不用 HAL_FLASHEx_Erase()
选择：nvram.c 自己写寄存器擦页：固定 BKER=1 + PNB=31（bank2 页 31 = 0x0801F800），不用 HAL_FLASHEx_Erase()
放弃：否决 ①继续用 HAL_FLASHEx_Erase（数不清多少次只报 OK 不擦）②按 OPTR DBANK 位在 HAL_FLASH_BANK_1/2 之间分支（本机该位不可信，分支会选错 bank 并去擦 0x0800F800 的 app 代码）③改成 mass erase（会连 app 一起擦）
因为：HAL 按 FLASH_OPTR 的 DBANK 位决定 BKER，而这颗料该位读出 0（与硬件实际行为不符），HAL 的擦除请求被硬件静默忽略、且不报错，无法从返回值发现

## 2026-09-30 — 保存失败的重试退避逐次翻倍 5s→10s→20s→40s→60s 封顶，成功后复位
选择：保存失败的重试退避逐次翻倍 5s→10s→20s→40s→60s 封顶，成功后复位
放弃：否决保持固定 5s 退避（失败时提示一直弹）、也否决失败后完全不再重试（用户改的设置就永远存不进去）
因为：用户 m03645 抱怨的就是「过一会儿弹一个」，而这正是一旦写失败就每 5s 重试的提示刷屏

## 2026-09-30 — 诊断插桩用完就删干净（它们的目的已达到：定位到擦除静默失败）
选择：诊断插桩用完就删干净（它们的目的已达到：定位到擦除静默失败）
放弃：否决把 g_nvdbg 留在固件里当长期调试量（占 bss、结构上也无必要；要复现按 PROJECT.md 坑里的 J-Link 命令手写寄存器即可）
因为：2026-09-30 的工作树里带着排查用的 volatile uint32_t g_nvdbg[16] 与一圈插桩写

## 2026-09-30 — `tools/flash.ps1` 默认不显式传擦除，靠 CubeProgrammer 下载自带的「按镜像范围擦扇区」（= app 的扇区 0..47），NVRAM 页（sector 63 = 0x0801F800）不在范围内所以设置保留；要整片擦才加 `-MassErase`
选择：`tools/flash.ps1` 默认不显式传擦除，靠 CubeProgrammer 下载自带的「按镜像范围擦扇区」（= app 的扇区 0..47），NVRAM 页（sector 63 = 0x0801F800）不在范围内所以设置保留；要整片擦才加 `-MassErase`
放弃：否决 `-e [0 47]`（语法不被接受）与「整片擦 + 用备份写回 NVRAM」（多一次写 Flash 的风险且丢失中间设置）
因为：这个 CLI 不认 `-e [0 47]` 这种扇区编码语法（实测报 `Error: Sector 0032 does not exist`、返回 -1073741819），而「整片擦完再恢复设置」会真丢用户数据

## 2026-09-30 — `firmware/Makefile` 的 flash 规则改成优先 `pwsh`（`PSFLASH := $(if $(shell where pwsh),pwsh,powershell)`），`tools/flash.ps1` 保持「UTF-8 with BOM」
选择：`firmware/Makefile` 的 flash 规则改成优先 `pwsh`（`PSFLASH := $(if $(shell where pwsh),pwsh,powershell)`），`tools/flash.ps1` 保持「UTF-8 with BOM」
放弃：否决「只用 5.1 + 每次改脚本后手工补 BOM」（踩过两次）
因为：Windows PowerShell 5.1 按 GBK 读无 BOM 的 UTF-8 脚本会 `ParserError: UnexpectedToken`，而 `edit`/`write` 工具会剥掉 BOM，来回补 BOM 很脆；PS7 默认按 UTF-8 读，没这个问题

## 2026-09-30 — 「电机测试」页滑条改成**闭环速度目标**：`app_ui.c` 的 `motor_test_apply()` 走 `SpeedControl_Enable(1U) + SpeedControl_SetTarget(tgt[0], tgt[1])`，不再自己 `Load()`
选择：「电机测试」页滑条改成**闭环速度目标**：`app_ui.c` 的 `motor_test_apply()` 走 `SpeedControl_Enable(1U) + SpeedControl_SetTarget(tgt[0], tgt[1])`，不再自己 `Load()`
放弃：保留开环（滑条=占空比油门）。否决理由：量纲不同，屏上两个数字永远差一截（用户 m03156 抱怨「编码器一直差个 20」就是它），怎么调都对不上
因为：用户 m03156 问「这个显示的 0~100 是带着速度环的目标值吗？为什么左边编码器一直跟不上」，m03300 在选项里选了「改成闭环速度目标（推荐）」；知情代价：退出页面后环仍以该目标跑、再看电流时电流不再等于给定占空比

## 2026-09-30 — 滑条划到两路 0（含按「清零」）走 `SpeedControl_Stop()`，而不是 `SetTarget(0,0)`
选择：滑条划到两路 0（含按「清零」）走 `SpeedControl_Stop()`，而不是 `SetTarget(0,0)`
放弃：只把目标置 0 并保持环使能
因为：`speed_control.c` 的积分不随目标清零，带着热积分按 `ki·∫err` 最多 ±6000 PWM 反向踹一脚；`SPEED_FF_FRIC_MIN_TGT 0.5f` 只挡前馈不挡积分

## 2026-09-30 — 标题行「返回」「清零」用**左/右切换选中项**（上/下仍是 4 项循环）
选择：标题行「返回」「清零」用**左/右切换选中项**（上/下仍是 4 项循环）
放弃：只能靠上/下绕到清零
因为：用户 m03156：这两个按钮在一排上，从「返回」按右就该到「清零」；`motor_key_press()` 里 LEFT/RIGHT 合并成一个 case 处理

## 2026-09-30 — 电机测试页自己读五向键原始位图（`app_ui_raw_keys()` + `motor_key_scan()`，页内 20ms 消抖 + 长按加速），并显式丢弃 kk_ui 的 UP/DOWN 事件
选择：电机测试页自己读五向键原始位图（`app_ui_raw_keys()` + `motor_key_scan()`，页内 20ms 消抖 + 长按加速），并显式丢弃 kk_ui 的 UP/DOWN 事件
放弃：给 kk_ui 加第 4/5 个键（要动 keys[3]、输入掩码、三处 dispatch、pending_dir 和 host 用例）
因为：kk_ui 只有三键，`app_ui_read_input()` 把左/右折成上/下，分不出左右；而框架保证 `KK_UI_CustomOnTick()` 只在「当前页 + 无翻页过渡」时被调，所以自扫不会和翻页动画抢键

## 2026-09-30 — 速度档用 int8_t `s_motor_level[2]` 表示 -100..+100，直接映射 ±MOTOR_PWM_MAX（`level * 7200 / 100`），删掉固定 `MOTOR_TEST_PWM 6000`
选择：速度档用 int8_t `s_motor_level[2]` 表示 -100..+100，直接映射 ±MOTOR_PWM_MAX（`level * 7200 / 100`），删掉固定 `MOTOR_TEST_PWM 6000`
放弃：保留固定占空比 + 方向档（停/正/反）
因为：用户 m02868 要「中间 0、左边 -100、右边 +100，想调多少就调多少」；代价是档位 <48 时右轮只嗡嗡响不转（起转阈值实测 3400~3550）

## 2026-09-30 — 「清零」= 左右两路一起归零（停车）
选择：「清零」= 左右两路一起归零（停车）
放弃：只清当前选中那一路 / 只清实测读数
因为：用户只说「加一个清零的选项」没指定哪一路；按测试页「一键停」实现，已在回复里说明可改

## 2026-09-30 — 字模 must-have 一次给全：扫 `firmware/App` + `kk_ui` 全部 `.c/.h` 字面量的 416 个汉字 → `kk_font_body` 572 字形 / 14136 B（+3121 B flash）
选择：字模 must-have 一次给全：扫 `firmware/App` + `kk_ui` 全部 `.c/.h` 字面量的 416 个汉字 → `kk_font_body` 572 字形 / 14136 B（+3121 B flash）
放弃：只补新版面缺的「清」「零」两个字
因为：KK_OLED 用 u8g2 静态子集字模，子集外的字静默不画；逐次补字会反复返工。同时新增 `tools/font/measure_text.mjs` 量字宽排版（实测 `-100`=23px、`100`=17px、汉字 11~12px）——实测数字右对齐列因此定在 x=60 而不是 58

## 2026-09-30 — 循迹/角度环增益类参数一律按 7.2× 重标（line_kp 375→2700、line_ki 1.5→10.8、line_kd 50→360、gyro_kp 100→720、gyro_ki 1.5→10.8、gyro_kd 10→72），积分限幅不折算（line_ilimit 50、gyro_ilimit 1000 不变），循迹基准速度 3000→6480
选择：循迹/角度环增益类参数一律按 7.2× 重标（line_kp 375→2700、line_ki 1.5→10.8、line_kd 50→360、gyro_kp 100→720、gyro_ki 1.5→10.8、gyro_kd 10→72），积分限幅不折算（line_ilimit 50、gyro_ilimit 1000 不变），循迹基准速度 3000→6480
放弃：继续保持旧板原值、等用户明确确认（此前已问过用户但没答）；也否决了「只乘 PWM 相关的项但保留 base 3000」——base 3000 配放大后的 Kp 时 base-output 会掉进死区
因为：这两个环的 PID 输出是直接加在左右轮上的 PWM 偏置，旧板满量程 1000、本板 7200，照抄旧值等效增益只有 1/7.2；积分限幅的量纲是误差本身（灰度重心、度），不随 PWM 刻度变。目标是「与旧码算法/流程同步」（m02112），而不是等一个不确定的手感确认

## 2026-09-30 — HP_BASE_SPEED_SLOW 保持 2500，不按 ×7.2 折算成 1440
选择：HP_BASE_SPEED_SLOW 保持 2500，不按 ×7.2 折算成 1440
放弃：严格等比：旧 200×7.2 = 1440
因为：1440 仍低于本车静摩擦起转死区（开环实测 L≈3000、R≈3400~3550），写下去等于慢速档直接不动；2500 是死区下沿、能起步

## 2026-09-30 — 参数表一致性核对从「手抄一份固件 FW 列表」改成解析固件源码：新增 tools/host/check_param_table.py（解析 host_link.h 的 id 宏 + host_link.c 的 SPECS 默认值，与 protocol.PARAMS 逐项比并算 param_crc），并在 tools/host/tests/test_protocol.py 里加用例；删掉 out/_param_crc_check.py
选择：参数表一致性核对从「手抄一份固件 FW 列表」改成解析固件源码：新增 tools/host/check_param_table.py（解析 host_link.h 的 id 宏 + host_link.c 的 SPECS 默认值，与 protocol.PARAMS 逐项比并算 param_crc），并在 tools/host/tests/test_protocol.py 里加用例；删掉 out/_param_crc_check.py
放弃：继续维护那份手抄表（它这次没同步、对着一份正确的固件报了假 MISMATCH，反过来误导排查）
因为：手抄表本身就是确定性漂移源：改默认值必须想起它、忘了它就撒谎。解析源码的核对不会忘记

## 2026-09-30 — 陀螺仪方框的轴映射做成 8 档「位掩码」（bit0=交换轴、bit1=画面横滚取反、bit2=画面俯仰取反），而不是给 8 个 case 各写一遍
选择：陀螺仪方框的轴映射做成 8 档「位掩码」（bit0=交换轴、bit1=画面横滚取反、bit2=画面俯仰取反），而不是给 8 个 case 各写一遍
放弃：被否决：只补 4 个新 case（两轴独立取反的组合会写漏，且旧 4 档的语义已经在用户脑子里混了）
因为：两轴取反互相独立 + 是否交换轴也是独立一位，掩码天然覆盖全部 8 种组合，中键循环只要 &7U，NVRAM 也只要一个字节

## 2026-09-30 — 陀螺仪页右侧三行数字（横滚/俯仰/偏航）继续由 gyro_angles() 给，不跟着 s_gyro_axis_mode 变
选择：陀螺仪页右侧三行数字（横滚/俯仰/偏航）继续由 gyro_angles() 给，不跟着 s_gyro_axis_mode 变
放弃：被否决：让数字和方框共用同一套映射（会把用户熟悉的三行数字符号再翻一次）；副作用（默认档 M3 下数字俯仰符号与方框俯仰方向相反）已写进 固件接口规格.md §3.5 并去问用户要不要跟
因为：数字取反是用户 m01903 明确点过的读数习惯；本轮只修方框朝向，不顺手改用户已经习惯的读数

## 2026-09-30 — 陀螺仪页默认档定为 M3（s_gyro_axis_mode = 2U，画面 r=-SP、p=+SR）
选择：陀螺仪页默认档定为 M3（s_gyro_axis_mode = 2U，画面 r=-SP、p=+SR）
放弃：被否决：沿用旧默认档号（旧 M3 的语义是 r=+SR、p=+SP，与用户实测相反）
因为：用户 m02505 实测：旧 M1 的俯仰方向对（p=+SR）、旧 M2 的横滚方向对（r=-SP），把两者拼起来就是 r=-SP、p=+SR，正好是 8 档里的 M3；旧 4 档表里根本没有这一组，所以之前两次「M3 才对」只是最接近的一档

## 2026-09-30 — 把旧板两个中断节拍（TIM2 循迹 10Hz、TIM4 角度环 100Hz）合并到 TIM6 的 5ms 节拍里跑：角度环直接在 IDLE 分支调 GyroControl_Task()，后台循迹用 PID_TRACE_BG_DIV 20 分频到 10Hz
选择：把旧板两个中断节拍（TIM2 循迹 10Hz、TIM4 角度环 100Hz）合并到 TIM6 的 5ms 节拍里跑：角度环直接在 IDLE 分支调 GyroControl_Task()，后台循迹用 PID_TRACE_BG_DIV 20 分频到 10Hz
放弃：否决「给 TIM2/TIM4 在新板各开一个真定时器」：两个周期 tick 已由 bsp_periodic_start() 统一启动，再开中断只是把节拍结构改回旧板、对现有速度环/界面无收益，还要多占一对中断优先级
因为：新工程 htim2/htim4 只是 TIM6/TIM7 的别名（旧上层 extern 的名字），旧码里 PID_Tracing_Timer_Start/Stop、Gyro_Timer_Start/Stop 实际会去停 5ms 运动节拍与 TIM7 舵机缓动，而循迹/角度环一次都没跑

## 2026-09-30 — 把 renwu() 接回主循环（renwu_flag != 0 时每圈调用），并新增 renwu_stop() 在任务结束时关角度环 + Load(0,0)
选择：把 renwu() 接回主循环（renwu_flag != 0 时每圈调用），并新增 renwu_stop() 在任务结束时关角度环 + Load(0,0)
放弃：否决「继续当死代码，等上真车再说」与「用 Toggle_Gyro_Control(0,0,0) 收尾」——后者会把 saved_base_speed_* 写回电机
因为：旧板 main.c 每圈都调 renwu()，而新工程全工程没有调用点，955 行比赛流程是死代码；旧板 renwu_flag==0 时靠每圈 Load(0,0) 停车

## 2026-09-30 — 舵机动作表 Servo_Move() 的 14 个预设角度与延时逐个沿用旧板的值（两板的 Servo_Control 公式、1µs/20ms 时基完全相同），只把输出通道从旧板 TIM1_CH4/CH3 换到新板 CH1/CH2
选择：舵机动作表 Servo_Move() 的 14 个预设角度与延时逐个沿用旧板的值（两板的 Servo_Control 公式、1µs/20ms 时基完全相同），只把输出通道从旧板 TIM1_CH4/CH3 换到新板 CH1/CH2
放弃：否决「重新按新板机械结构调整角度表」：没有装车实测数据，擅自改角度只会丢掉旧板调好的动作
因为：out/servo_table_cmp.py 比对事件流逐值一致；bsp_board.h:31 已记明新板接线是 CH1/CH2

## 2026-09-30 — 新建 docs/旧码到新板移植对照.md 作为「旧码同步到什么程度」的权威清单（逐模块表 + 刻度/时基/参数对照 + 仍未同步清单），固件接口规格.md 只放 §5.1/§5.2 两份摘要
选择：新建 docs/旧码到新板移植对照.md 作为「旧码同步到什么程度」的权威清单（逐模块表 + 刻度/时基/参数对照 + 仍未同步清单），固件接口规格.md 只放 §5.1/§5.2 两份摘要
放弃：否决「全部塞进固件接口规格.md 的 §5.1」
因为：用户 m02112/m02339 明确要逐模块「旧码 vs 新码」对照说明；这类内容放在接口规格里会把那份文档撑成混合体

## 2026-09-30 — renwu.c 的旧刻度数值包一层 RW(v)=v*HP_TASK_SPEED/900，而不是把 45 处数字直接乘 7.2 写死
选择：renwu.c 的旧刻度数值包一层 RW(v)=v*HP_TASK_SPEED/900，而不是把 45 处数字直接乘 7.2 写死
放弃：直接硬编码 6480/6624/7200...（不可调、也丢掉了与旧码逐行对照的可读性）
因为：旧板 900 表达的是「90% 占空比」这个意图，本板还得按场地调；包一层后整体缩放只动一个参数 0x0304

## 2026-09-30 — HP_BASE_SPEED 1000→3000、新增 HP_TASK_SPEED 6480、HP_BASE_SPEED_SLOW 200→2500
选择：HP_BASE_SPEED 1000→3000、新增 HP_TASK_SPEED 6480、HP_BASE_SPEED_SLOW 200→2500
放弃：保持 1000/200（新刻度下只有 13.9%/2.8% 占空比，低于死区，循迹和慢速档都不动）或直接按纯换算 7200/1440
因为：旧 1000 是「满油门」，纯换算 7200 会让循迹冲出去；3000=41.7% 既够快又能拐弯，2500=死区下沿；任务流程的 900 是 90% 占空比，纯换算就是 6480

## 2026-09-30 — 把 renwu() 接回主循环（renwu_flag 非 0 时调用），并新增 renwu_stop()
选择：把 renwu() 接回主循环（renwu_flag 非 0 时调用），并新增 renwu_stop()
放弃：继续当死代码；或用 Toggle_Gyro_Control(0,0,0) 来停
因为：m02112 要求任务流程与旧码同步，旧板上 renwu() 就是主循环调的，不接永远跑不了比赛流程；Toggle(0,0,0) 会把 saved_base_speed_* 写回电机，停不干净

## 2026-09-30 — 增益类参数（line_kp/ki/kd/ilimit、gyro_kp/ki/kd/ilimit）不擅自 ×7.2
选择：增益类参数（line_kp/ki/kd/ilimit、gyro_kp/ki/kd/ilimit）不擅自 ×7.2
放弃：一次性按换算清单改默认值
因为：它们的量纲是「PWM 刻度/误差单位」，旧板按 1000 刻度标定，但新板可能已重新标定过；擅自乘会让用户在台上重调一遍。清单已写进 固件接口规格.md §5.1 等确认

## 2026-09-30 — 电机测试页占空比 MOTOR_TEST_PWM 由 4000 改成 6000（读数约 42 → 约 73）。
选择：电机测试页占空比 MOTOR_TEST_PWM 由 4000 改成 6000（读数约 42 → 约 73）。
放弃：① 直接上 7200 满速 —— 桌上测试车会窜出去、也超出「稍微快点」的诉求；② 做成菜单可调参数 —— 要动 host_link 参数表 + 上位机 + NVRAM 一大圈，用户只是嫌慢，不值当。
因为：用户 m01975 说「速度有点慢，你可以稍微快点」；6000 是实测能稳定起转（右轮阈值 3400~3550）又不到满速档，读数落在表盘 3/4 处一眼能看出刻度在动。

## 2026-09-30 — 选中行用 XOR 空心圆角框，履带图标「在转=实心 / 停=空心」。
选择：选中行用 XOR 空心圆角框，履带图标「在转=实心 / 停=空心」。
放弃：整行反白（填实 XOR）—— 视觉上吃掉图标与文字；只改文字颜色/加箭头 —— 没有可用的箭头字模。
因为：整行 XOR 填实会把描在行内的图标和文字一起反白，反而看不清哪一路在转、状态是什么；空心框 + 实心/空心履带既标出选中项又一眼看出两路状态。

## 2026-09-30 — 模块测试页与电机页显式忽略中键的 REPEAT 连发事件。
选择：模块测试页与电机页显式忽略中键的 REPEAT 连发事件。
放弃：保留连发当「按住快速切档」—— 方向档乱跳且会连带 Load() 猛启停电机。
因为：kk_ui 对按住不放的键会持续发 KK_UI_INPUT_REPEAT（kk_ui.h:58），在电机页会把方向档一路刷过去（停→正→反→停），在模块测试页会一次按住连进两级；照 splash 页的写法只认第一次 PRESS。

## 2026-09-30 — 放宽 kk_ui 的「自定义页 → 自定义页」压栈禁令（删掉 kk_ui_nav.c 原 :104-109 的 `if (KK_UI_CurrentPageType()==KK_UI_PAGE_CUSTOM && route->type==KK_UI_PAGE_CUSTOM) return KK_UI_NOT_ALLOWED;`），让模块测试页 PAGE_CUSTOM_TEST 能用中键压入电机测试页 PAGE_CUSTOM_MOTOR。
选择：放宽 kk_ui 的「自定义页 → 自定义页」压栈禁令（删掉 kk_ui_nav.c 原 :104-109 的 `if (KK_UI_CurrentPageType()==KK_UI_PAGE_CUSTOM && route->type==KK_UI_PAGE_CUSTOM) return KK_UI_NOT_ALLOWED;`），让模块测试页 PAGE_CUSTOM_TEST 能用中键压入电机测试页 PAGE_CUSTOM_MOTOR。
放弃：否决「把电机页并进模块测试页做同页两视图」（两套界面挤一个页面、状态机自己写，且没有翻页动画）；否决「先 KK_UI_CustomRequestClose() 再压栈」（过渡期间第二次压栈会返回 KK_UI_BUSY，得自己在 OnTick 里排队，视觉上还会先闪回首页）；否决「把电机页做成首页图标」（用户要的入口在模块测试页里）。
因为：用户 m01540 要的入口就是「模块测试页→电机」这个两级自定义流程；而框架本身已具备该能力——压栈时 kk_ui_start_transition() 就把旧页标记 leave_custom（kk_ui_nav.c:68-75），过渡结束回调 KK_UI_CustomOnLeave（:200-202），弹栈与页面类型无关，栈深由 KK_UI_NAV_DEPTH 管。

## 2026-09-30 — 重建 kk_font_body 字模时，must-have 用「全部界面字面量汉字的并集」（267 字，落在 out/_ui_chars.txt），而不是只补当前缺的那几个字
选择：重建 kk_font_body 字模时，must-have 用「全部界面字面量汉字的并集」（267 字，落在 out/_ui_chars.txt），而不是只补当前缺的那几个字
放弃：只补「前馈斜率/前馈摩擦」这几个字 —— 省 ~3KB flash，但下次改文案还会缺、还得再走一遍联网生成+烧录
因为：KK_OLED 用 u8g2 静态子集字模，子集外的字静默不画（不报错也不画方框），逐次补字会反复返工；实测代价只有 text 82940→86036（+3096 B），而验收判据可以自动复核（check_charset 对 267 字缺字=0）

## 2026-09-30 — 修 tools/font/gen_font.mjs：写回新字模时自动把旧文件头部的 #include 行捡回来
选择：修 tools/font/gen_font.mjs：写回新字模时自动把旧文件头部的 #include 行捡回来
放弃：每次生成后手工补 #include（已手工补过两次），或在文档里写一句提醒
因为：LEDFont API 返回的 c_source 只有数组本体，脚本原本直接覆盖整个 .c 文件 —— 丢掉 #include "kk_font.h" 后 uint8_t 未声明、编译直接报错，是个每次都会重现的确定性坑

## 2026-09-30 — 电机测试页离开时**不再**停电机（删掉 `KK_UI_CustomOnLeave()` 里那句无条件 `Load(0, 0)`），方向状态用 static 跨页保留，下次进页面由 OnEnter 重新写一遍
选择：电机测试页离开时**不再**停电机（删掉 `KK_UI_CustomOnLeave()` 里那句无条件 `Load(0, 0)`），方向状态用 static 跨页保留，下次进页面由 OnEnter 重新写一遍
放弃：退出即停（原逻辑）
因为：用户 m01540 明确要「我设置好之后退出，它还在转，我好去看电流」

## 2026-09-30 — 编码器显示口径 = 最近 20ms 窗口的计数**之和**（`SpeedControl_WindowCounts()`，0..100 里 100 = 满速，倒转为负）
选择：编码器显示口径 = 最近 20ms 窗口的计数**之和**（`SpeedControl_WindowCounts()`，0..100 里 100 = 满速，倒转为负）
放弃：累计里程表（`s_odo[2]` / `SpeedControl_Odometer()`，已加过又全部删除）
因为：用户 m01700 原话「不用一直累计计数，它就显示个实时的速度就行」；换算 4950 计数/s × 0.02s ≈ 99 ≈ 100

## 2026-09-30 — 屏上读数接速度环自己的 `s_win_sum`，界面不碰编码器硬件
选择：屏上读数接速度环自己的 `s_win_sum`，界面不碰编码器硬件
放弃：界面直接调 `Encoder_ReadDelta()` 去数 TIM3/TIM4
因为：`Encoder_ReadDelta()` 读完立刻把 TIM 计数器清零，界面拿走这一拍速度环就少一拍；`s_win_sum` 在 `if (!s_enabled) return;` 之前更新，未使能时也有值，正是本页需要的

## 2026-09-30 — 屏上写「左」「右」，不为「左轮/右轮」再生成一次字模
选择：屏上写「左」「右」，不为「左轮/右轮」再生成一次字模
放弃：跑 `tools/font/gen_font.mjs` 补一个「轮」字
因为：字模子集外只影响文案（`check_charset` 已确认其余 16 字都在），两字标签足够，避免再动一次字模文件（上回生成器丢了首行 `#include "kk_font.h"` 需手工补回）

## 2026-09-30 — 速度环前馈常数 0.662/2327 从 speed_control.c 的 static 变量提升为参数表参数（HP_SPEED_FF_SLOPE=0x0605 / HP_SPEED_FF_FRIC=0x0606），菜单「PID→速度环→前馈斜率/前馈摩擦」、上位机参数表、SpeedControl_SetFeedforward() 三个入口读写同一份值，并随 nvram 存档。
选择：速度环前馈常数 0.662/2327 从 speed_control.c 的 static 变量提升为参数表参数（HP_SPEED_FF_SLOPE=0x0605 / HP_SPEED_FF_FRIC=0x0606），菜单「PID→速度环→前馈斜率/前馈摩擦」、上位机参数表、SpeedControl_SetFeedforward() 三个入口读写同一份值，并随 nvram 存档。
放弃：保留 s_ff_slope/s_ff_fric 静态变量、只在菜单里另加一份镜像 —— 否决，两份值会不一致，且菜单改的那份不影响控制环，等于用户改了没用。
因为：用户看到菜单「PID→速度环」里还是旧值：前馈以前只存在于 speed_control.c 的 static 变量，菜单和参数表里根本没有它，所以菜单显示的永远是那四个 PI 参数（那四个确实没改过）。更要命的是「参数表里的值是控制环真在用的值」这个前提被破坏了——静态变量那份和参数表那份会各自漂移，台架扫表扫出来的行为不等于出厂行为。

## 2026-09-30 — nvram 记录 version 1→2，新字段只追加在 payload 最末尾，开机时按 length 迁移 v1 记录（前缀原样搬、前馈填当前默认）。
选择：nvram 记录 version 1→2，新字段只追加在 payload 最末尾，开机时按 length 迁移 v1 记录（前缀原样搬、前馈填当前默认）。
放弃：把 version 升到 2 并在校验里直接拒绝 v1 记录 —— 否决，那等于以后每次动 nvram 结构都逼用户重标一次黑白阈值。
因为：加两个参数不能把用户已经标定好的 8 路灰度黑白阈值冲掉。payload 布局是「int32 块 → 布尔 → trace_cal_data_t cal」，只有把字段加在最末尾，v1 记录才会是 v2 记录的一个前缀，迁移才能只补尾巴。

## 2026-09-30 — 速度环验收口径定为「高速段 ≤2%，低速段（<1500）不做精度要求」，前馈常数保留 `0.662/2327`
选择：速度环验收口径定为「高速段 ≤2%，低速段（<1500）不做精度要求」，前馈常数保留 `0.662/2327`
放弃：否决了继续追低速 ±5% 的候选前馈 0.62/2650（它能把 R@1250 从 −6.2% 拉到 ≤1.5%，但在 3000/4000 要多灌 300~360 PWM 才够）；也否决了为低速单独加第三项「起转提速」前馈，因为低速已不在验收口径内，加参数只增加标定负担。
因为：用户明确（低速不用这么准，高速 2% 以内）。实测现有默认参数在 ≥1500 已经全部 ≤2%、≥2000 全部 ≤1%，不需要再动。

## 2026-09-30 — 保留摩擦前馈，不做纯 PI
选择：保留摩擦前馈，不做纯 PI
放弃：否决了「只调 kp/ki/ilim」：实测（`out/bench_ff2.log` fric=0 组）纯 PI 在 ≥3000 也已能到 ±1.5%，但低速段必须靠大积分硬顶，更容易进粘滑极限环。
因为：前馈把稳态输出直接喂到工作点（5000 时前馈 5637 = 实测稳定输出 5637，PI 贡献≈0），积分不用长时间爬，低速段从纯 PI 的 −33% 改善到 −8%。

## 2026-09-30 — 保留 20ms 滑动平均测速（`speed_control.c` 的 `SPEED_AVG_TICKS = 4`），不改回单拍。
选择：保留 20ms 滑动平均测速（`speed_control.c` 的 `SPEED_AVG_TICKS = 4`），不改回单拍。
放弃：否决了「回退成单拍」——高速段的收益可复现且无代价；也否决了「为了低速再调增益」——低速的问题不是增益，是静摩擦物理下限，此前已定结论。
因为：台架 A-B-A-B-A（同一份固件只差 `-DSPEED_AVG_TICKS=1`，交替复跑 5 次）实测：目标 ≥3000 时控制器 PWM 输出峰峰抖动降到约 1/3–1/2，且两组取值范围完全不相交；稳态跟踪均值两版一致（零代价）；阶跃上升时间两版都是 ~20ms。低速段（≤1250）测不出差别，但那里的主导现象是粘滑极限环、不是测速噪声，所以那一段的代表性不足以推翻高速段的结论。

## 2026-09-30 — 把台架 A/B 的正确做法固定为：命令行覆盖 `C_DEFS` 并在**交替序列里复跑多次**，而不是「改一次源码跑一次」。
选择：把台架 A/B 的正确做法固定为：命令行覆盖 `C_DEFS` 并在**交替序列里复跑多次**，而不是「改一次源码跑一次」。
放弃：否决了「改源码 → 跑一次 → 改回来 → 跑一次」的单次对比法。
因为：低速段（目标 ≤1250）的粘滑极限环是混沌的：同一份固件两次复跑，目标 1000 的均值能差 42%（358 vs 510）、输出峰峰差 2.3 倍（1882 vs 809）。单次对比在那里完全没有分辨力，只有把 A-B-A 交替多次、看取值范围是否相交才站得住。

## 2026-09-30 — 速度环控制节拍保持 5ms 不动，只把“测速”改成 20ms 滑动平均（最近 4 拍的计数之和）来提升低速分辨率
选择：速度环控制节拍保持 5ms 不动，只把“测速”改成 20ms 滑动平均（最近 4 拍的计数之和）来提升低速分辨率
放弃：否决 “TIM6 整体改成 20ms”：会同时拖慢循迹/角度环；`gyro_control.c` 的 `GYRO_DT 0.005f` 写死、会静默按 4 倍错的 dt 算；`integral += err` 没乘 dt 所以 `ki` 要重调 4 倍；`SPEED_FULL_COUNTS`/`SPEED_PERIOD_MS`/消抖窗口都得跟着改；目标刻度 0-7200 → 0-100 会撞到 PID_tracing / gyro_control / 参数表 / UI / 上位机协议。也否决了先做台架 A/B 实测（用户拍板直接实现）。
因为：5ms 是整个运动控制节拍（TIM6 一拍里同时跑循迹 PID / 角度 PID / 速度环三层），不是速度环私有的；而且同一时间段内编码器边沿总数不变（495ms/100ms 两种采法一样）—— 5ms 是让 PI 积分器跳拍平均，20ms 是让计数器在窗口里平均，信息量相同，改 20ms 等于用带宽换单次信噪比。

## 2026-09-30 — 极性修复分两处、必须成对：`motor.c` 的 `Load()` 把第二路取负 + `encoder.c` 的 `Encoder_ReadDelta()` 把左路取负
选择：极性修复分两处、必须成对：`motor.c` 的 `Load()` 把第二路取负 + `encoder.c` 的 `Encoder_ReadDelta()` 把左路取负
放弃：否决了“整体给编码器取负 / 整体给电机取负”（只把两路的公共符号翻掉，消不掉“两路约定相反”这个病根）；也否决了“只改一处”（会让速度环变正反馈）。
因为：四条独立证据解出 `σA=+1, pA=−1, σB=−1, pB=+1`：台架 `Load(+,+)` 时两路编码器都读负、用户目视两轮物理转向相反、手推前进时左编码器累计 −21676 / 右 +14623。**两条编码器的“正方向”约定本来就相反**，所以整体翻转救不了。

## 2026-09-30 — 速度环参数暂不动，只把“目标 ≥1500 才可控”写进规格书、由上层限速
选择：速度环参数暂不动，只把“目标 ≥1500 才可控”写进规格书、由上层限速
放弃：暂不加“死区前馈补偿 / breakaway kick 脉冲”—— 未做实测验证前不先改控制律。
因为：目标 1000 时实测在 0 与 656 计数/秒间来回爬，是**静摩擦粘滑极限环**（物理性质，不是增益没调好）；而目标 1500–5000 已经跟到 ±5% 以内。`SPEED_MIN_ACTIVE` 从 500 改成 2600 也不会变好，只是从“不转”变“转太快”。

## 2026-09-30 — SVD 换成 STM32CubeCLT 1.22.0 自带的那份（G4 v3.0、3,515,514 B），同时装成 STM32G474.svd 与 STM32G474xx.svd；旧 modm 版（G4 v2.9）备份在 .work/svd-backup-modm-v2.9/
选择：SVD 换成 STM32CubeCLT 1.22.0 自带的那份（G4 v3.0、3,515,514 B），同时装成 STM32G474.svd 与 STM32G474xx.svd；旧 modm 版（G4 v2.9）备份在 .work/svd-backup-modm-v2.9/
放弃：继续用从 modm-io/cmsis-svd-stm32 取的那份（G4 v2.9、2,965,329 B）
因为：两份外设名集合（各 83 个）、baseAddress、cpu 块完全一致；CLT 版寄存器 1055 vs 918、位域 8408 vs 7586，多的是把 derivedFrom 的 TIM4/TIM5/TIM7/TIM8/TIM17/TIM20 展开成显式寄存器

## 2026-09-30 — 本工作区的 .ps1（含 tools/flash.ps1）必须保持 UTF-8 + BOM；用 tools.edit 改完 .ps1 要检查并补回 BOM（本机 flash.ps1 已补，6244 B / first3=ef bb bf）
选择：本工作区的 .ps1（含 tools/flash.ps1）必须保持 UTF-8 + BOM；用 tools.edit 改完 .ps1 要检查并补回 BOM（本机 flash.ps1 已补，6244 B / first3=ef bb bf）
放弃：直接跑无 BOM 的 .ps1（会失败）
因为：tools.edit 会丢掉 BOM，Windows PowerShell 5.1 对无 BOM 文件按 GBK 解码 → 中文乱码 + 引号错位 → 报 Unexpected token / The string is missing the terminator，make flash 直接失败（2026-09-30 实测踩到）

## 2026-09-30 — 烧录前置改走 STM32CubeCLT 的 STM32_Programmer_CLI（-c port=JLINK -e all -d bin 0x08000000 -v），并用用户级环境变量 STM32_PROGRAMMER_CLI + PATH 指过去；flash.ps1 探测先点已知布局，递归扫描根里去掉 D:\APP；CLI 失败只告警继续走 J-Link
选择：烧录前置改走 STM32CubeCLT 的 STM32_Programmer_CLI（-c port=JLINK -e all -d bin 0x08000000 -v），并用用户级环境变量 STM32_PROGRAMMER_CLI + PATH 指过去；flash.ps1 探测先点已知布局，递归扫描根里去掉 D:\APP；CLI 失败只告警继续走 J-Link
放弃：保留「C:/D: 常见位置 -Recurse -Depth 6」全扫；CLI 一失败就硬中断
因为：全扫实测让 make flash 卡满 600 s 死线且未生成 flash.jlink；CLI 不把 SEGGER 加进 PATH 也能直连探针（Device ID 0x469、4000 kHz），而 CLI 刚 -e all 后 J-Link 报 "Skipped. Contents already match" 正证明 CLI 真写进去了；唯一可信结论仍是 savebin 回读

## 2026-09-30 — C: 只清可再生的缓存/转储/安装包残留，从剩 3.14 GB 清到 23.13 GB
选择：C: 只清可再生的缓存/转储/安装包残留，从剩 3.14 GB 清到 23.13 GB
放弃：改 pagefile.sys（31 GB 固定，改 8 GB 要重启并放弃完整内存转储）；DISM /ResetBase 清 WinSxS（本机被拒 CDISMPackageManager::Internal_Finalize(hr:0x80070005)）
因为：249 GB 的 C: 盘只剩 3 GB，构建/烧录随时会因空间失败；但用户说「不用了到此为止」

## 2026-09-30 — 本机烧录只用 -device STM32G474RE
选择：本机烧录只用 -device STM32G474RE
放弃：用 Makefile 里的 DEVICE = STM32G474RB
因为：DLL 6.82c 下 RB 静默假成功（打印 O.K. 却一个字节没写、还置 FLASH_SR=0xA0），RE 实测 66 KB/s O.K.

## 2026-09-30 — 回读校验把 0x080001D8..0x080001DF 8 字节当已知差异
选择：回读校验把 0x080001D8..0x080001DF 8 字节当已知差异
放弃：要求 bin 与 Flash 完全一致
因为：objcopy -O binary 用 0x00 填 .isr_vector 与 .text 之间的对齐空洞，ihex 没这段所以 Flash 是 0xFF

## 2026-09-30 — 生成 JLink 脚本时每行写字面量、失败时读未过滤原始输出
选择：生成 JLink 脚本时每行写字面量、失败时读未过滤原始输出
放弃：用数组拼接字符串 + Select-String 过滤
因为：拼接把 loadfile 一行拆成两行 → JLink 报 Syntax/Unknown command、什么都不烧；过滤器又把这两行滤掉，误导成板子坏了

## 2026-09-29 — INA226 配置寄存器保持 0x4127 不动，只把注释改成实测确认的位序（bit11:9=AVG、bit14:12 只读保留）
选择：INA226 配置寄存器保持 0x4127 不动，只把注释改成实测确认的位序（bit11:9=AVG、bit14:12 只读保留）
放弃：按原注释把配置真改成 16 次平均（0x0A47）——实测该值转换周期约 100ms，与 16x2.2ms=35ms 的预期不符、机理没吃透，先不引入没解释清楚的行为
因为：位序是上板实测出来的：写 0x0007 回读 0x4007、写 0x0000 回读 0x4000、写 0x6007 回读 0x4007 证明 bit14:12 只读；CVRF 计数 221 次/500ms 证明 0x4127 是 1 次平均而不是原注释说的 16 次

## 2026-09-29 — 满量程时不显示中文『超量程』，改用 ASCII '!' 前缀标在电流/功率数值前
选择：满量程时不显示中文『超量程』，改用 ASCII '!' 前缀标在电流/功率数值前
放弃：重生成 kk_font_body 加入『超量程』三字（要联网调 ledfont API，且会改变整份字模，收益只有一处提示）
因为：kk_font_body 静态子集里缺 超/程/载/满/顶/大/最/小/值/效 等字（check_charset 实测），显示中文得联网重生成字模；'!' 在 ASCII 子集里、零风险

## 2026-09-29 — 过渡合缝取表盘页自己那圈圆角框的右边线（两页整屏 127px 同速滑），不再用 32px 视差
选择：过渡合缝取表盘页自己那圈圆角框的右边线（两页整屏 127px 同速滑），不再用 32px 视差
放弃：上一版：32px 视差 + 合缝取'进来那页的左边缘'，另外在 kk_ui_nav.c 里加了 kk_ui_draw_card() 给两页各画一圈框
因为：视差让出去那一页只滑 32px，它的框右边线永远落在合缝右边、被裁剪窗口切掉，看屏上就是一条黑线把框切断（用户 m04708 的原话）。合缝钉在框右边线上，框就整圈完整、边界自然就是它

## 2026-09-29 — 外框归页面所有：翻页中每页各自的圆角框跟着自己的位移滑（kk_ui_draw_card，按该页裁剪）；静止时只有表盘页自己画一圈框，菜单页不画。
选择：外框归页面所有：翻页中每页各自的圆角框跟着自己的位移滑（kk_ui_draw_card，按该页裁剪）；静止时只有表盘页自己画一圈框，菜单页不画。
放弃：上一版把外框做成贴屏边、不带过渡位移的常驻框（KK_UI_DrawScene 里 OLED_DrawRFrame(0,0,128,64,6)），对所有页面生效。
因为：用户 m04657 明确说不要给菜单带边框，而要的是主页到任务过渡时合缝处是'带圆角的直线'而不是黑边直接切过来 —— 常驻框解决不了合缝、还在菜单上多出一圈用户不要的框。

## 2026-09-29 — 外框由 KK_UI_DrawScene 每帧画一圈贴屏边(0,0,128,64,r=6)的常驻圆角白框，画在两页之后、对话框/Toast 之前，且不带 s_dx。
选择：外框由 KK_UI_DrawScene 每帧画一圈贴屏边(0,0,128,64,r=6)的常驻圆角白框，画在两页之后、对话框/Toast 之前，且不带 s_dx。
放弃：继续让表盘 draw_splash_page 自己画外框（即使去掉 s_dx 也只覆盖自定义页；若保留 s_dx 则翻页时框跟着滑、屏沿露黑边）。
因为：用户要求边界永远是主页那种圆角白框；画在场景层一处就对所有页面（表盘/图标菜单/各设置页）生效，且与过渡位移解耦，翻页中也不会露出黑边。

## 2026-09-29 — '正在保存' 提示每拍重试，只有 KK_UI_ShowToast 返回 KK_UI_OK（提示真的画上屏）才锁按键；BUSY 期间等 OLED 搬运最多 NVRAM_NOTICE_MS+NVRAM_DISPLAY_WAIT_MS(1500U) 就照写。
选择：'正在保存' 提示每拍重试，只有 KK_UI_ShowToast 返回 KK_UI_OK（提示真的画上屏）才锁按键；BUSY 期间等 OLED 搬运最多 NVRAM_NOTICE_MS+NVRAM_DISPLAY_WAIT_MS(1500U) 就照写。
放弃：原来是状态跳变那一拍弹一次、按 BUSY 状态无条件锁键。
因为：对话框开着时 KK_UI_ShowToast 返回 KK_UI_BUSY(kk_ui.c:420)，提示永远不显示而按键照样被吃掉 → 看着像死机；OLED 传输万一被 latch 住，无限等会让 BUSY 永不到头、按键永久失效。

## 2026-09-29 — 把 KK_UI_NavigateTo/NavigateToDirected/NavigateBack/NavigateBackDirected/KK_UI_SetEntrySelection 与 weak 钩子 KK_UI_OnKeyPress/KK_UI_HomeOnMove 的原型加进公共头 kk_ui.h，去掉 app_ui.c 自己的 extern。
选择：把 KK_UI_NavigateTo/NavigateToDirected/NavigateBack/NavigateBackDirected/KK_UI_SetEntrySelection 与 weak 钩子 KK_UI_OnKeyPress/KK_UI_HomeOnMove 的原型加进公共头 kk_ui.h，去掉 app_ui.c 自己的 extern。
放弃：应用直接 extern 只在 kk_ui_internal.h 里声明的符号。
因为：内部头改签名时应用侧不会报错(审计 agent 4)。

## 2026-09-29 — kk_ui_home_move() 端点夹住、绝不回绕（首项再按左/末项再按右就停住）。
选择：kk_ui_home_move() 端点夹住、绝不回绕（首项再按左/末项再按右就停住）。
放弃：m02517 时期用 move %= item_count 的环形回绕。
因为：出屏的移动已由 KK_UI_HomeOnMove 钩子接管（表盘↔菜单环），回绕会让'末项再按右'莫名绕回首项，观感就是滑到缝隙又弹回来。

## 2026-09-29 — 存 Flash 前先 `__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS)`，擦除失败再清一次重擦一遍；擦除结果改用 FLASH->SR 错误位判断，写完保留 memcmp 回读校验
选择：存 Flash 前先 `__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS)`，擦除失败再清一次重擦一遍；擦除结果改用 FLASH->SR 错误位判断，写完保留 memcmp 回读校验
放弃：不靠 nvram 层 5s 重试兜底（那是掩盖症状，用户表现为第一次不提示、5s 后才提示成功）；也没去追查残留标志是谁置的（CubeProgrammer/J-Link 烧录时序，清掉即可，回读校验保证正确性）
因为：HAL_FLASHEx_Erase 一进来就 FLASH_WaitForLastOperation，只要 SR 里带任何错误标志就直接返回 HAL_ERROR 而不擦——复位后 SR 里就有 PGAERR|PGSERR，造成"第一次存盘必失败"

## 2026-09-29 — 删掉 KK_UI 过渡时画在分界处的贯穿竖线（kk_ui_nav.c KK_UI_DrawScene 两个分支各一处 OLED_DrawVLine(boundary,0,64)），只留 OLED_ResetClipWindow()
选择：删掉 KK_UI 过渡时画在分界处的贯穿竖线（kk_ui_nav.c KK_UI_DrawScene 两个分支各一处 OLED_DrawVLine(boundary,0,64)），只留 OLED_ResetClipWindow()
放弃：把线改画在页面之前 / 缩短线长 / 把线挪开 2px —— 都仍是与边框平行的第二条直线，解决不了「看起来是一条直线」；让表盘外框在过渡中不画也不可能（框就是页面的边框）
因为：那条线贯穿 y0..63，而表盘圆角外框只有 y1..62 且圆角要 6px 才收进，两条线只差 1-2px，用户看到的就是「主页边界变成一条直线，切完又变回圆角」（m03792）

## 2026-09-29 — 设置里的「灰度接口」用 KK_UI_IntBinding 新增的第 9 个字段 const char *const *names 显示枚举文案（自动/I2C/串口/数字），NULL 保持老的数字显示
选择：设置里的「灰度接口」用 KK_UI_IntBinding 新增的第 9 个字段 const char *const *names 显示枚举文案（自动/I2C/串口/数字），NULL 保持老的数字显示
放弃：另起一种专门的“枚举型”菜单项类型；或在 INT 行里按 ref 特判
因为：老初始化器只写 8 个字段，第 9 个自动补 NULL，13 个既有绑定一行都不用改，渲染点只有 kk_ui_menu_value 和对话框两处

## 2026-09-29 — 设置+标定存档到最后一页 0x0801F800（第 63 页），记录 = magic "HXB1" + version + length + CRC32 + 144B payload + 补齐，按 20 个双字写
选择：设置+标定存档到最后一页 0x0801F800（第 63 页），记录 = magic "HXB1" + version + length + CRC32 + 144B payload + 补齐，按 20 个双字写
放弃：用 HAL_FLASH 的 EEPROM 模拟、或用选项字节/备份寄存器、或把 .ld 的 FLASH 留出一页专门放数据段
因为：G4 单 bank 擦除粒度 2KB，只有最后一页整页空闲；自算 CRC + 写完 memcmp 回读能立刻发现页坏/RDP，容量也够（160B/2048B）

## 2026-09-29 — nvram_poll 200ms 检查、改动后静 1.5s 才写、OLED_IsBusy() 时让路、开机绝不写（没记录就拿当前值当基线）
选择：nvram_poll 200ms 检查、改动后静 1.5s 才写、OLED_IsBusy() 时让路、开机绝不写（没记录就拿当前值当基线）
放弃：改一下立刻写、或开机把默认值先写进 Flash 建立记录
因为：擦写要停 CPU ~30ms 且与 I2C1 屏总线抢总线；开机就写等于每次上电损耗一次擦写，还会把“没存过”和“存了默认值”混为一谈

## 2026-09-29 — 标定改成一按中键就在一次调用里连采 32 帧（burst），删掉 SAMPLE_BLACK/SAMPLE_WHITE 状态与进度条
选择：标定改成一按中键就在一次调用里连采 32 帧（burst），删掉 SAMPLE_BLACK/SAMPLE_WHITE 状态与进度条
放弃：保留每 20ms 采一帧的状态机、只把 32 帧降到 8 帧
因为：原来 32×20ms=640ms 一个点，用户嫌长；burst 采一次 ≈20~30ms（I2C 16B≈0.6ms/帧），均值仍然是 32 帧去极值

## 2026-09-29 — 灰度模块模拟量走「双通道自动选择」：优先 I2C1 从机 0x01 / 寄存器 0xA2（8 路 BE16），失败 5 帧切 UART4（发 0x57+地址，收 {0x75,8×BE16,0x11} 18 字节或 {0x75,数字量,位置,9×BE16,0x15} 22 字节），两条都判死才回落 PA0–PA7 数字口，2s 后重试。
选择：灰度模块模拟量走「双通道自动选择」：优先 I2C1 从机 0x01 / 寄存器 0xA2（8 路 BE16），失败 5 帧切 UART4（发 0x57+地址，收 {0x75,8×BE16,0x11} 18 字节或 {0x75,数字量,位置,9×BE16,0x15} 22 字节），两条都判死才回落 PA0–PA7 数字口，2s 后重试。
放弃：只用 I2C（用户提议「直接串口通讯给 OLED 省总线」）也没有采纳为唯一通道：I2C 是模块默认接线，串口要另插 H2/U15，两条都支持成本很低。
因为：OLED(0x3C)/INA226(0x40)/灰度模块共用 I2C1，串口通道完全不占该总线；但用户手上模块的接法未知，所以做成自动。

## 2026-09-29 — 换通道只换一次（新增 s_tried 标志），失败到 TRACE_SRC_NONE 后退避 TRACE_OFF_RETRY_MS=2000ms 再从首选通道重试。
选择：换通道只换一次（新增 s_tried 标志），失败到 TRACE_SRC_NONE 后退避 TRACE_OFF_RETRY_MS=2000ms 再从首选通道重试。
放弃：最初写的 other_source() 互转（I2C→UART→I2C→…）被自己发现并修掉。
因为：UART 读失败时每个字节要干等 TRACE_UART_BYTE_MS=2ms、最多 18 字节约 36ms，两条都不通时会无限弹跳，把 235Hz 主循环拖垮。

## 2026-09-29 — 标定点采样升级为 TRACE_CAL_SAMPLE_N=32 帧，插入排序后去掉最高/最低各 TRACE_CAL_TRIM=4 帧再平均（robust_mean），两点里较差的抖动存 s_jit。
选择：标定点采样升级为 TRACE_CAL_SAMPLE_N=32 帧，插入排序后去掉最高/最低各 TRACE_CAL_TRIM=4 帧再平均（robust_mean），两点里较差的抖动存 s_jit。
放弃：原来的 8 帧直接求平均。
因为：用户要求「按键之后多记录几次值取平均」；去掉极值再平均顺带抗手抖/单帧毛刺。32×20ms = 640ms/点，手感还能忍。

## 2026-09-29 — 每路标定有效性独立判定：跨度 < TRACE_MIN_SPAN(32) 或 jitter*3 >= 跨度 → 该路无效，trace_cal_channel() 返回 -1（该路回落数字口），界面在对应柱子下画 '!'。
选择：每路标定有效性独立判定：跨度 < TRACE_MIN_SPAN(32) 或 jitter*3 >= 跨度 → 该路无效，trace_cal_channel() 返回 -1（该路回落数字口），界面在对应柱子下画 '!'。
放弃：整页一刀切判「标定成功/失败」。
因为：8 路里坏一路（虚焊/没压到）很常见，不该让整页作废；同时也不能拿不可信的阈值去跑循迹。

## 2026-09-29 — 采样进度显示 done/32（新函数 draw_progress，手写十进制不用 snprintf）；采样阶段的标题故意复用已有的「全黑按中键」「全白按中键」原文案。
选择：采样进度显示 done/32（新函数 draw_progress，手写十进制不用 snprintf）；采样阶段的标题故意复用已有的「全黑按中键」「全白按中键」原文案。
放弃：采样时换成「黑」「白」两字（更短更好看）。
因为：KK_OLED 用静态字符子集字模，遇到子集外字符静默不画；复用已有文案就不用重新生成字模。

## 2026-09-29 — 应用侧 KK_UI_Invalidate() 闸门从 40ms 改成 20ms（main_app.c:166，与 KK_UI_FRAME_INTERVAL_MS 一致）
选择：应用侧 KK_UI_Invalidate() 闸门从 40ms 改成 20ms（main_app.c:166，与 KK_UI_FRAME_INTERVAL_MS 一致）
放弃：没有去动 KK_UI 的 140ms 过场时长、也没改 I2C 速率（400kHz 是硬件上拉决定的，提到 1MHz 要换 4.7k 上拉）
因为：环里长按翻页的过场是 KK_UI_PAGE_MS_FAST=140ms，40ms 闸门只够画 3~4 帧，用户说"还是感觉有一点卡"；20ms 后同样 140ms 能画 6~7 帧

## 2026-09-29 — 电流检测页放在首页环「陀螺仪」之后（`{ "电流", kk_icon_power, PAGE_CUSTOM_POWER }`），页内固定三行 电流/电压/功率
选择：电流检测页放在首页环「陀螺仪」之后（`{ "电流", kk_icon_power, PAGE_CUSTOM_POWER }`），页内固定三行 电流/电压/功率
放弃：没有做成二级菜单或滚动列表——三个量一屏放得下，直接静态三行
因为：用户 m03132 原话要在陀螺仪后面加一个菜单看这三个量

## 2026-09-29 — INA226 采样放在 KK_UI_CustomOnTick 里，200ms 一次、`!OLED_IsBusy()` 才读、值变了才返回 true 置脏
选择：INA226 采样放在 KK_UI_CustomOnTick 里，200ms 一次、`!OLED_IsBusy()` 才读、值变了才返回 true 置脏
放弃：没放进 TIM 中断、也没跟着 host_link 的 100ms 遥测一起读（那条路径已经在读 bus/current，但只喂上位机）
因为：INA226 与 OLED 共用 I2C1(PA15/PB9)，抢总线会把屏刷坏；整屏 8 页 @400kHz 要 23ms，不能每拍重绘

## 2026-09-29 — 功率直接读 INA226 寄存器 0x03，`*mw = (int64_t)u * 25`（Power_LSB = 25×Current_LSB）
选择：功率直接读 INA226 寄存器 0x03，`*mw = (int64_t)u * 25`（Power_LSB = 25×Current_LSB）
因为：芯片自己算好的值，不用 U*I 相乘（还会放大误差）；CAL=1024 @5mΩ → 1mA/bit → 25mW/bit

## 2026-09-29 — 字模 charset 必须把新页要显示的汉字显式拼进去再 POST generate
选择：字模 charset 必须把新页要显示的汉字显式拼进去再 POST generate
放弃：不能只靠『扫全部源码字面量』自动收集
因为：第一次生成时漏了 流/压/率（它们从没在任何源码字符串字面量里出现过），check_charset 报缺字；OLED_DrawUTF8 遇到子集外字符是静默不画

## 2026-09-29 — 长按连翻在 KK_UI 中间件里修：过渡期间被挡下的方向键重复事件只记最后一个方向、落地瞬间补发一次；且重复事件触发的页面过渡缩短为 KK_UI_PAGE_MS_FAST 140ms（单次 press 仍是 KK_UI_PAGE_MS 220ms）
选择：长按连翻在 KK_UI 中间件里修：过渡期间被挡下的方向键重复事件只记最后一个方向、落地瞬间补发一次；且重复事件触发的页面过渡缩短为 KK_UI_PAGE_MS_FAST 140ms（单次 press 仍是 KK_UI_PAGE_MS 220ms）
放弃：否决了在应用侧续翻（HOME 页拿不到过渡结束回调）、也否决了给表盘加停顿时长（用户 m02919 已经否掉）
因为：原来过渡期间丢弃重复事件，长按翻页的实际节奏是 240/240/240→160×6→200ms（按住 ≥2s 时反而从 160 回落到 200，就是用户 m03081 说的"卡一下再重新加速"）；补发后变成 240→160→140 单调加速，主机端复刻时序已验证

## 2026-09-29 — 陀螺仪页默认轴档 = M3（app_ui.c 的 s_gyro_axis_mode = 2U）
选择：陀螺仪页默认轴档 = M3（app_ui.c 的 s_gyro_axis_mode = 2U）
放弃：M4（=3U，线框时期定的）
因为：用户 m02919：方框从线框改成实体渲染后重新确认『m3 才是对的』。线框没有前后遮挡，看不到立体朝向，当初才误判成 M4。上电后按中键仍可切 M1..M4。

## 2026-09-29 — 表盘不为长按设『停顿时长』，环的节奏完全交给框架（翻页动画 220ms + 重复间隔 240/160/100ms）
选择：表盘不为长按设『停顿时长』，环的节奏完全交给框架（翻页动画 220ms + 重复间隔 240/160/100ms）
放弃：曾经加的 SPLASH_DWELL_MS 450S 门控（进表盘 450ms 内不理会长按重复）
因为：用户 m02919：『本来是会越来越快的转，现在到你这里就卡住一下然后重新加速』。过渡期间输入本来就被框架屏蔽（kk_ui.c:596），表盘天然能看清 220ms，不需要额外停顿。

## 2026-09-29 — 陀螺仪方框 = 隐藏面消隐 + 车顶 50% 网点填充（gy_fill_quad 隔行扫描线）
选择：陀螺仪方框 = 隐藏面消隐 + 车顶 50% 网点填充（gy_fill_quad 隔行扫描线）
放弃：更早的 12 条线框；以及实心填充（OLED_DrawFilledTriangle）
因为：用户 m02820『正方形只有线条容易看错』；实心会变成一块黑方块，轮廓和立体感都没了，网点填充既看得出朝向又保留棱线。

## 2026-09-29 — 长按环包含表盘：KK_UI_HomeOnMove 里首项继续往左 / 末项继续往右都真的滑回表盘（弹出，不涨栈）
选择：长按环包含表盘：KK_UI_HomeOnMove 里首项继续往左 / 末项继续往右都真的滑回表盘（弹出，不涨栈）
放弃：遇到 REPEAT 直接 return false 交还框架在菜单内取模回绕
因为：用户 m02820：回绕把表盘整屏跳掉了。环现在是 表盘→任务→PID→循迹测试→模块测试→陀螺仪→设置→表盘 连续 7 屏，栈仍恒为 1 层。

## 2026-09-29 — 烧录用 STM32CubeProgrammer CLI（-c port=JLINK）作为可信路径；J-Link loadbin/verifybin 只当便捷路径，且超过 64KB 必须用 savebin 回读逐字节比对。
选择：烧录用 STM32CubeProgrammer CLI（-c port=JLINK）作为可信路径；J-Link loadbin/verifybin 只当便捷路径，且超过 64KB 必须用 savebin 回读逐字节比对。
放弃：继续只信 J-Link 的 'Verify successful'。
因为：2026-09-29 实测：J-Link loadbin 声称 O.K.+Verify successful，但 savebin 回读发现 0x08008000 之后根本不是我们的镜像（CPU 跑到已擦除区 → CFSR=0x00010000 未定义指令 → HardFault）；改用 CubeProgrammer mass erase + 编程 + verify 后回读与 bin 69180 字节完全一致（diffs=0）。

## 2026-09-29 — RTT 抓日志固定带 -RTTAddress 0x20002550。
选择：RTT 抓日志固定带 -RTTAddress 0x20002550。
放弃：让 JLinkRTTLogger 自己搜 'SEGGER RTT' 控制块。
因为：自动搜索一直停在 'Searching for RTT Control Block...'，而用 mem8 直接读 0x20002550 明明是 'SEGGER RTT' + MaxUp=3/MaxDown=3/pBuffer=0x20001550/WrOff 在涨；加 -RTTAddress 后立刻出数据。

## 2026-09-29 — option byte DBANK 从 1（双 bank 64-bit）改成 0（单 bank 128-bit）。
选择：option byte DBANK 从 1（双 bank 64-bit）改成 0（单 bank 128-bit）。
放弃：保持出厂双 bank 设置。
因为：128KB 的 G474RB 在 DBANK=1 下 J-Link 只覆盖 bank0(64KB)，超过 0x08010000 的镜像静默丢失（本工程固件第一次超过 64KB 才暴露）。

## 2026-09-29 — PID 参数在固件里用 ×100 定点整数存储（app_settings_t 的 *_x100、KK_UI_IntBinding 的 value），只在显示层用新增的 decimals 字段还原小数、下发上位机时 ÷100 变 float
选择：PID 参数在固件里用 ×100 定点整数存储（app_settings_t 的 *_x100、KK_UI_IntBinding 的 value），只在显示层用新增的 decimals 字段还原小数、下发上位机时 ÷100 变 float
放弃：给 KK_UI 的数值编辑框加浮点/定点通用支持，或把参数直接绑到 float 上
因为：kk_ui.h 的 KK_UI_IntBinding 只有 int32_t* 与整数 step，改造面太大；加一个只影响显示的 decimals 字段既不动老代码，又能让屏幕显示真小数（用户 m01814 直接问「这个*100是为啥」）

## 2026-09-29 — 固件 firmware/App/Svc/host_link.c 的 SPECS 表与上位机 tools/host/protocol.py 的 PARAMS 表必须同序、同 id、同默认值（加参数时连同 docs/上位机通信协议.md 参数表一起改）
选择：固件 firmware/App/Svc/host_link.c 的 SPECS 表与上位机 tools/host/protocol.py 的 PARAMS 表必须同序、同 id、同默认值（加参数时连同 docs/上位机通信协议.md 参数表一起改）
放弃：两边各自维护、靠人工比对
因为：param_table_crc 是把每项 (id, def) 打包成 6 字节算 CRC16，顺序或值不一致上位机就报参数表不匹配；本次加了 4 项速度环参数，Node 复算必须两边都等于 crc=0x661D 才算对

## 2026-09-29 — 全页面按键反馈做成 KK_UI 中间件的 weak 钩子 KK_UI_OnKeyPress(keys, now)，应用写同名强定义接管
选择：全页面按键反馈做成 KK_UI 中间件的 weak 钩子 KK_UI_OnKeyPress(keys, now)，应用写同名强定义接管
放弃：在每个页面的输入分支里各写一遍判断（原来只在任务 A/B 按中键那一处）
因为：KK_UI 只对 CUSTOM 页给应用输入回调，内置页（表盘/菜单）应用侧根本没有按键回调点，逐页写既漏又散；钩子放在去抖之后、'过渡中屏蔽按键'之前，能覆盖所有页面且天然只在新按下时触发一次。

## 2026-09-29 — 陀螺仪页：三维线框放屏幕左半边、右侧三行全名（横滚/俯仰/偏航）数值右对齐
选择：陀螺仪页：三维线框放屏幕左半边、右侧三行全名（横滚/俯仰/偏航）数值右对齐
放弃：原来把「横/俯/偏 + 数值」三列并排在底部一行（每列 42px）
因为：线框外接半径最坏 2.42*GY_SC，底部一行必然被线框压住；而且并排三列时 |角度|>=100.0 必须省掉小数才不会顶到下一列。全名比单字清楚，是用户明确要求（用显示全名字比如横滚）。

## 2026-09-29 — 图标菜单顺序（也就是环上从表盘往右的顺序）= 任务 / 循迹测试 / 模块测试 / 陀螺仪 / 设置，设置排在最后。于是从表盘左滑第一格是"设置"、第二格是"陀螺仪"。
选择：图标菜单顺序（也就是环上从表盘往右的顺序）= 任务 / 循迹测试 / 模块测试 / 陀螺仪 / 设置，设置排在最后。于是从表盘左滑第一格是"设置"、第二格是"陀螺仪"。
放弃：旧顺序 任务 / 设置 / 循迹测试 / 模块测试 / 陀螺仪（设置排第二，左滑第一格是陀螺仪）。
因为：用户明确要求"把设置放到陀螺仪后面，变成左滑第一个是设置、第二个是陀螺仪"。表盘入口落点用 sizeof(k_home_items)-1 取最后一项，所以改数组顺序会自动跟着变，不用再改导航代码。

## 2026-09-29 — 表盘(PAGE_SPLASH)与图标菜单 = 同一条 6 位环：…设置→循迹测试→模块测试→陀螺仪→(回到)表盘；进入菜单时落点跟按键方向走（按左→陀螺仪、按右/中→任务），菜单内部绝不回绕。
选择：表盘(PAGE_SPLASH)与图标菜单 = 同一条 6 位环：…设置→循迹测试→模块测试→陀螺仪→(回到)表盘；进入菜单时落点跟按键方向走（按左→陀螺仪、按右/中→任务），菜单内部绝不回绕。
放弃：否决了两版：①把主页做成图标菜单里带房子图标的一项（用户：不是这个）；②菜单条带环形回绕（kk_ui_home_move 的 % item_count + kk_ui_home_delta 折 ±半圈）。
因为：回绕会让选中第 0 项时最后一项被折到 x=-16 显示在屏幕左边缘（用户看到'任务左边冒出陀螺仪'），且 5 项换项时某些项会瞬移 4*64=256px（'动画完全不对'）；而真正的环应由表盘在两端缝合、菜单内部是一条直线。

## 2026-09-29 — 跨屏导航一律成对匹配：前进用 KK_UI_NavigateTo/NavigateToDirected（压栈），回退用 KK_UI_NavigateBack/NavigateBackDirected（弹栈），导航栈恒为 1 层。
选择：跨屏导航一律成对匹配：前进用 KK_UI_NavigateTo/NavigateToDirected（压栈），回退用 KK_UI_NavigateBack/NavigateBackDirected（弹栈），导航栈恒为 1 层。
放弃：否决了'每次回家都 NavigateTo'的写法（旧版就是这么写的）。
因为：kk_ui_config.h 的 KK_UI_NAV_DEPTH=8 压满后 kk_ui_navigate_push 会记 KK_UI_NAVIGATION_FULL 并置 kk_ui.reject_flash=2 → 屏幕闪一下白框且进不去（用户报的'卡在主页、按按键亮一下白框'）。

## 2026-09-29 — 主页＝KK_UI 的 root CUSTOM 页（表盘），与图标菜单互为『左右相邻两屏』：菜单→表盘用 KK_UI_NavigateBack 弹出，表盘→菜单用 KK_UI_NavigateTo 压入
选择：主页＝KK_UI 的 root CUSTOM 页（表盘），与图标菜单互为『左右相邻两屏』：菜单→表盘用 KK_UI_NavigateBack 弹出，表盘→菜单用 KK_UI_NavigateTo 压入
放弃：把主页做成图标菜单里带房子图标的一项（用户明确否掉：『不是说现在这个房子图案』）；也否掉了『每次回家都 NavigateTo』的写法
因为：KK_UI_NAV_DEPTH=8，压入/弹出必须成对才不会把栈压满；栈满时 KK_UI_NavigateTo 会记 KK_UI_NAVIGATION_FULL 并置 reject_flash=2，实机表现就是『卡住并亮一下白框』。另外 kk_ui.c:429-438 的 KK_UI_CustomRequestClose 在 stack_count==0 时返回 NOT_ALLOWED，所以根页只能用 NavigateTo 出去

## 2026-09-29 — 中文文案必须与 kk_font_body/kk_font_home 的静态子集逐字对齐；本次选择按 kk-oled-font 流程重生成字模补入『右』U+53F3（而不是把文案改成别的字）
选择：中文文案必须与 kk_font_body/kk_font_home 的静态子集逐字对齐；本次选择按 kk-oled-font 流程重生成字模补入『右』U+53F3（而不是把文案改成别的字）
放弃：把『按左右键』改成不含右字的文案
因为：KK_OLED 是静态子集字模，子集外字符静默不画（不是乱码/方框），极易漏；重生成能让字符集只增不减并彻底消除这个隐患，成本只是联网一次 + 28 字节 flash

## 2026-09-29 — 菜单末尾的「返回」行不再由应用自己添加，交给 KK_UI 在 item_index==page->item_count 处合成
选择：菜单末尾的「返回」行不再由应用自己添加，交给 KK_UI 在 item_index==page->item_count 处合成
放弃：改中间件去识别 label=='返回' 再改箭头方向 —— 没必要且更脏。
因为：kk_ui_menu.c:351 本来就会合成一行返回（画的是 back=true 的 "<"，走 KK_UI_NavigateBack）；应用再放一个 { "返回", KK_UI_MENU_PAGE, ... } 会多出一行，而且它是 PAGE 类型 → kk_ui_draw_enter_arrow(..., false) 画成 ">"。

## 2026-09-29 — 上电首屏用新的自定义页 PAGE_SPLASH(=11)，KK_UI_App.root_page 指过去
选择：上电首屏用新的自定义页 PAGE_SPLASH(=11)，KK_UI_App.root_page 指过去
因为：桌面/菜单要能互跳；主页按下键→PAGE_HOME，图标菜单第一项(任务)按上→PAGE_SPLASH。KK_UI_NavigateTo 只在 kk_ui_internal.h 里有声明，app_ui.c 里显式 extern 一下即可。

## 2026-09-29 — 主页文案只用 ASCII + 已在字模子集里的中文（HAIXINBEI / TERRAIN ROBOT / 按下键）
选择：主页文案只用 ASCII + 已在字模子集里的中文（HAIXINBEI / TERRAIN ROBOT / 按下键）
放弃：想写「全地形机器人」但 KK_OLED 是静态子集字模，子集外字符静默不画；要写新中文必须先按 kk-oled-font 技能重生成字模。

## 2026-09-29 — KK_OLED 字模改为「按 UI 实际字符集重新生成」而不是随手增删：`kk_font_body`=wenquanyi_9pt@12（186 字形）、`kk_font_home`=wenquanyi_11pt@15（108 字形），字符集 = 全部可打印 ASCII + UI 用到的中文
选择：KK_OLED 字模改为「按 UI 实际字符集重新生成」而不是随手增删：`kk_font_body`=wenquanyi_9pt@12（186 字形）、`kk_font_home`=wenquanyi_11pt@15（108 字形），字符集 = 全部可打印 ASCII + UI 用到的中文
放弃：否决「只把新加的几个字补进去」——旧子集缺 模/块 等字形且缺字时静默不画，无法从屏幕判断漏了哪些；也否决用不支持的字号（目录里该字体只有固定字号）
因为：LEDFont 的 API 是「按 text 生成压缩字模」，一次给全字符集最省事也最不容易漏；且 kk-oled-font 技能要求以在线目录和返回的 characters/glyph_count 为准

## 2026-09-29 — 主界面结构定为首页 **5 个入口：设置 / 任务 / 循迹测试 / 总测试 / 陀螺仪**；「任务」是二级菜单（任务A / 任务B）；**各入口不允许重复出现在别处**（设置菜单里不再放陀螺仪/总测试）
选择：主界面结构定为首页 **5 个入口：设置 / 任务 / 循迹测试 / 总测试 / 陀螺仪**；「任务」是二级菜单（任务A / 任务B）；**各入口不允许重复出现在别处**（设置菜单里不再放陀螺仪/总测试）
放弃：否决把陀螺仪、总测试同时挂在首页和设置菜单里 —— 用户看到重复项后明确要求清掉
因为：用户 2026-09-29 反馈"你把这些重复的都清掉了,我发现好多重复的"

## 2026-09-29 — 主程序显示按 **180° 安装**：`OLED_SetRotation(OLED_ROTATION_180)`，**五向键同时翻转**（上↔下、左↔右，中键不变），由 `App/Inc/app_ui.h` 的 `APP_UI_DISPLAY_ROT180` 一个开关控制
选择：主程序显示按 **180° 安装**：`OLED_SetRotation(OLED_ROTATION_180)`，**五向键同时翻转**（上↔下、左↔右，中键不变），由 `App/Inc/app_ui.h` 的 `APP_UI_DISPLAY_ROT180` 一个开关控制
放弃：否决只翻显示不翻按键 —— 那样用户按"上"菜单会往下走
因为：用户要求"屏幕翻转一下对应的按键也转过来"

## 2026-09-20 — 主程序刷新方式定为 **I2C1 @400kHz + KK_UI_REFRESH_MODE=KK_UI_REFRESH_IT**（页命令和页数据都走中断）；**不用 DMA**
选择：主程序刷新方式定为 **I2C1 @400kHz + KK_UI_REFRESH_MODE=KK_UI_REFRESH_IT**（页命令和页数据都走中断）；**不用 DMA**
放弃：否决 `KK_UI_REFRESH_DMA`（页数据走 DMA1_Channel1）—— 实测 400kHz 下**偶发黑屏**，同速下 IT 正常
因为：人眼四组合实测（`make DIAG=1` 诊断固件）：100K阻塞✅ / 100K+DMA✅ / 400K阻塞✅ / 400K+DMA❌；第二轮换速率又变成 400K+DMA✅ / 300K+DMA❌ / 200K+DMA✅ / 400K+IT✅ —— **同一配置两次结果不同 = 临界偶发，不是干净的速率阈值**，典型来源是 HAL 里"中断发页命令→DMA 发页数据"的交接竞争。保留 BLOCKING 作为最快退回的基线

## 2026-09-20 — 主程序显示刷新改为**异步**：I2C1 跑 **400kHz** + `-DKK_UI_REFRESH_MODE=KK_UI_REFRESH_DMA`（页命令走 I2C1 中断，页数据走 **DMA1_Channel1 / DMA_REQUEST_I2C1_TX**）
选择：主程序显示刷新改为**异步**：I2C1 跑 **400kHz** + `-DKK_UI_REFRESH_MODE=KK_UI_REFRESH_DMA`（页命令走 I2C1 中断，页数据走 **DMA1_Channel1 / DMA_REQUEST_I2C1_TX**）
放弃：否决保持 100kHz 阻塞刷新（整屏 ~119ms，主循环被卡住 4.7ms/帧）
因为：用户要求"提高实时"。实测 400kHz+阻塞 整屏 29.7ms；改异步后主循环单帧耗时 avg=max=30us（不再阻塞），每秒 334 次输入采样

## 2026-09-20 — 工程改为**双目标**：`make` 出主程序(haixin-app)，`make SELFTEST=1` 出自检固件；显示层用 **KK_OLED + KK_UI**，`kk_oled` 自带的 CH1116 驱动已按本板改写成 **SSD1315**
选择：工程改为**双目标**：`make` 出主程序(haixin-app)，`make SELFTEST=1` 出自检固件；显示层用 **KK_OLED + KK_UI**，`kk_oled` 自带的 CH1116 驱动已按本板改写成 **SSD1315**
放弃：否决继续用旧的江协科技 `OLED.c`(48KB 软件I2C) —— 上层要的实时画面改为放进 KK_UI 的**自定义页**里画
因为：用户要求：只换底层驱动不动上层，OLED 驱动换成 kk_ui 并用它的动效做菜单

## 2026-09-20 — 电机驱动**必须用 AT8236**（立创 C2827823），不得替换成 **AT8236N**；板上 VREF(脚4) 硬接 3.3V、ISEN(脚7) 直接接 GND
选择：电机驱动**必须用 AT8236**（立创 C2827823），不得替换成 **AT8236N**；板上 VREF(脚4) 硬接 3.3V、ISEN(脚7) 直接接 GND
放弃：AT8236N（4 脚是 nFAULT 开漏输出）—— 本板 4 脚硬接 3.3V，换 N 版后过流/过温把 nFAULT 拉低就等于把 3.3V 对地短路
因为：查 AT8236 手册：两款第 4 脚定义不同（VREF vs nFAULT）；v6 网表实测 U3.4/U4.4 接 VCC3V3、U3.7/U3.9 接 GND

## 2026-09-18 — **以 `固件接口规格.md` 为唯一权威文档**，`pcb/主板选型与设计.md` 降为「选型/电源/位号/布局」辅助文档（v1.0，已按网表 v6 重写同步）
选择：**以 `固件接口规格.md` 为唯一权威文档**，`pcb/主板选型与设计.md` 降为「选型/电源/位号/布局」辅助文档（v1.0，已按网表 v6 重写同步）
放弃：否决删掉 pcb 那份（选型理由、被否决的替代、布局要点在规格书里没有位置）
因为：用户 2026-09-18：「那就用固件接口，然后你同步一下」——两份文档职责分开，冲突时以规格书为准

## 2026-09-16 — I2C1 上拉 R4/R5 保持 **4.7kΩ**（不改成 2.2k）——与灰度模块自带的 10k 并联后 Rp≈**3.2k**，既低于 4.7k 又远高于下限 967Ω
选择：I2C1 上拉 R4/R5 保持 **4.7kΩ**（不改成 2.2k）——与灰度模块自带的 10k 并联后 Rp≈**3.2k**，既低于 4.7k 又远高于下限 967Ω
放弃：改成 2.2k（并联后 1.8k，400kHz 余量更大但静态电流更大；且若模块 10k 接 5V，2.2k 能把空闲电压压得更低）
因为：用户决定：4.7k 并联后降低但不至于太低；且当前主要跑 100kHz 场景，4.7k 已足够

## 2026-09-16 — I2C1 上拉电阻 R4/R5 由 4.7kΩ 改为 **2.2kΩ**（→3.3V），以支持 400kHz
选择：I2C1 上拉电阻 R4/R5 由 4.7kΩ 改为 **2.2kΩ**（→3.3V），以支持 400kHz
放弃：保留 4.7k（400kHz 下只能容忍约 75pF 总线电容，插上灰度模块会超）；也不取更小值（Rp_min=(3.3−0.4)/3mA≈967Ω）
因为：总线上有 MCU+INA226(0x40)+OLED SSD1315(0x3C)+灰度模块+H3/U14 两座，估算 Cb 约 50~130pF；2.2k 在 400kHz 下可容忍≈161pF，100kHz 更稳

## 2026-09-15 — 循迹/灰度模块改用 **I2C 接口**（接 H3/U14，即 I2C1：PA15=SCL、PB9=SDA）；8 路 huidu1–8 数字线仅作备用
选择：循迹/灰度模块改用 **I2C 接口**（接 H3/U14，即 I2C1：PA15=SCL、PB9=SDA）；8 路 huidu1–8 数字线仅作备用
放弃：用 8 路数字直连 PA0–PA7
因为：模块自带 MCU 且支持 I2C；用户明确 I2C 信号是 3.3V（5V 仅供电），故不存在 5V 顶 TT 脚的问题

## 2026-09-15 — **D5 (SS54) 保留**——串在 5V 输出上是有意做**防倒灌**，不是错误，不改
选择：**D5 (SS54) 保留**——串在 5V 输出上是有意做**防倒灌**，不是错误，不改
放弃：我此前建议去掉 D5
因为：用户说明其为防倒灌用途

## 2026-09-15 — **BOOT0 不加跳线**——板上没有外挂 Flash，确认只用内部 Flash 启动 + SWD 下载
选择：**BOOT0 不加跳线**——板上没有外挂 Flash，确认只用内部 Flash 启动 + SWD 下载
放弃：加 BOOT0 排针/跳线帽
因为：用户明确无外挂 flash，启动模式固定

## 2026-09-15 — **暂时不做 RGB/WS2812**：PC12 空着，不上 WS2812
选择：**暂时不做 RGB/WS2812**：PC12 空着，不上 WS2812
放弃：原 v0.8 设计保留 2×WS2812
因为：用户 2026-09 明确「暂时先不要 rgb」

## 2026-09-15 — INA226 的 **ALERT 先不接**（与 PB12 一起标 no-connect），只用 I2C 轮询读电流/电压/功率；若以后要过流中断再接 PB12 + 10k↑3V3
选择：INA226 的 **ALERT 先不接**（与 PB12 一起标 no-connect），只用 I2C 轮询读电流/电压/功率；若以后要过流中断再接 PB12 + 10k↑3V3
放弃：接 ALERT 做事件中断
因为：用户问「必须接吗」；不接不影响测量

## 2026-09-15 — I2C 上拉**只在 MCU 端加一组 4.7k→3.3V**（SCL=PA15、SDA=PB9），不每个模块都加；模块自带的上拉能去则去
选择：I2C 上拉**只在 MCU 端加一组 4.7k→3.3V**（SCL=PA15、SDA=PB9），不每个模块都加；模块自带的上拉能去则去
放弃：每个模块各加一组（并联后阻值过小）
因为：I2C 开漏总线整条只需一组上拉，且必须拉到 MCU 的 3.3V 电平

## 2026-09-15 — HSE 晶振用 **25MHz（晶振 CL=12pF）+ 2×15pF**，PLL = **/5 ×68 /2 = 170MHz**（替代原 8MHz + 2×22pF）
选择：HSE 晶振用 **25MHz（晶振 CL=12pF）+ 2×15pF**，PLL = **/5 ×68 /2 = 170MHz**（替代原 8MHz + 2×22pF）
放弃：否决 8MHz+22pF
因为：用户选定 25MHz 晶振；CL=12pF 按 C=2(CL−Cstray) 算理论 14–18pF，15pF 合适

## 2026-09-15 — 电机驱动最终定 **2× AT8236**（单路 H 桥，每电机一颗；板载）：电机A `PC6=IN1 / PC7=IN2`、电机B `PC8=PC9=IN1/IN2`（TIM8_CH1–4，PWM 直接打输入）；**ISEN/VREF 接 GND**（不启用逐周期限流）、**EPAD 接 GND 铺铜散热**；`OUT1/OUT2 → 电机`、`VM → +12V`
选择：电机驱动最终定 **2× AT8236**（单路 H 桥，每电机一颗；板载）：电机A `PC6=IN1 / PC7=IN2`、电机B `PC8=PC9=IN1/IN2`（TIM8_CH1–4，PWM 直接打输入）；**ISEN/VREF 接 GND**（不启用逐周期限流）、**EPAD 接 GND 铺铜散热**；`OUT1/OUT2 → 电机`、`VM → +12V`
放弃：否决 DRV8412（太贵）、TB6612FNG（1.2A 余量小）、RZ7899-MS（标 6A 实为 ~3A、SOP-8 无散热焊盘）
因为：用户选定；且 AT8236 没有 nRESET/FAULT/OTW，**PB0/PB1/PB2 三个脚省下来**，引脚更宽松

## 2026-09-15 — I2C1（OLED+INA226+灰度）改用 **PA15=I2C1_SCL / PB9=I2C1_SDA**，PB8 归还 BOOT0；编码器2 从 PA15/PB3 改到 **PB4/PB5 = TIM3_CH1/CH2**；INA226 ALERT 改 **PB12**、状态灯改 **PA12**
选择：I2C1（OLED+INA226+灰度）改用 **PA15=I2C1_SCL / PB9=I2C1_SDA**，PB8 归还 BOOT0；编码器2 从 PA15/PB3 改到 **PB4/PB5 = TIM3_CH1/CH2**；INA226 ALERT 改 **PB12**、状态灯改 **PA12**
放弃：否决继续用 PB8/PB9 做 I2C1（PB8=BOOT0，I2C 上拉会把 BOOT0 拉高导致不启动，必须靠选项位兜底）；否决 I2C2 用 PC4/PC5（PC5 根本没有 I2C 功能）
因为：以 ST 官方 STM32_open_pin_data（STM32G474R(B-C-E)Tx.xml）核对：G4 的 I2C1_SCL 只在 PA13/PA15/PB8、I2C1_SDA 只在 PA14/PB7/PB9；BOOT0 就在 PB8 上

## 2026-09-14 — 电源方案（用户自己设计）：12V→**7.4V（舵机）** 与 12V→**5V（逻辑）** 两路，均用 **芯洲科技 SP1N28STER**（ESOP-8，Vin 3.8–36V，**5A** 连续，同步降压，内置 45mΩ/20mΩ MOS，**Vfb=0.8V±1%**，频率 100kHz–2.2MHz 可编程，4ms 软启动）
选择：电源方案（用户自己设计）：12V→**7.4V（舵机）** 与 12V→**5V（逻辑）** 两路，均用 **芯洲科技 SP1N28STER**（ESOP-8，Vin 3.8–36V，**5A** 连续，同步降压，内置 45mΩ/20mΩ MOS，**Vfb=0.8V±1%**，频率 100kHz–2.2MHz 可编程，4ms 软启动）
放弃：我此前建议的 SY8205(舵机) + MP2315(5V) 方案（用户改用自己的方案）
因为：用户决定自己设计电源；已按 SP1N28 的 0.8V 基准和 5A 能力核对

## 2026-09-14 — 陀螺仪改回**外接 JY61P 模块**，走 **USART3（PB10/PB11）115200 + DMA RX**（与旧板一致）
选择：陀螺仪改回**外接 JY61P 模块**，走 **USART3（PB10/PB11）115200 + DMA RX**（与旧板一致）
放弃：否决板载 ICM-42688-P(SPI2)
因为：用户要求沿用之前的 JY61P

## 2026-09-14 — 串口重新分配：USART3=JY61P；**UART4(PC10/PC11)=无线下载器**；**LPUART1(PC0/PC1)=备用串口 + J6 串口灰度(15/16)**
选择：串口重新分配：USART3=JY61P；**UART4(PC10/PC11)=无线下载器**；**LPUART1(PC0/PC1)=备用串口 + J6 串口灰度(15/16)**
放弃：—
因为：JY61P 占用 USART3 后无线下载器需改到 UART4；灰度的串口接口由 UART4 改为 LPUART1

## 2026-09-14 — 主控最终定 **STM32G474RBT6**（LQFP64，128KB Flash + 128KB RAM，52 I/O，170MHz；立创 C529413）
选择：主控最终定 **STM32G474RBT6**（LQFP64，128KB Flash + 128KB RAM，52 I/O，170MHz；立创 C529413）
放弃：否决 G431RBT6（RAM 仅 32KB）、G474RET6（512KB Flash 用不上且更贵）、LQFP100 的 G474VET6/G431VBT6（本项目引脚够用，没必要换封装）
因为：它与 G431RBT6 同封装同引脚，但 RAM 是 4 倍（128KB）、模拟资源更多，且不必升 LQFP100；128KB Flash 对旧工程(hex 81KB)够

## 2026-09-14 — 全部功能板载：DRV8412 做到主板上，电机 6P 直接接 DRV 输出；不再外接驱动模块
选择：全部功能板载：DRV8412 做到主板上，电机 6P 直接接 DRV 输出；不再外接驱动模块
放弃：否决外接驱动模块方案（此前 v0.4/v0.5 的备选）
因为：用户明确「所有都板载」

## 2026-09-14 — 五向方向键用 XKB **TM-4175-B-B**（立创 C465996，SMD-8P 7.5×7.5×6mm，12V/50mA，库存约 3761）
选择：五向方向键用 XKB **TM-4175-B-B**（立创 C465996，SMD-8P 7.5×7.5×6mm，12V/50mA，库存约 3761）
放弃：否决 WS-4175(C53207707) 与 K1-5203UA-06(C5370424)，这两款库存仅 37/126
因为：用户指定 TM-4175-B-B；且该型号立创库存充足

## 2026-09-14 — 灰度传感器主用 **I2C2（PC4/PC5）**，UART4(PC10/PC11) 作为备份/调试
选择：灰度传感器主用 **I2C2（PC4/PC5）**，UART4(PC10/PC11) 作为备份/调试
放弃：—
因为：用户说「灰度主用 i2c」

## 2026-09-14 — 电流检测只做 12V 主电源（INA226 + 10mΩ 分流），不做每路电机电流采样
选择：电流检测只做 12V 主电源（INA226 + 10mΩ 分流），不做每路电机电流采样
放弃：否决每路电机分流/INA + 电流环
因为：用户明确「电流检测只做主电源的」；DRV8412 用 OC_ADJ 设逐周期限流，无需外部分流

## 2026-09-14 — 驱动选 TI DRV8412 双路 H 桥（2×3A 持续 / 2×6A 峰值，最高 52V，HTSSOP-44），用 4 路 PWM(PC6-PC9, TIM8_CH1-4) + RESET_AB/RESET_CD + FAULT 控制；余量可选 DRV8432(2×7A)
选择：驱动选 TI DRV8412 双路 H 桥（2×3A 持续 / 2×6A 峰值，最高 52V，HTSSOP-44），用 4 路 PWM(PC6-PC9, TIM8_CH1-4) + RESET_AB/RESET_CD + FAULT 控制；余量可选 DRV8432(2×7A)
放弃：否决单路 DRV8871/8873、否决 TB6612/L298
因为：用户要 DRV 系列、双路、电流大；JGB37-3865-520 堵转 2.5~3.5A，2×3A 持续刚好覆盖

## 2026-09-14 — 按键换成「五向方向键」（上/下/左/右/中），立创现货候选 WS-4175(C53207707, 7.5×7.5 SMD-8P) 或 K1-5203UA-06(C5370424, 8.4×7.6 SMD)
选择：按键换成「五向方向键」（上/下/左/右/中），立创现货候选 WS-4175(C53207707, 7.5×7.5 SMD-8P) 或 K1-5203UA-06(C5370424, 8.4×7.6 SMD)
放弃：否决原 3 个独立轻触按键、否决摇杆
因为：用户要类似手柄的方向键、非摇杆；占用 5 个 GPIO(PC13/14/15 + PC2/PC3)

## 2026-09-14 — 灰度接口 I2C 与串口都保留：循迹排针同时引出 I2C2(PC4/PC5) 与 UART4(PC10/PC11)
选择：灰度接口 I2C 与串口都保留：循迹排针同时引出 I2C2(PC4/PC5) 与 UART4(PC10/PC11)
放弃：—
因为：用户明确两种都要留

## 2026-09-14 — 板尺寸无限制；OLED/蜂鸣器/RGB 全部保留；确认 7.4V 是舵机供电、电池仍 12V
选择：板尺寸无限制；OLED/蜂鸣器/RGB 全部保留；确认 7.4V 是舵机供电、电池仍 12V
放弃：—
因为：用户本轮明确

## 2026-09-14 — 不生成原理图/KiCad 工程，只做选型与布局辅助
选择：不生成原理图/KiCad 工程，只做选型与布局辅助
放弃：否决生成 netlist/KiCad 供导入
因为：用户说「你不用生成原理图就是辅助我就行」

## 2026-09-14 — 电机直接插主板 6P（XH2.54-6P：M+/M-/VCC/GND/A/B），电机电流过板；外部驱动输出经 KF128-4P 进板再接 6P
选择：电机直接插主板 6P（XH2.54-6P：M+/M-/VCC/GND/A/B），电机电流过板；外部驱动输出经 KF128-4P 进板再接 6P
放弃：否决「电机进外部驱动、主板只取 4P 编码器」的省电流方案
因为：用户选定要沿用 6P 接口；布局须按 ≥3A 走线、建议 2oz 铜，XH2.54 每针约 3A 是边界

## 2026-09-14 — 编码器供电 5V；A/B 输入做「直连 / 10k-20k 分压」三焊盘跳线，按所选引脚是否 FT 选择
选择：编码器供电 5V；A/B 输入做「直连 / 10k-20k 分压」三焊盘跳线，按所选引脚是否 FT 选择
放弃：否决固定直连（怕非 FT 脚）与固定分压（损失边沿质量）
因为：用户确认编码器 5V；PB6/PB7/PA15/PB3 是非 ADC 数字脚，FT 待 CubeMX 核对

## 2026-09-14 — 舵机 2 路，供电 7.4V（独立 SY8205 可调 buck）
选择：舵机 2 路，供电 7.4V（独立 SY8205 可调 buck）
放弃：否决 5V 与 6V
因为：用户明确 2 路、7.4V；LDX-218 17kg 数字舵机适合高压

## 2026-09-14 — 灰度传感器暂用现有 8 路数字（PA0-PA7），但预留 I2C2(PC4/PC5) 与备用串口，循迹接口做成通用排针
选择：灰度传感器暂用现有 8 路数字（PA0-PA7），但预留 I2C2(PC4/PC5) 与备用串口，循迹接口做成通用排针
放弃：否决现在就上 12+ 路或固定某种总线
因为：用户说灰度未定、后续可能 I2C 或串口

## 2026-09-14 — 舵机 LDX-218 用独立的「可调 buck」电源轨（SY8205FCC 12V→6.0/7.4V，5A），不接 5V 轨
选择：舵机 LDX-218 用独立的「可调 buck」电源轨（SY8205FCC 12V→6.0/7.4V，5A），不接 5V 轨
放弃：否决舵机直接吃 5V（若 LDX-218 是 6~8.4V 高电压版会没力/发抖）
因为：LDX-218 是 17kg·cm 全金属数字舵机，高电压版常见 6.0–8.4V；官方 datasheet 未取得，做成可调可兜住 5V/6V/7.4V 三种情况

## 2026-09-14 — 电机确认为 JGB37-3865-520，减速比 45:1，空载 150RPM@12V，编码器 11PPR 霍尔 AB 相（1980 计数/输出轴圈）
选择：电机确认为 JGB37-3865-520，减速比 45:1，空载 150RPM@12V，编码器 11PPR 霍尔 AB 相（1980 计数/输出轴圈）
放弃：—
因为：用户提供；据此算出 150RPM ≈ 4950 计数/s，编码器占用 TIM4/TIM2，PID 节拍需改用 TIM6/TIM7

## 2026-09-14 — 无线用外接「带无线串口的下载器」，主板不板载无线模块，改为预留 2 个备用串口（UART4=PC10/PC11、LPUART1=PC0/PC1）
选择：无线用外接「带无线串口的下载器」，主板不板载无线模块，改为预留 2 个备用串口（UART4=PC10/PC11、LPUART1=PC0/PC1）
放弃：否决板载 ESP32-C3-MINI-1（WiFi+BLE）与 nRF24L01 方案
因为：用户已有外接无线下载器，板上再放模块浪费面积与成本

## 2026-09-14 — 板载 IMU 选 ICM-42688-P（LGA-14，SPI2）
选择：板载 IMU 选 ICM-42688-P（LGA-14，SPI2）
放弃：否决继续外接 JY61P、否决 BMI088
因为：用户指定 ICM-42688-P；板载后走线短、零偏小

## 2026-09-14 — 电机型号 JGB37-3865-520（12V 直流减速 + 11PPR 霍尔 AB 编码器，XH2.54-6P），仍外接驱动模块
选择：电机型号 JGB37-3865-520（12V 直流减速 + 11PPR 霍尔 AB 编码器，XH2.54-6P），仍外接驱动模块
放弃：未选无刷/FOC
因为：用户指定；每电机 6 线：M+/M-/VCC/GND/A/B，堵转约 2.5~3.5A

## 2026-09-14 — 新主控选 STM32G474/G431 系列（Cortex-M4F，170MHz）
选择：新主控选 STM32G474/G431 系列（Cortex-M4F，170MHz）
放弃：否决 STM32F407/F411（生态最熟但封装大、价格高）、STM32H743（算力溢出、成本与布线难度高）、ESP32-S3（无线方便但实时性与抗干扰弱）、RP2040（HAL 资料少）、GD32/AT32（虽 Pin2Pin 兼容 F103 但用户接受换主控）
因为：竞赛小车控制不需要 M7 算力，G4 有 FPU/CORDIC/丰富定时器与 ADC，立创商城好买，性价比高

## 2026-09-14 — 新增功能：AB 相编码器测速、IMU 升级、无线通信、电源监测
选择：新增功能：AB 相编码器测速、IMU 升级、无线通信、电源监测
放弃：未增加更多电机通道、摄像头、更多舵机、12 路以上循迹、SPI 屏
因为：用户从候选清单中选定这四项；其余本轮不做，但新板预留扩展引脚

## 2026-09-14 — 整机供电用 12V DC 适配器
选择：整机供电用 12V DC 适配器
放弃：否决 2S/3S 锂电池组、18650 电池组
因为：用户指定；12V 直接给直流电机，板载 DC-DC 降到 5V/3.3V

## 2026-09-14 — 直流减速电机仍外接驱动模块，主板只出 PWM+方向信号
选择：直流减速电机仍外接驱动模块，主板只出 PWM+方向信号
放弃：否决板载 MOS/集成驱动（TB6612/DRV8874 等）
因为：用户选定；主板简化、散热与走线压力小

## 2026-09-14 — PCB 用立创EDA绘制、嘉立创打板
选择：PCB 用立创EDA绘制、嘉立创打板
放弃：未选 KiCad/Altium；层数与尺寸未定
因为：用户指定，元器件优先选立创商城在库型号
