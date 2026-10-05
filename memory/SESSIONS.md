# SESSIONS

> 发生了什么，什么时候。**只追加，最新在最上。**
>
> 条目形状：
>
> ```
> ## YYYY-MM-DD
> 完成：现在是真的是什么，以及怎么验证的
> 未完成：还没做完的
> 下一步：一个具体的下一步
> ```
>
> 只写「完成」而没有验证，那是主张，不是记录。没验证的就写没验证。

## 2026-10-06 · 循迹调参
完成：真车 A/B（第25/26轮，随机摆位）发现：`corr` 恒 7200 顶死、航向误差稳定在 16.8°（= 180/ang_kp）、`rate_ref` 要 271 °/s 而执行器只能给 ~110~150 ⇒ 车绕着线转圈（用户原话「贴着线走」）。逐帧证据 `out/_drv_7000_s2_cr1200rm300.txt`。机理：`rate_ref` 被 `s_rate_max` 夹住 ⇒ `corr ≤ rate_kp × rate_max = 40×120 = 4800 < base 7000` ⇒ 内侧轮永远到不了 0。
未完成：第27轮（降 `pu`/`kd`/收 `am`）被用户当场否掉：那些参数把大转弯的需求也砍了，直角完全跟不上 —— **用户的原话就是判据**。降需求这条路对本车的症状是反的。
下一步：第28轮正在跑：只改 `trace_rate_max`（120 旧行为 / 200 / 300），pivot 系数与角度环全部锁死，主要看直角工况能不能救回来。

## 2026-10-06 · 固件-验证
完成：把两个结构性改动做完并烧进真车（bin 118016 B、回读 firstDiff=none，text 117632/data 376/bss 33976）：① `app_ui.c` 的 `line_kd` 三处同步 + NVRAM 一次性迁移；② 新增 `HP_TRACE_RATE_MAX`(0x020A, 默认 300) 只对循迹生效的速率天花板（`gyro_control.c` 用 `s_trace_rate_max`、`PID_tracing.c` 外环夹子也改用它）；③ `GyroControl_CorrLimit()` 硬夹子放宽到 `MOTOR_PWM_MAX+|base|`。验证：固件主机测试 **15/15 通过**（含新加的 `t_cascade` 桩）、上位机 `pytest` **176 passed**（173+3）。烧后开机 `/api/params/reload` 读回：`line_kd=12`（迁移生效，不再是 360）、`trace_rate_max=300`、角度环 10/40/0/120/3000/3000/0.8 全对（`param_crc` 29653）。
下一步：等第28轮结果定 `trace_rate_max`；定完再把 `task_speed` 按用户要求从 6480 提到 ~7000（SPECS-only，不用迁 NVRAM）并重刷一次；然后回原地转圈复验 pivot 没被影响（`corr` 夹子放宽在 base=0 时逐位不变、`rate_max` 也没动，预期无变化）；最后提交。

## 2026-10-06 · 循迹调参
完成：物理验证「turn 正 = 左转」并落盘。做法：`out/_signtest.py`（新建，复用 `_gsweep.api/run_once`，自己解析日志）发 `base_turn base=0 turn=+60` 两趟，采 yaw/编码器 vl,vr/corr。两趟一致：yaw +60.10°/+59.50°、vl −108/−118、vr +118/+113 mm/s、corr +1500/+1623；POST 段 yaw 变化 0.0°（证明无人碰车）。用户目视 = 往左转(逆时针)。链路自洽三段：turn>0 → yaw 增 → corr>0 → 左轮倒转/右轮正转。
未完成：GUI 侧左/右直觉问题未处理（用户明确说「你不用管这个」）：`tools/console/web/src/pages/Imu.tsx:140-146` 朝向滑块 min=−180/max=+180、右端标「+180° 左转到底」⇒ 往右拖真会左转，标签方向与直觉相反。
下一步：按用户要求继续调循迹环：先修 `out/_tsweep.py`，再在 base≈7000 下跑一轮真车 A/B（先验基线能否跟住线，再看外环 `trace_angle_per_unit`/`line_kd` 是否要跟着内环 rate_kp 40 重调）。

## 2026-10-06
完成：原地转圈续调定稿：gyro_ang_kp 7→10、gyro_settle_deg 1.0→0.8（配 rate_kp 40）。台架 out/_gsweep.py --cases {ak5,fin1,fin2} 同会话交错 A/B 共 19 趟（每轮插基线、两个转向各半）：到 90% 0.67→0.50s、10-90% 0.47→0.29s，窗 p-p/换向/静默不变（~3°/0~1 次/0.11~0.59s）。验证：真车 RAM 回读（out/_poke.py --dump）s_ang_kp=10.0 / s_rate_kp=40.0 / s_settle_deg=0.8；固件主机测试 15/15 通过；上位机 python -m pytest tools/console/tests ⇒ 173 passed；ARM 构建 bin 117936 B（上限 118784）。提交 475d651（4 文件：host_link.c SPECS、gyro_control.c 兜底初值、t_hostlink.c 断言、tools/console/bridge/params.py 镜像）。
未完成：车上跑的还是旧 build（新固件未烧录），定稿值只下发到 RAM、未调 /api/params/save 存 NVRAM；gyro_ang_kp 与 gyro_rate_kp 与循迹共用（循迹航向环开环增益 7×160=1120 → 10×400=400）⇒ 赛道复验未做。
下一步：上赛道跑循迹复验；若 OK 则烧固件 + 存 NVRAM。

## 2026-10-06
完成：原地转圈「到目标角附近晃」定位并定稿：同会话交错 A/B（每 4 个候选前紧跟一趟基线、每趟交替转向，rate_kp=40 共 17 趟）⇒ `gyro_rate_kp` 160→40 + `gyro_settle_deg` 2.0→1.0：窗 p-p 6.2°→2.7~3.0°、换向 1~3→0、静默 0.5~1.0s→0.31~0.46s、90% 到位 0.48s→0.55~0.69s、末偏 −1.4~+1.1° 无偏；8s 长保持证明没有持续极限环（1.7s 之后每桶 p-p 恒 0.00°、corr 恒 0、vl=vr=0）。已同步进固件出厂默认值与上位机镜像默认表，提交 081d3aa（4 文件 +112/−22，只含本次相关文件）。验证：`python -m pytest tools/console/tests -q` → 173 passed；`pwsh -File firmware/test/run_tests.ps1` → 通过 15 / 失败 0；`make -C firmware all` → OK（bin 117936 B / 上限 118784 B）
未完成：①`gyro_rate_kp` 与循迹共用（循迹也走 `gyro_control.c:766`），循迹 P/D 是在 160 下标定的 ⇒ 赛道行为待复验 ②新固件没烧（车上跑的是旧 build；这两个参数是上一轮用 `/api/params/set` + `/api/params/push` 下发、并用 SWD 回读 RAM 校验过的），也**没**调 `/api/params/save` 存 NVRAM（掉电即失） ③工作区还有约 20 个与本次无关的未提交改动（`PID_tracing.c`、`trace_cal.c`、`app_ui.c`、`board.c`、`docs/灰度标定与检验.md`、`memory/*` 等），本次刻意没扫进提交
下一步：赛道复验循迹（先看 `line_kd`/`trace_angle_per_unit` 是否需要跟着 rate_kp=40 重调）；若冲突就给 pivot 分支单独加一个速率环增益，而不是在 40/160 之间回退

## 2026-10-05
完成：用原地对转扫了 7 组参数（ang_kp / rate_kp / rate_kd / rate_max / corr_limit / corr_slew / settle，各组 3~5 次重复、交替转向），定稿 ang_kp=7、rate_kp=160、rate_kd=0，其余不动；写进固件出厂默认值（host_link.c 的 SPECS、gyro_control.c 的兜底初值）、上位机镜像默认表（tools/console/bridge/params.py）与 t_hostlink.c 的 bounds 断言；`pwsh -File firmware/test/run_tests.ps1` 通过 15/失败 0，`python -m pytest tools\console\tests -q` 173 passed；提交 2c1afab。
未完成：固件还没烧录（源码默认值已改好；车上 RAM/NVRAM 现在正好就是这套值，所以烧完自然对齐）；车上 Flash 是否已持久化这套值没验证（要断电复位才能确认）。
下一步：等用户摆车后在赛道上复验：内环从 ang3.5/rate80/kd4 换成 ang7/rate160/kd0 后，循迹 P/D（trace_angle_per_unit=30、line_kd=12）是否还最优。

## 2026-10-05 · 编译坑
完成：定位并修掉了一个"注释里中文被当成 C 代码"的编译失败：块注释里 `**` 紧跟 `/` 会拼出 `*/` 把注释提前闭合，gcc 报 `stray '\342' in program`。定位法是扫源文件字节流：从注释起点找下一个 `*/` 落在第几行，并列出所有"前一个字符是 `*`"的 `*/`（即 `**/`）。
下一步：中文注释里写 Markdown 粗体时，绝不让 `**` 紧跟 `/`（分隔符改用 `、`）。

## 2026-10-05 · 循迹调参
完成：第 17~22 轮真车扫趟（w/v/x/q/k/m 六个系列）把内环与外环都定稿，并写进出厂默认值：`gyro_rate_kp=80`、`gyro_rate_kd=4`、`gyro_rate_max=120`、`gyro_corr_limit=3000`、`gyro_corr_ref=3000`、`trace_angle_per_unit=30`、`trace_angle_max=35`、`line_kd=12`。同步改了 `firmware/App/Svc/host_link.c`（SPECS 五项 + 实测证据注释）、`firmware/App/Svc/gyro_control.c`（`s_corr_limit` 初值 2000→3000）、`firmware/test/t_hostlink.c`（断言 250→80）、`tools/console/bridge/params.py`（镜像默认表五项）。验证：`pwsh -File firmware/test/run_tests.ps1` 通过 15/失败 0；`python -m pytest tools\console\tests -q` 173 passed。已提交 `21515a3`。
未完成：`PID_tracing.c` 的 `TRACE_ANGLE_PER_UNIT_DEFAULT 30` / `TRACE_POS_KD_DEFAULT 12` 与 `gyro_control.c` 的循迹前馈部分**没能一起提交** —— 它们的 hunk 和更早的串级重写搅在一起；固件默认值**也还没烧录**（用户说了先不动车）。
下一步：晚上/用户许可后：把 `PID_tracing.c`+`gyro_control.c` 那批（含更早的串级重写）整理成一个独立提交，再烧录固件、确认 `GET /api/params` 里 `default` 变成新值。

## 2026-10-05
完成：查清并修掉间歇性"无应答"（一直在杀掉整趟的那个 m29913）：`link.py` 抬驱动接收缓冲到 256 KB（`set_buffer_size`）+ `poll` 改成循环搬空 + 新增 `max_in_waiting`/`max_gap_ms`/`last_poll_ms` 诊断；`state.py` 的 `send_wait` 加 `retries`（只在"这条 seq 一帧都没回来"且链路还活着时重发，一次性动作留 0）；`param_push` retries=2、`set_drive` retries=1；`out/_capdrive.py` 补上"发车没 ACK 也先发停车"。验证：`python -m pytest tools\console\tests -q` → 173 passed。已提交 `499e1c5`。
未完成：车侧的 `DL_TX_SLOTS`(8)/`DL_RX_DMA_SZ`(2048) 是否抬高还没定，要等烧录；`out/` 目录被 gitignore，`_capdrive.py` 的改动不在版本控制里。
下一步：下次上车时看 1 Hz RTT 行里的 `s_dl_tx_drop`/`s_dl_rx_overrun` 有没有非零，再决定要不要扩队列。

## 2026-10-05
完成：两个前端 bug 修好并提交 `710c09c`：① `lib/ring.ts` 的抽稀相位改成锚**绝对样本号**（原来环绕回后相位退化成"物理下标 ≡ push 次数 (mod step)"，每帧整条线换一批样本）；② 姿态两路按车体轴对调（JY61P 横装 ⇒ 传感器 pitch 量车身横滚、roll 量车身俯仰）。验证：`npx vitest run` 194 passed / 8 skipped；`npm run build` 干净。
未完成：`git status` 里还有一大坨更早的未提交工作（`trace_cal.c/h`、`app_ui.c`、`board.c`、`nvram.c`、`pid.c`、`kk_font_body.c`、docs、memory、`tools/console/docs/API.md`）—— 故意没动。
下一步：由用户决定那坨怎么提交（它还和 `PID_tracing.c`/`gyro_control.c` 的默认值改动缠在一起）。

## 2026-10-05 · 循迹调参
完成：第 22 轮（P=10/20/30 各 5 趟）的结论已用**整齐摆位**的 m10~m15 六趟（用户确认是他改了摆法）复核：P30 追得最紧、P20 抖动最小、P10 最差 ⇒ `trace_angle_per_unit` 保持 30。前 9 趟的丢线（限幅%rate 27~100%、丢线 20~77%）已定性为**摆位/起点航向**造成，不是参数问题。
未完成：第 22 轮前 9 趟那批数据在"干净摆位"口径下作废。
下一步：无需再扫 P；若以后还要提鲁棒性，先解决"起跑前 0.5s 跟不上弯道"（基准速度 7000 偏高）。

## 2026-10-05
完成：灰度判线按用户 m31902 整改完并实车验证：删掉单帧自适应（s_base/s_shift/s_dr/jolt/minlvl，代码净减 1312 字节），改成每 20ms 一帧算 `d = raw-阈值`、`lvl = 128 ± 255*|d|/跨度`（128 = 正好在阈值上 = 0/1 判定线），判据只剩「有一路越过阈值 = OK，一路都没有 = WEAK」；PID 权重只取越过阈值的部分。验证：15/15 主机测试（t_tracecal 191 项）、`make -C firmware -j8` text 116672/bin 117048 烧录回读一致、J-Link 直读 idle 帧手算逐路对上（ch5 raw328/thr294 ⇒ off=61 ⇒ lvl=67 = s_fl_lvl[5]）、驱动实测 base600/3s 丢线 0/610（改前同参数 24~88%）。
下一步：循迹 PID 实车整定（用户 m31048 已放行）；带速循迹读帧抖动继续观察（本轮已 0/610）

## 2026-10-05
完成：菜单「自动判线」改名「连续判线」并用 `node tools/font/gen_font.mjs firmware/App/Fonts/kk_font_body.c wenquanyi_9pt 12 kk_font_body 连续判线` 重新生成字模（旧字模只缺 续 U+7EED；生成前自动备份 .bak、生成后自动捡回 #include；check_charset 复核缺字 0，含上次 814 字的整份字符集也缺字 0）；OLED 截屏（out/_scr_settings2.png）确认设置页第三行显示「连续判线 开」。顺手修掉 `out/_lcd.ps1` 的选面 bug：必须一个 J-Link 会话里把两个显存面都 savebin 再挑有内容的（halt 落在 kk_oled.c:95/:142 的 memset 之后时，stable/transfer 下标指的是刚被清空的那一面）。
下一步：docs/灰度标定与检验.md §3 已按新口径重写（并修掉 Get-Content/Set-Content 造成的 LF→CRLF 污染，trace_cal.c 也被污染过一次）

## 2026-10-05
完成：把 `docs/灰度标定与检验.md` 的 §2 检验视图末行表、§3（原「单帧自适应」整段 → 新「标定阈值 + 离阈值的距离」+ 几何缓存/实时性 + 本车实测）、§5（补 gen_font.mjs 重生成流程）、§6（`out/_base_read.jlink` 与 `out/_lcd.ps1` 的新口径）全部同步，LF 结尾保持（`git diff --stat` = 124 insertions/182 deletions，与 --ignore-cr-at-eol 一致）
下一步：无

## 2026-10-05
完成：用户 m30822 报的两个 bug 已修完、验证全绿、未提交：①环形缓冲抽稀相位从「环内逻辑下标」改成锚绝对样本号（ring.ts 新增单调 seq，start 现在是绝对样本号、恒 start%step==0，view() 多回一个 endT），新增回归用例钉死 [32,36,40]（旧实现给 [33,37]）；②波形页纵轴改用 nextRange 滞回包络、x 轴钉在窗口右端；③channels.ts 两路姿态对调（俯仰←roll、横滚←pitch），Imu.tsx/Overview.tsx 读数与三处文档同步。验证：tsc exit 0、vitest 194 passed/8 skipped、npm run build 干净，桥已托管新包 index-9C3Dz9W7.js（响应带 no-store，刷新即生效）。等用户刷新确认后再提交
未完成：未提交（工作树有改动，HEAD 仍是 b965584 之后的 c496609）
下一步：用户确认界面后提交这两个 bug 的修复

## 2026-10-05
完成：灰度模块恢复健康，用 J-Link 实测确认（当前构建 firmware/build-app/haixin-app.elf，2026-10-05 09:58）：trace_cal_frame_dbg.state=1(TRACE_FRAME_OK)、analog_ok=1、src=2=TRACE_SRC_UART（AUTO 下串口优先）、trace_cal_mode=0(AUTO)、trace_cal_valid=1、s_fail=0、s_off_until_ms=0；raw=349/381/399/241/244/310/361/351（最黑 ch3/ch4），黑度 0/0/20/196/251/189/26/0，trace.pos(data)=3.4175=4.5 偏左一格；trace_adapt_enable=1（单帧自适应开着）
下一步：用慢速档跑一次串级循迹，40ms 快采样看 pos/angle.err/corr/pwm_l·pwm_r，判定「还是直行」是死区正常还是转向没送到轮子

## 2026-10-05
完成：坑：idle（mode 0）时没人调用 read_tracing_data_pid()，所以遥测的 trace.pos 与 flags.lost 是最后一次循迹运行的残留值 —— 传感器健康、gray 是活的，遥测仍可能显示 lost=true。判断真实链路要看 trace_cal_frame_dbg.state/analog_ok/s_fail，不要看遥测 lost
未完成：残留标志要不要在停车时清掉/标注（未定）

## 2026-10-05
完成：按用户 m30484 定档把上位机链路切到 **921600**（我上一轮「921600 做不到」的结论作废：那是旧探针 PID_0105 的结论，车上现在插的是第二颗 OB-RA4M2-Full、PID 1024、COM27）。改动：`firmware/Core/Src/board.c:581` `DL_BAUD 921600u`；`firmware/App/Svc/host_link.c:1087` 遥测默认 100→**200Hz**；桥/网页/启动器/文档默认值全切 921600（`bridge/link.py`/`main.py`/`server.py`/`state.py`/`bench.py`/`launch.ps1`/`web/src/lib/api.ts`/`Settings.tsx`/`Overview.tsx`/`mock/server.mjs`/`docs/API.md`/`启动上位机.bat`）；`bridge/serial_safety.py` 整文件重写成**按探针 PID 分档**（`J_LINK_MAX_BAUD_BY_PID` 0105→230400、1024→3000000，认不出按 230400 保守拦）。验证全绿：固件主机测试 15/15、`pytest` 173 passed、`vitest` 193 passed + 8 skipped、`npm run build` 干净、烧录回读一致 118380 字节。真车实测：`bridge.probe @921600` 2s = 410 帧 / 0 CRC / 0 重同步；`bridge.bench --attach 8770` = 实收 **200.6 帧/s**（链路喂 201.1、丢 4 帧）、SSE 35.5 事件/s / 140.3 KB/s、链路 0 CRC / 0 重同步 / 0 丢帧、rtt 5.4~5.8ms；460800/230400 只收到乱码（板上只说 921600，符合预期）。车全程没动（mode 0 / pwm 0,0 / lost true），烧录没碰 nvram（`/api/cal` 仍 quality 8）。桥已重启为 PID 45268（`--baud 921600`）。
未完成：本轮（含前两轮）改动仍未提交；`docs/上位机通信协议.md` §8.2 已用今天实测数字改写；用户还没在 8770 刷新页面看新界面
下一步：问用户要不要提交这三拨未提交改动（上位机修复 m30228 / 循迹丢线修复 m28949 / 本轮 921600+200Hz），以及遥测默认 200Hz 留不留

## 2026-10-05
完成：按用户 m30228「好好检查好好搞一搞」把上位机查了一遍并修完：①修掉“设了 200Hz 还显示 100Hz”的根因——`tools/console/bridge/state.py` 的 `stats_payload()` 现在回实测 `telem_hz`（`self.measured_hz`，`_maybe_stats()` 的 1s 窗口算出来存下来）+ `telem_hz_set`/`telem_tx_hz`/`loop_us`，`/api/link` 保持设定值；②网页 `types.ts`/`store.ts`（不再把实测写回 `link.telem_hz`）/`Control.tsx`（滑块初值 50→100、显示“实测 / 设定”）/`Overview.tsx`/`Imu.tsx`/`Settings.tsx`（新增实测-设定行、环形缓冲时长按实测算、波特率提示重写为“唯一真值是固件 DL_BAUD=230400，J-Link VCOM 上限也是 230400”）/`docs/API.md`；③「车上控制占用」→「界面刷新占用」；④新增 pytest `test_stats_hz_is_measured_not_setpoint`；⑤把两个会打真车桥的 web 测试改成 `CONSOLE_BASE` opt-in。验证：pytest 168 passed、tsc exit 0、vitest 默认 19 文件 193 passed + 8 skipped 0 失败、带 mock 桥那 2 个文件 8/8、`npm run build` 干净；重启 8770 的真车桥后抓 SSE：`telem_hz:99/100, telem_hz_set:100, loop_us:4129 与 10, busy_pct:82.6 与 0.2` —— 实测帧率真的看得见了。车全程没动（`mode=0 pwm=0,0 motor=False lost=True vbat=11718`）。另外查清他记忆里两件事都真做过：200Hz 是**滑块上限**（2026-10-03 100→200，固件默认仍 100 且只存 RAM）、波特率 2026-10-04 已**全线统一回 230400**（板上 DL_BAUD + 探针 VCOM 上限）。
未完成：改动未提交；用户说的“波特率换了”具体想换成什么还没问到（他可能是想 921600，那要换普通 USB-TTL 接 U12）；网页要他在 8770 刷新页面才看到新界面。
下一步：把这轮改动提交（固件循迹修复 + 上位机修复两批，或分开两次提交）；问用户要不要把固件默认遥测频率从 100 改成 200（一行 `host_link.c:1087`，需重烧）。

## 2026-10-05
完成：修掉用户 m28949/m29239 点名的四件事并提交 b965584：①任务块 SET_MODE 同模式会把速度环永久关死（跑循迹不动 / 7200 不动）②停车命令停不下来（drive/stop、tasks/stop、cmd stop 全无效，只能拔电池）③拔电池再上电自己跑（新增母线低压自动解除武装）④遥测 v_l/v_r 恒 0。验证：主机测试 14/14 全绿（t_hostext 131 项、t_motion 91 项，含两个新小节），变异检验证明两处修复都被用例钉住（去掉任一处都红），make -C firmware -j8 无新告警（bin 118136B）、check_warnings 待处理 0、check_static 0 error，已用 tools/flash.ps1 烧车（回读一致），真车实测 POST /api/drive {wheels,1200,1200} 后 POST /api/drive/stop ⇒ 连续 6 个采样 pwm 0/0、motor=False、vl/vr=0（修前是 pwm 3200~4110 一直跑）。文档 docs/自主优化记录.md §20。
未完成：循迹 PID 实车整定没做完：一次 trace base=1200 的 1.5s 实测里 trace.pos 稳定在 4.22~4.76（在线上）、gray 居中，但角度修正周期性打到 ±2000 上限把左轮打成反转（pwm -2500/-4900），还不知道是正常抖动还是过激。
下一步：让用户把车放回线上，用 40ms 快采样同时看 trace.pos / angle.err / angle.rate_ref / angle.corr / pwm_l·pwm_r，再决定降 trace_angle_per_unit(10°/格) 还是降串级 corr 上限

## 2026-10-05
完成：踩到并记录：本仓库所有文本文件都是 LF，用 PowerShell Get-Content/Set-Content 改文件会把整个文件写成 CRLF，git diff 于是显示整文件改动（docs/自主优化记录.md 出现 1822 行假改动，用 git diff --ignore-cr-at-eol --stat 才看出真实差异是 34 行插入）；改法改成 [System.IO.File]::WriteAllText + 把换行归一化成 LF 之后 diff 就干净了。另外 pwsh 没有 <<<，提交信息必须写文件走 git commit -F。

## 2026-10-05
完成：用户 m26176「凡提到状态一律带名字、别丢裸编号」已提交 db7cf9d（7 files, +65/−39）：taskedit.ts 的 stateLabel(i,names) 改 export，传了 names 但状态没名字时写「状态 3（没名字）」；Tasks.tsx 的主页提示/运行监视/状态标签 title/块库提示全改走它，状态卡片头部那个编号标 data-own-state 供审计脚本跳过；task.ts 四条校验文案（跳到不存在的状态/超时改道/两轮速度/主页越界）改带名字。验证：tsc --noEmit 干净；CONSOLE_BASE=http://127.0.0.1:8788 npx vitest run = 21 文件/201 条全过；npm run build 重建 dist（index-DSkQ9Ytr.js）；node out/staterefs.cjs = 裸编号 0 处 + console 错误 0（有裸编号就非零退出）；出图 out/staterefs-waitcond.png。坑：用户截图里那串「状态 0…14」是改动前的老 bundle（SPA 不会自己刷新、带名字的 stateLabel 是上一个提交 db06be7 进的）——他再报裸编号先让他硬刷新 / 看 dist 哈希。2026-10-05 他问「改完了吗」时重跑 staterefs.cjs 复查，仍是 0 处裸编号。
下一步：任务页里「主页」是否改叫「起始」待用户定（要连带 task.ts 校验文案与 4 个测试）

## 2026-10-05
完成：第十三轮（用户 m26712 的 ②③）：修「跑任务时屏幕像暂停刷新、8 路数字都显示 1」与「灰度丢线后保持住之前的状态」+ 找回线。改动：trace_cal.c/h 加 s_hold_lvl[8]/s_hold_ms/s_hold_valid + hold_ready() + 三个新口 trace_cal_disp_level/disp_bit/held，trace_cal_channel/level 的 !analog_ok 分支不再回落（数字模式仍回落）；main_app.c 显示改吃 disp 口；app_ui.c 循迹页数据源槽与任务页标题行加「丢线」标记（任务页的量宽排挤用 OLED_GetUTF8Width，不能写死 x）。验证：firmware/test/t_tracecal.c 新增第 13 节（好帧/WEAK/断链+超时），`run_tests.ps1 -Only tracecal` 128 项 0 失败、全套 14/14；make 无新告警（bin 117820 字节）；check_static 0 error、check_warnings 待处理 0；字模检查 0 缺字；tools/flash.ps1 烧车 verified + J-Link 回读一致；烧后 /api/state gray=0,0,0,168,251,150,0,0 而 line 仍全 1（证明界面已不吃数字口），抓显存 out/_task.png 确认任务页「任务 2/2 测试」+8 柱 +0/0/0/1/1/1/0/0 排版没撞；RTT FRAME avg≈830us max≈4400us、ISR 9us over=0、i2c recover=0 disp_fix=0；桥未断（rx 153893）。提交 8d1ffa7（6 files, +223/-13），文档 docs/自主优化记录.md §19。
未完成：真车「断链」场景（拔灰度模块的线、或把灰度接口选成不存在的源）下，「丢线」标记与柱按住的复现要用户配合——桥的 /api/params 里没有 gray_mode，无法远程切；①「寻迹 PID 停下无速度差」仍等用户说口径。
下一步：让用户拔掉灰度模块线后看任务页：新版应先按住 0.5s 再清空柱并出「丢线」，旧版会 8 路全 1。

## 2026-10-05
完成：修掉用户 m26333「车上屏幕有时候特别卡」。两条根因：①`firmware/Core/Src/board.c` 的 `board_i2c_bus_recover()` 结尾调 `board_i2c_init()`（= 100kHz），而开机配的是 400kHz ⇒ 每次总线恢复都把 I2C1 降到 1/4 速、无人调回，屏从 ~20 帧/s 掉到 ~5 帧/s（只有发生过恢复的车才慢，断电重启才恢复）；②`kk_ui.c` 的 `kk_ui_latch_display()` latch 后 `KK_UI_Update()` 每拍直接 return（不画、不处理按键），恢复入口 `KK_UI_RecoverDisplay()` 全工程无调用者。改法：board 记速率并回落同一速率 + 新增 `App/Svc/disp_heal.{h,c}`（300ms 重试、第 3 次顺手复位总线）+ RTT `FRAME:` 行加 `i2c=%luHz disp_fix=%lu` + `t_dispheal.c`。验证：`make` text 116828/data 380/bss 34016、bin 117352B；`run_tests.ps1` 通过 14/失败 0（t_dispheal 200039 项检查）；`tools/flash.ps1` 回读一致 117352B firstDiff=none；RTT 实测 `FRAME: avg=823~840us max=4189~4413us | oled tx_done=20 err=0 busy=0 st=0 | i2c=400000Hz disp_fix=0`、`ISR avg=9us over=0 | i2c recover=0`、`HOST crc=0 resync=0 hz=100`；线上字节（`out/rawtelem.py`）证明 `app_ui_tick` 空转 ~10µs、真画帧 ~4.1-4.3ms。提交 bf154db，文档 `docs/自主优化记录.md` §18。
未完成：「总线恢复」这条路径没有在真车上复现（只验了正常态 400kHz / recover=0 / disp_fix=0）；4.2ms/帧的 UI 绘制成本（约 20% CPU、屏 20 帧/s 的上限来源）没优化
下一步：若用户再报屏慢，先让 RTT 的 `i2c=` / `st=` / `disp_fix=` 三个字段说话：i2c 不是 400000Hz 说明还有别处在降速，st≠0 或 disp_fix 增长说明是传输/latch 路线

## 2026-10-05
完成：上位机文案全面人话化并提交 db06be7（11 files, +867/-350）：tasks/edit 的 p0~p3、flags.bit0、0xFFFF、tracing_cross()、Motion_GetFault() 全部消失，字段下面常显一行说明、label 与下拉各 option 都有 title 探针说明、块库卡片列「可设置：…」；IMU「固件能力位 0xff(bit0…）」换成能力名列表；调参页固件键名/编号挪进悬停。验证：npx tsc --noEmit=0；CONSOLE_BASE=http://127.0.0.1:8788 npx vitest run = 21 文件/201 条全过（含新增文案护栏，正则禁 p0-3/flags./0x../bitN）；npm run build 重建 dist（index-CCF5y8Pa.css + index-qOQp3XtG.js）；out/taskdocs.cjs 扫 10 页 + 展开等条件块 = 内部名字 0 命中、只剩参数指纹 0xB157/校验和 0xA38C、errors=0；截图 out/tasks-waitcond.png
未完成：任务页里「主页」这个叫法还没改成「起始」（改它要连带 task.ts 的校验文案与 4 个测试，已作为备选留给用户定）
下一步：等用户读完 docs/旧全程任务对照讲解.md 后反馈看不懂的那句；他第一次上板时盯拐弯方向（旧代码 TurnLeftFixed/TurnRightFixed 名字与行为相反，转换按名字翻、未实车验证）

## 2026-10-05
完成：教训（写下来免得再犯）：改前端文案后只跑 tsc/vitest 不够——8770 的桥服务的是 tools/console/web/dist，必须先 npm run build，否则 out/taskdocs.cjs 抓到的还是旧文案（第一次跑就踩了，页面仍显示 p2/p3 与 0xFFFF）
下一步：以后改前端可见文案：build → 再跑浏览器验收脚本

## 2026-10-05
完成：新增 docs/旧全程任务对照讲解.md（旧 Haixin/Hardware/renwu.c 的 void A() 27 段 ↔ tools/console/web/src/lib/tasks/renwu-lap.json 15 状态 190 块 2127B 逐段对照，含旧命令词典、black_num 真相、八路全白、Fixed 函数名字反了、死段 8/12/14/15/16/20/22、单任务上限 4096）；docs/任务编程.md §6 补界面文案约定与本文档链接
下一步：用户若要真跑减速带，S3 现在是空壳（旧代码那段本来就没有任何动作）

## 2026-10-04
完成：两颗探针在 1 MB/次 停机读下的完整速率对比：探针 #1（J-Link PLUS，S/N 602711039）最佳 651.8 KB/s @12000 kHz（20000/30000 kHz 反而退化到 79.6）、写 116904 B 用 16.8 KB/s；探针 #2（上报 SAM-ICE，S/N 24060505，实为克隆 OB）@100000 kHz 达 2056~2072 KB/s、写 39.6 KB/s；USB Hub 直插对比差异 ≤2% ⇒ 天花板在探针自身（≈2.07 MB/s ≈ 24 MHz 等效 SWD 时钟）。物证 out/probe_compare.md、out/_bench_samice*、out/_bench_direct、out/_bench2.ps1
下一步：用 TestRSpeed 交叉验证读速（用户 m00577/m00581 的遗留问题）

## 2026-10-04
完成：定性"车上任务 0（常态循迹）整条消失"：22:56:01 的 dump 里 122 B 任务还在，22:57:42 的 dump 里 0x1D000..0x1FFFF（目录块 29 + 程序块 30）全 0xFF ⇒ 一次 TaskStore 擦除完成后被打断、编程未开始；J-Link 自己的日志证明我的写测只覆盖 0x08000000..0x0801CFFF（块 0..28）且 `Skipped`，与我无关；task_store.c 里只有 host_link.c:988/1018 两条命令路径会擦块 29/30，TaskStore_Init() 不碰 Flash（已排除"上电格式化"）。已用桥 API（put×2 + commit）原样恢复任务 0，/api/tasks/get 回读 byte-diff=0，/api/tasks 显示 id 0 / len 122 / crc 61751 /「常态循迹」
未完成：发起者未定：桥不记录 HTTP 任务命令，console_20261004.log 里只有遥测行
下一步：问用户 22:56 前后有没有在网页上点「删除」或「烧写到车」

## 2026-10-04
完成：任务 0 字节码解码：state0「常态循迹」= SET_MODE(3) → SET_SPEED 6480 → WAIT_COND CROSS 3000 ms → GOTO 0；state1「路口转向」= WAIT_MS 200 → TURN_DEG(fl=1, 2000, -90°, 1.0°) → GOTO 1（自环，无回 state0 的路）。firmware/App/Svc/task_run.c:162-181 的 TURN_DEG 不读 flags、不改 s_base，motion.c:150-153 的 MOTION_GYRO 会沿用 state0 留下的 6480 ⇒ "边转边走"，与用户 m00117「执行任务一直往前走、不循迹」一致
未完成：实车确认（任务已恢复，可直接跑）
下一步：跑 out/_watch_task.ps1 记录 state_id 是否 3 s 后 0→1、mode 是否 3→1

## 2026-10-04
完成：排除误判：参数表是默认值（param_crc 45399、44 项）、标定完好（quality 8/8 路 ok）、自适应帧状态 OK（state=1、cal valid/analog_ok=1）、遥测 line(8 路) 走的是数字口不是循迹环信号（模拟量在 gray）
下一步：按 out/_watch_task.ps1 实车取证后再改

## 2026-10-04
完成：桥以 `python -m bridge.main --port 8770 --baud 230400 --serial COM27 --no-browser` 常驻（workdir tools/console），当前 rx 44406 / tx 16 / crc 0 / resync 0 / drops 0 / telem_hz 100 / rtt 120 ms，固件 quandi-car 0.2.0 proto 2 caps 255

## 2026-10-04
完成：新探针 VCOM 速率实测完成：用 J-Link 临时改 LPUART1->BRR(0x4000800C)（纯寄存器写、复位还原）逐档验证，确认 230400~3 Mbaud 全通、帧长 71B；真起桥 @3000000 + set_telem_hz=200 得 rx=201.3 帧/s、crc=0、resync=0。期间误报『链路变噪声』，用桥自带 FrameDecoder 解码 2 秒抓包（28719B/405 帧/crc_errors=0）证伪。已把测试时设的 telem_hz=200 复位为 100，桥在 COM27@230400 恢复（rx +304/3s=101Hz、crc=0、resync=0）
未完成：是否把链路默认速率正式提到 460800 或 921600（待用户定）；『执行任务一直往前走、不循迹』的实车记录仍未做（主电已上 vbat≈11.9V）
下一步：用户定档后：改 firmware/Core/Src/board.c:561 DL_BAUD + tools/console/bridge/link.py:57 / main.py:56 / launch.ps1:17 默认 baud + serial_safety.py:27 按 PID 放行新探针（PID_1024 放行、PID_0105 仍卡 230400），重编译烧录

## 2026-10-04
完成：查清「超过支持上限」弹窗并恢复被它连带影响的 CPU：弹窗 = JLinkARM.dll V6.82c 自己的速度检查（`JLINKARM_SetSpeed(120000 kHz): Maximum JTAG speed exceeded. Did you specify speed in Hz instead of kHz ?`），探针 #2 上限 100000 kHz（100000 静默通过 2056 KB/s；110000/120000/150000/200000 全部弹模态框卡住进程，我杀掉进程后对话框随之消失 ⇒ 「弹一下就没了」是我造成的）。验证无副作用：脚本在 speed 即中止、无任何 loadbin/savebin、param_crc 仍 45399、无残留 JLink 进程。真副作用是 JLinkGDBServerCL 一连上就 halt CPU，我杀进程后 CPU 留在停止态（表现为桥 rx/fw.t 冻结），用 out/_resume.jlink 复位 + `go` + 清 DEMCR 恢复（E000EDFC=0，rx +305 B/3 s、fw.t 2 s 走 2019 ms、vbat 11903 mV）。
未完成：用户是否要装新版 J-Link 软件（建议先 7z 解包免安装试跑）
下一步：若要试新版：下最新安装包 → 7z 解到临时目录 → 用其中 JLink.exe 对同一颗探针复测 1 MB 读带宽，再决定是否正式安装

## 2026-10-04
完成：用户换上第二颗探针后完成测速与两颗对比，报告 out/probe_compare.md（1MB 读扫描 12 档 + 256KB 对照 + 不停机 + 强制真烧写 39.6 KB/s，读回与 haixin-app.bin 逐字节一致）。三个坑已验证：①内容与镜像相同时 J-Link 跳过编程却报成功（0.48s 空跑），要测写速必须先烧垃圾镜像再烧真镜像、用两点法算；②单次 savebin 有 ~154ms 固定开销，256KB/次最多测到 1.05 MB/s、1MB/次才到 2.0 MB/s（真实传输率约 2.8 MB/s），两探针必须在同读长度下比；③换探针后 J-Link CDC 换名（COM26→COM27），桥必须换口重启。
未完成：「执行任务一直往前走、不循迹」仍未实车确认（主电 vbat=447mV 未上）。
下一步：等用户把主电接上、车放到线上后，跑 out/_watch_task.ps1 记录任务 0 的 state/t_flags/mode，确认「执行任务一直往前走」是不是 state0 的 WAIT_COND 3 秒超时跳进 state1（路口转向）后不再回循迹。

## 2026-10-04
完成：查明「车上没应答（0.6s 超时）」真因：用户换第二颗探针（SAM-ICE S/N 24060505）后 J-Link 的 CDC 口重新枚举，COM26 整个消失、新口变 COM27，而常驻桥仍抱着死句柄。证据：停桥后用 SerialPort 开 COM26 报 'Could not find file COM26'；直接听 COM27 @230400 收到 21592 字节/3s（帧头 A5 5A 03 00）⇒ MCU 一直活着。已把桥以 --serial COM27 重启为后台任务，验证 link.rx 正常增长、resync=0、fw.t 一秒走一秒。另清掉一个台架遗留的 JLink.exe（PID 39792，100% 烧 CPU 并占住 SWD）。
下一步：等用户上主电（现在 vbat=495mV）并把车放线上，启动任务，用 out/_watch_task.ps1 记录 state_id/超时位/mode 确认「3 秒跳出循迹」这条

## 2026-10-04
完成：解析车上任务 0 字节码（122 字节，crc 0xF137，新核对对齐正确）：state0「常态循迹」= SET_MODE 3 + SET_SPEED 6480 + WAIT_COND(CROSS,超时3000ms→跳state1) + GOTO 0；state1「路口转向」= WAIT_MS 200 + TURN_DEG(flags=1 相对 -90° 容差1.0° 超时2000ms) + GOTO 1。⇒ 干净的直线上 3 秒必超时跳出循迹，而 state1 没有回 state0 的路，且 TURN_DEG 不清基准速度（motion.c 的 MOTION_GYRO 分支把当前 s_base 当真速用）⇒ 表现为「一直往前走、不循迹」。已核对 flags.bit0 的语义是「p1 是相对当前朝向」（web/src/lib/taskedit.ts:101 + pages/Tasks.tsx:123），不是原地转。
未完成：H6 只差实车记录确认（state_id 是否 3s 后变 1、任务 flags bit0 是否置上、mode 是否 3→1）

## 2026-10-04
完成：第二颗探针（SAM-ICE S/N 24060505，CDC=COM27）开始按探针 #1 同一套流程测速率，脚本 out/_bench2.ps1：每档速度先跑 base（connect/halt/go）再跑 meas（同前 + 4 次 savebin 256KB），纯读时间 =(meas-base)/4；另含 info 横幅、最好档复核 3 次、不停机对照、Export-Csv。

## 2026-10-04
完成：用户 m25581「不要文字背景」改完并提交 **8a54bed**（6 files）：`tools/console/web/src/index.css` 的 perf-lite 段 —— 侧栏/卡片 `background-color: transparent`（卡片连 background-image 高光与 shadow-card 一起去掉，只留 `.glass` 的 `ring-1 ring-white/10` 分组），顶栏必须不透明所以改成**画 body 自己那三张 radial-gradient + `background-attachment: fixed` + 底色 #0b1020**（与 body 辉光逐像素对齐），`html.is-scrolling` 同步；删掉 #0d1428/#121a33/#141d3d。`perf.test.ts` 的"低特效是有层次的"换成"低特效下没有文字背景"（断言 transparent、旧三色去注释后不许出现、顶栏 radial-gradient 列表与 body 逐字相同 + fixed）；`docs/API.md` §5 设计行改口径。验证：`npx tsc --noEmit` 0；vitest **21 文件/200 条**（跑在 8788 mock 上）；`node out/looklite.cjs` 全过 —— **藏掉 header 后同一块像素最大通道差 1/255、藏掉 aside 0/255**（融合的硬证据），默认 perf-lite + backdrop 全 none + 脉冲 none，关掉开关回到 blur + pulseDot；`node out/allpages.cjs` 10 个页面各截一张 `out/pages/*.png` 零 console 错误；dist 重建 `assets/index-BFr2K83H.css` + `assets/index-DlTzi0AN.js`。
未完成：① 新观感（卡片只剩一圈 1px 亮边）待用户确认；他若嫌那圈线也是"方框"，去掉 ring 是一行，perf.test.ts 那条护栏要同步改。② 仍是暗底的"文字背景"没动：控制与日志页的日志框/ACK 框（`Control.tsx:42,55` 的 `bg-ink-900/60`）、操控页读数小方块（`Drive.tsx:242-288` 的 `bg-ink-900/50`）、任务页编译输出块（`Tasks.tsx:120,354` 的 `bg-black/20|10`）、波形/迷你波形屏（`ui.tsx:273,312` 的 `bg-ink-900/80`，这个是"屏幕"观感，建议保留）—— 他说的"文字背景"若指这些，得再抹一遍。
下一步：让用户刷新 http://127.0.0.1:8770 看总览/任务页；若他指的是日志框那类小暗块，报一句就一起抹平。

## 2026-10-04 · KK_UI 界面
完成：用户 m24999 的两件事都落地并烧进真车（提交 7d08bb5，25 files +670/−428）：①车上任务页重做 —— 标题行 = 状态词（任务/运行）+ 选中序号 i/N + 最右端任务名（右对齐到 x=122，UTF-8 截宽），y=14 分隔线以下是整块循迹扫描状态（与循迹页共用同一份 draw_trace_bars()：8 路灰度柱 + 每路 0/1 + XOR 阈值线），上/下 选任务（绕圈跳空槽）、中键 跑选中的/正在跑就停、左键 返回，进页面时解释器在跑就自动选中在跑的那个，空车打「车上没任务」；②「开机自启」整个删掉 —— main_app.c 上电只扫描不启动，task_store 删 TASK_STORE_FLAG_AUTO/TaskStore_AutoSlot、flags 变保留位原样往返，桥/仿真器/mock 不再掩码 bit0，Tasks.tsx 删开关+徽章+toast，docs/任务编程.md、docs/上位机通信协议.md、memory/PROJECT.md 同步。验证：run_tests.ps1 通过 13/失败 0（t_taskstore 560 项检查 0 失败）；make 干净（text 116520/data 376/bss 34008）；pytest 167 passed；tsc --noEmit 干净；vitest 21 文件/200 条全过；dist 重建（index-CIPzNBCO.js 344.68kB + index-W9H3ASIu.css 39.45kB，新 bundle grep「自启」0 命中）。真车：tools/flash.ps1 回读逐字节一致（116904 B，firstDiff=none，页 58/63 未擦）；复位后 /api/tasks/status = running=false id=65535 ⇒ 上电不自启；车上目录仍是用户那条 #0「常态循迹」122B flags=0；/api/cal 8 路 quality=8 全 ok、阈值与烧前一致（nvram 页 63 完好）；/api/params 44 项 param_crc=45399（含 0x0112=3000）；rx 持续增长 crc=0 resync=0；J-Link 抓屏 out/taskpage.png 是任务页真实版面（任务 1/1 + 常态循迹右对齐 + 8 柱 + 0/1）；网页只读探针 out/pwtasks.cjs 确认「开机自启」字样已消失、车上现有列出 #0、零 console 错误
未完成：「运行」态版面与中键启动/停止只由代码与字形检查覆盖，没在真车上按过中键（按了会真的开车）；屏上观感与按键手感只有用户能验
下一步：请用户在车上任务页按下/上选任务、看标题栏最右的名字，再按中键跑他那条常态循迹（离线、不接电脑），要停再按一次中键；刷新网页看任务页已无「开机自启」开关

## 2026-10-04
完成：用户 m24215 的三条诉求全部改完并验证：①固件新增 `gyro_corr_ref` 让差速天花板随基准速度放大（nvram 升 v6 保住标定）②桥/前端把「每帧弹 HTTP 200」改成说人话的错误 + toast 去重 ③低特效当默认并把面板改成不透明分层主题。验证：`firmware/test/run_tests.ps1` 13/13（t_gyrotest 48044 检查、t_nvram 47 含新的 v5→v6 升级用例）；`tools/console` pytest 167；web `tsc --noEmit` 0 + vitest 21 文件/200 条；真车 J-Link 烧录回读逐字节一致（扇区 63 未擦）、复位后遥测恢复、桥 44 项参数 `param_crc=45399` 与车上一致、`0x0112=3000` 在位、`/api/cal` 与 `/api/actions` 完好；浏览器验收 `node out/looklite.cjs` 全过（默认挂 perf-lite、三层不透明有层次、无纯黑、关掉开关回到模糊+脉冲），截图 `out/lite-overview.png` / `out/lite-params.png`。提交 4452987。
未完成：满速「前进+左转」的真实转弯半径只能由用户在真车上按 W+A 复测（给车发非零速度是被禁止的，没法自证）；低特效主题在用户屏幕上的主观观感也待他确认。
下一步：等用户复测结果；若仍嫌转弯半径大就调 `gyro_corr_ref`（调小=放大更猛，0=退回旧行为）

## 2026-10-04
完成：用户 m03559（「我上下滚动就能感觉到这个画面是卡卡的」）的滚动流畅度优化已落地并提交 **250e652**（12 files, +582/−236）：新 `tools/console/web/src/lib/fx.ts`（滚动状态机：`is-scrolling` 挂/140ms 摘、`perf-lite` 常开两路）+ `index.css` 末尾关 blur/脉冲、header 转不透明 + `ui.tsx` 灰度柱 `transition-[height]`→`transition-colors` + `store.ts`/`Settings.tsx` 新增 `perfLite` 开关 + `fx.test.ts` 6 条 + `perf.test.ts` 2 条源码级护栏，dist 已重建（`assets/index-CXed0o4p.js` + `index-I5KCaTTu.css`）。验证：tsc 0；vitest **21 文件 194 条**全绿（跑在 8788 mock 上，contract/ssrPreview 未 skip）；bridge pytest **165 条**全绿；`out/pwshot.cjs` 实测滚动中 html class=`is-scrolling` + header backdrop none + 底色 `rgb(7,10,22)` + pulse 动画 none、松手 0.5s 全恢复、页面零 console 错误；同一套 bundle 的 A/B（`out/pwviz.cjs --damage --dmgms 4`，blurback 变体把模糊强行加回来）：合成器+raster 总耗时 1296ms → 263ms（每帧 2.17 → 0.54ms，4.9×）；逐页复量总览 layoutN 643→54 / taskPct 24.4→13.4 / 光栅化 247.6→32.0ms，传感器 layoutN 602→50 / 25.2→13.4 / 259.8→29.6ms，任务页滚动期间 DrawFrame 0.543→0.097ms/帧。
未完成：这台机上复现不出用户所说的掉帧（p95 恒 4.3ms、over16/32 全 0）⇒ 修的是「每帧白花的开销」而不是「已观测到的卡顿」，真正的验收得看用户在他自己的显示环境上的体感。
下一步：让用户刷新页面（桥按请求读盘、不用重启）后滚滚看；如果还卡：先确认是不是在远程/虚拟显示上，再让他打开设置页的「低特效模式」；仍不行就要继续减 `.glass` 元素数量或看 GPU 驱动/虚拟显示。

## 2026-10-04
完成：为了给“滚动卡”找证据，新增了三个可重用的量测脚本（都只读、放 `out/`）：`out/pwviz.cjs`（浏览器进程 CDP tracing + rAF 间隔 + Performance 指标增量 + CSS 变体对照 + `--damage/--dmgms` 把合成器喂满）、`out/pwshot.cjs`（类切换/样式验收 + 滚动中截图）、`out/pwlook.cjs`（有 blur / 去 blur 的外观对照）。关键教训：短页（任务页只多 152px）一次滚轮就到底、之后不再有 scroll 事件，140ms 后类就摘了 —— 验 `is-scrolling` 要用长页（调参 2535px）并在一次 `evaluate` 里同时读类和计算样式。
下一步：以后改前端视觉/动画，先用 pwviz 在 tasks+params+scope 上跑 base 与对应变体，比“每帧 drawFrame/每帧合成器总 ms”，不要比总帧数。

## 2026-10-04
完成：用户 m03190 那一轮全部做完并提交 5d5ebbc（10 files）：①电量口径改「≥12.2V 满电」——固件 app_ui.c 的 kBv 电压轴从公开 3S 的 [12.6,9.0] 压到 [12.2,9.0]（v_new = 9000+(v_old-9000)*3200/3600），上位机新增 src/lib/battery.ts + battery.test.ts（6 条，用正则解析 app_ui.c 的 kBv/kBp 逐值对拍）并让 Overview 显示「V · NN% · mA」。②真车验证：J-Link 读 RAM 电量静态量 = s_bat_mv 12094 → s_bat_pct **95**（旧表同一电压 80%），s_bat_ma_ok/s_bat_ok 都 1。③顺手挖出并修掉一个真 bug：T_TASK_STATUS 的变化判据含 elapsed_ms ⇒ 每拍一帧，自启任务跑着时链路被灌到 338 帧/s、桥 8s 丢 1904 帧；改成跳过 elapsed_ms 后 bench --attach 量到 101.2 帧/s、丢 5 帧、rtt 13.1→5.3ms、事件 task 8/telem_batch 305。④开机自启端到端真车验证通过：烧一个**不含 SET_SPEED/SET_MODE/TURN_DEG/ACTION/SET_SERVO** 的测试任务（out/autotask.c 生成，2 状态 WAIT_MS+GOTO，len=89 crc16=0xD604）到 id 6 并 COMMIT flags=1，J-Link dump 页 58 确认 flags=0x01 AUTO；烧固件复位后 RTT 打出 `task auto: slot=6 load=0 / start=0`，桥侧 status = id 6 running true elapsed 递增、state 0/1 在换算、遥测 task_id=6、pwm_l/pwm_r 恒 0（车没动）。⑤按用户要求把测试任务删干净并取证：跑着时 DELETE 被拒（status 7 忙）→ stop（status 0）→ delete（status 0）；J-Link dump 页 58 现在 count=1 只剩 `#0 id=0 len=122 crc=0xF137 flags=0x00 常态循迹`，槽 1..7 全 FF；再 `r`+`go` 复位并等 3s/6s，task_run 静态量块（0x20001DD0..0x20001DEF）全 0 ⇒ 什么都不自启；桥侧 /api/tasks/status = id 65535 state 255 running false。验证链：make -C firmware（text 115908/data 364/bss 33944、bin 116280 B、MD5 E8187C1CFF2CF1C48B7298498164C95B，烧录回读 firstDiff=none）、主机测试 13/13、tsc 0、vitest 20 文件 186 条、npm run build（dist/assets/index-BiYDclBi.js + index-D6G-7ug6.css，桥已在发新产物、都 200）。
未完成：用户之前列的历史项仍未做：参数写 Flash（PROJECT.md「未做」里那条已过时，需与用户确认）、921600 需换普通 USB-TTL 接 U12（4=GND/5=PC1/6=PC0）、m19448（旧内置长流程真板实跑）、m20490（任务编排模型 A/B/C/D 等用户选）。
下一步：用户眼睛确认两处：刷新页面看「电池 / 电流」是否显示 96% 一类的百分比（车上 12.1V）、以及任务页「车上现有」只剩 #0 常态循迹且没有自启徽标。

## 2026-10-04
完成：为「自启会不会自己所有都测试了」做的安全取证（结论写进 memory/DECISIONS.md）：自启路径 firmware/Core/Src/main_app.c:155-181 每次复位都跑（TaskStore_Init → 逐槽打 RTT → TaskStore_AutoSlot → TaskStore_Load → TaskRun_Start）；TaskRun_Start（task_run.c:268-296）只调 Motion_SetSpeed（motion.c:53-56 只写 s_base、不使能速度环），mode 留在 MOTION_IDLE（motion.c:163-175 不写 PWM）⇒ 只含 WAIT_MS/GOTO 的任务车上不可能动。
下一步：若以后还要在真车上放自启任务，沿用这套取证顺序：先 TaskProg_Validate 自检 + 只放等待类块，再看 RTT 的 task auto 行与 /api/state 的 pwm。

## 2026-10-04
完成：修掉用户报的「自启任务吓我一跳 + 改不了删不掉」。现场：车上任务 6（契约自检，flags bit0=自启）正在跑，固件对跑着时的 DELETE/COMMIT 一律回 ST_BUSY（host_link.c:967,999）→ 页面只显示「被车拒绝：忙」。清理：POST /api/tasks/stop（elapsed 停在 85622）→ POST /api/tasks/delete id=6 → status 0 OK；J-Link dump 页 58（out/_dirnow.jlink → out/dir_now.bin）只剩 id 0「常态循迹」、flags=0x00 ⇒ 车上已无自启任务。代码（提交 d6e2687）：Tasks.tsx 状态轮询改 1 Hz 常开 + 「正在跑」横幅不等监视 + 进页面自动静默读目录（refreshRemote(true)）+ 删/烧/换任务前先停车 + 自启徽标与警告；taskapi.ts 新增 delWithStop(id)（先删→被拒就停车→再删，回 {result, stopped}）。验证：npx tsc --noEmit 干净；vitest 19 文件 / 180 passed（taskapi 新增 3 例钉死请求顺序 delete,stop,delete）；npm run build 通过（dist/assets/index-DfKcBMOQ.js）；Playwright 只读探针 out/pwtasks.cjs 打开 http://127.0.0.1:8770/#/tasks（不点任何按钮）= 自动列出车上 #0 常态循迹、1 Hz /api/tasks/status 在跑、零 console 错误/零请求失败。
未完成：暂无
下一步：用户刷新任务页确认「车上现有」自动出现、开关不再被卡住。

## 2026-10-04
完成：③「灰度 1 路/8 路读数偏低」结案，两根因都钉死。根因 a：我上一轮的 run2 探针固件为测 PNB→块映射故意擦了块 31（页 62+63）⇒ 车上 nvram 记录（参数镜像 + 两套灰度标定）被抹成全 FF、之后没人恢复（`tools/flash.ps1` 只擦镜像范围 0..56）⇒ 开机 `nvram_load()` 找不到记录 ⇒ `trace_cal_valid=0` ⇒ `frame_compute_body()` 在 `if (!trace_cal_valid) return;`（`trace_cal.c:642-645`）直接返回、`trace_cal_level()` 落到 `level_of()` 的未标定分支（滚动 min/max：静态表面上窗口塌到 ~5 计数、黑白反相 ⇒ 读数低）。根因 b：桥 `protocol.py` 的 `decode_cal()` 把整套 `valid` 当位掩码（`valid & (1<<i)`）⇒ 标定有效时只第 1 路 ok、其余 reason=2、`quality` 报 1，标定页显示「第 1 路正常 / 其余异常 2 / 质量 1」。修：J-Link `loadbin out/nvram_page63.bin 0x0801F800` + `verifybin`（脚本 `out/_nvrestore.jlink`）；`protocol.py` 改成按 `bad[]` 判、`sim.py` 标定帧 valid 由 0xFF 改 1、新增 `test_decode_cal_per_channel_uses_bad_array`。验证（真车 COM26 @230400）：恢复后 J-Link RAM `trace_cal_valid=1`、`s_fl_state=1`(OK)、`s_wht`/`s_blk` 与备份 `cal[0]` 逐字节一致、遥测 `gray` 由 `[153,127,102,153,153,102,85,127]` 变成 `[235,250,221,244,234,242,230,227]`；用新代码重启桥后 `GET /api/cal` = `quality 8`、8 路全 `ok:true`、`blk/wht/thr/jit` 与固件 RAM（`s_bad` 8 字节全 0、`s_pol` 全 0）逐路一致；`pytest tools/console/tests -q` = 165 passed。提交 `4fce4e9`（桥侧）+ `out/_nvparse.py`（ctypes 复刻 nvram 记录布局，能打印参数与两套标定）。
未完成：无（③ 已收口）
下一步：刷新标定页目视 8 路全绿（真车桥已用新代码重启在 8770，PID 38224，输出重定向到 out/bridge8770.log）

## 2026-10-04
完成：② 任务烧录的移动端三项上真车验过：`POST /api/tasks/stop` 停掉开机自启中的任务 6（`elapsed_ms 161020` 正说明它是复位后自启的）、`POST /api/tasks/run {"id":0}` → 3s 后 `elapsed_ms 5017` 且遥测 `mode 3 / task_id 0 / pwm_l 6480 / pwm_r -6480`（`vbat_mv 347` ⇒ 电机轨没电，只出指令不动车）、再 stop = OK ⇒ RUN / STOP / 开机自启都跑过（COMMIT flags bit0 = 开机自启，见 `docs/任务编程.md:154`）。⑥ 数字：`python -m bridge.bench --attach 8770 --seconds 8`（workdir `tools/console`）= 实收 100.7 帧/s（链路喂 101.0、丢 3 帧）、报文间隔 p50 0.0 / p95 31.8 / max 32.3 ms、SSE 35.4 事件/s、72.7 KB/s、单条 2105B、攒批 260 条装 800 帧（3.0 帧/条）；桥进程 CPU（`Win32_Process.TotalProcessorTime` 采 10 s）= 7.3%（单核）；卡顿账本 `tools/console/web/src/lib/hotpath.test.tsx` 最贵页 IMU 中位 1.85 ms、每 tick 拷贝点 236~1023（旧实现 4000~12000）、总览 354 / 传感器 472 / 波形页扫描点 5。⑦ 全绿：pytest 165 passed、`npx vitest run` 19 文件 / 177 passed（要先 `node mock/server.mjs` + `CONSOLE_BASE=http://127.0.0.1:8788`，少了它是 skip 不报错）、`npm run build` 通过（dist 哈希不变）、`make -C firmware -j8` 通过、`pwsh -File firmware/test/run_tests.ps1` = 通过 13 / 失败 0、`tools/check_static.ps1` 0 error、`tools/check_warnings.ps1` 待处理 0。
未完成：①④⑤ 的界面观感（波形页、IMU 履带车外观与姿态方向）要用户目视
下一步：把 ①②③⑥⑦ 的数字与证据汇总报给用户

## 2026-10-04
完成：固件启动横幅的波特率改成从 `board_dl_baud()` 现取：`firmware/Core/Inc/board.h` 加声明并改注释（写明「别再手抄一份」）、`firmware/Core/Src/board.c:766` 加 `uint32_t board_dl_baud(void) { return DL_BAUD; }`、`firmware/Core/Src/main_app.c` 用 snprintf 组横幅（顺手 `#include <stdio.h>`）。验证：重烧后真车桥日志收到 `[fw] host link ready (LPUART1 230400 8N1)`；`bin 116240 B`（116K 上限余 2544）、MD5 `6CE640BD678ADBD101FDA98FB06E7F87`、`tools/flash.ps1` 回读 116240 字节 `firstDiff=none`、固件主机测试 13/13、check_static 0 error、check_warnings 待处理 0；烧后 nvram 未被碰（`/api/cal` 仍读到标定值，`gray` 依旧 236~250）。提交 `7b9e90a`。
下一步：无

## 2026-10-04
完成：修掉真机「一提交任务 App 就 HardFault」的两个根因：①`firmware/App/Svc/task_store.c` 直接写 `FLASH->CR` 却从没 `HAL_FLASH_Unlock()`（上电后 CR.LOCK=1，硬件静默忽略写、STRT 没被接受，擦除"成功"却页没变，只剩后面那道页首 0xFFFFFFFF 检查翻车）；②擦除目标公式 `page - 32` 在本板（4KB 块粒度）会擦到固件自己的页 54/55/56。现在 `ts_block_erase(block)` 用块号（页号/2）、等待改成 BSY 落 + 目标块首字真为 0xFFFFFFFF、`store_flash_write()` 自己解锁；布局改页 58 目录 + 页 59..61、`.ld` 116K；桩 `firmware/test/stub/host_hal.c` 换成同一套块模型（含 CR.LOCK 语义）。验证：`pwsh -File firmware/test/run_tests.ps1` 通过 13 / 失败 0（t_taskstore 556 项，新增 ⑭ 上锁语义、⑮「别擦掉固件自己」；把块号公式临时改回 page-32 时第 ⑮ 节立刻失败）；`make -C firmware -j8` text 115840 / data 364 / bss 33944、bin 116212 B（MD5 A0D5080FAA9EAB2AFDA1ACF0B9694ECA）、`tools/flash.ps1` 回读 firstDiff=none；真车（用户桥上 8770 → COM26 @230400）首次提交 / 覆盖写 / 删除 / 再提交四次全 `status 0 OK`，每次 `/api/tasks/get` 回读逐字节一致，J-Link 逐页 dump 证明页 54/55/56 与页 62/63 全程一个字节没动。提交 `60d1f3a`
未完成：任务在真车上的 RUN/STOP/开机自启还没跑过（本轮只验了存储读写删）
下一步：上车跑 RUN/STOP/开机自启（会动车：先把轮子架空或放赛道上），再按 docs/任务编程.md §8.2 全流程对拍一次

## 2026-10-04
完成：IMU 页 3D 车模型改成用户要的两轮履带车外观（`tools/console/web/src/components/Car3D.tsx` + `index.css`），姿态口径改成跟固件 OLED 方框一致：固件默认轴档 M3 把遥测 pitch 当横滚源（取反）、roll 当俯仰源 ⇒ `carTransform = rotateY(yaw) rotateX(-roll) rotateZ(+pitch)`（轴对调 + 符号，不是只翻符号；推导与 app_ui.c 行号写在 `web/src/lib/attitude3d.ts` 文件头，单测跟着改）；顺带修波形页 uPlot 量程：`u.setData(data, false)` 改成 `u.setData(data)`（resetScales 必须为 true，否则建图时空量程 [null,null] 永远画不出坐标轴和曲线），以及泳道指针离开画布时清掉游标读数（`onHover` 允许 null）。验证：`npx tsc --noEmit` 干净 + `npm run build` 成功（dist 重建 `index-CQ74MyRI.js` / `index-CBDvorbl.css`）；`npx vitest run` = 19 文件 / 177 项 passed（**注意 8770 上现在跑的是用户那台真车桥，`contract.test.ts`/`ssrPreview.test.tsx` 打 8770 会假红**：要起 `PORT=8788 node mock/server.mjs` 并设 `CONSOLE_BASE=http://127.0.0.1:8788` 再跑才是有效绿）

## 2026-10-04
完成：给上位机桥加了一键启动并提交 dc4bcf1（8 files, +213）：tools/console/launch.ps1（唯一逻辑：自己找 Python py -3/python 并验 pyserial、预检 HTTP 端口占用、真车模式先试开一次串口给人话警告、前台起桥由桥开浏览器、-Pause 让双击窗口不闪退）、三个纯 ASCII 的 .bat（tools/console/启动上位机.bat 真车自动找串口 @230400 / 启动上位机-仿真.bat / 启动上位机-列串口.bat）、仓库根同名转发 bat（可带 -Sim/-ListPorts/-HttpPort）。验证：launch.ps1 -ListPorts 与根 bat 都打出 COM26(←车上的)/COM12/COM4；经根 bat 起仿真桥 -Sim -NoBrowser -HttpPort 8799 后 /api/health = sim:true link_open:true frames_seen:321 web_built:true；同端口再启动被拦、报占用者 PID、退出码 3。顺手订正 docs/API.md 的 --port COM26 笔误（--port 是 HTTP 端口，串口是 --serial），跑法写进 PROJECT.md「东西都在哪」。
未完成：没有实测「真车自动找串口后成功打开 COM26」这一条 —— 当时用户自己的桥占着 COM26（PermissionError）；只验证到「报出打不开 + 桥照起」。
下一步：等用户关掉 8770 上的旧桥后，双击 tools/console/启动上位机.bat 走一遍真车路径（应无黄色警告、/api/link connected:true）。

## 2026-10-04
完成：踩坑并按规则改掉：给 launch.ps1 补 UTF-8 BOM 时用了 Get-Content -Raw -Encoding UTF8 + [IO.File]::WriteAllText，结果文件被写成 0 字节（size=0），而 PowerShell 语法检查对空文件照样报 OK，于是「静默退出、exit=0、无任何输出」查了好几轮才定位。补 BOM 的可靠写法：先 [IO.File]::ReadAllBytes 断言非空，再 Encoding.UTF8.GetString + WriteAllText(UTF8Encoding($true))。
下一步：凡是脚本补 BOM/重写后，先看 size + 行数，再跑一次真实调用（-ListPorts 之类）确认有输出，别只信 Parser::ParseFile。

## 2026-10-04
完成：用户 m22973 三条全部落地并提交 `c7de56e`（44 files, +1303/−1305）：①删录制/回放与波形 CSV（桥 `ReplayLink`/`--replay`/`--record`/`/api/record(s|ings)`、网页 `csv.ts`/`replay.test.ts`/`csv.test.ts`、`tools/console/tests/test_replay_link.py`，mock 同步，保留 `--sim`）②网页↔桥↔固件核对：命令名统一 `state.py:CMD_NAMES`、删死表 `CMD_BY_NAME`、清空日志真清桥侧（`POST /api/log/clear`）、波特率默认全线 230400、任务提示分三种 ③卡顿+IMU 抖动：`SeriesRing.view()`/`nextRange()`/窄订阅（拷贝点 4000~12000 → 236~1023）。验证：`pytest -q`（workdir tools\console）= 164 passed；`npx vitest run`（workdir tools\console\web）= 19 文件/177 项 passed；`npm run build`（tsc --noEmit + vite build）干净并重建 `dist`；起 `node mock/server.mjs` 后 contract+ssrPreview = 2 文件/8 项 passed（不起 mock 时它们是 skip 不报错）；真桥 sim 端到端：`GET /api/link` baud 230400、`POST /api/cmd {"cmd":"set_telem_hz","arg":200}` = OK(cmd 8)、`{"cmd":"telem_hz"}` = 用户报的那条 `没有这个命令：telem_hz`；`_task_reply` 三分支文案逐一打印核对。提交信息走 `out\commit-msg-console.txt` + `git commit -F`（pwsh 无 heredoc）。
未完成：记忆文件已就地订正（`memory/PROJECT.md` 里 `--replay`/`pytest 142|167`/`vitest 174`/「桥默认 921600」/「回放之后 link.port」五处）；**历史目标仍未完成**：跑 921600（需普通 USB-TTL 接 U12）、m19448 旧内置长流程真板实跑、m20490 任务编排模型 A/B/C/D 等用户选。
下一步：把 c7de56e 的数字与证据（pytest 164 / vitest 177 / 拷贝点对照 / 两条 toast 的根因与现状）报给用户；若用户要更彻底的卡顿优化，再拆 `Overview/Drive/Sensors/Servo/Control` 这 5 个仍整页 `useStore()` 的页面。

## 2026-10-04
完成：真机链路打通并实测（档位 230400，用户 m20705 的"烧录后实测"这一环完成）：`DL_BAUD` 改 230400 重烧（回读 116108 B firstDiff=none），probe @230400 = 204 帧/2s（≈101 帧/s、0 CRC、0 重同步，修节拍前 159 帧/2s），bench --serial = 实收 100.0 帧/s、SSE 72.0 KB/s、桥 CPU 14.8%、rx=792 crc=0 resync=0 drops=0，速率扫描 50/100/200 分别 51.3/101.7/201.4 帧/s（200Hz = 230400 的 62%）、rtt_ms 11.9(100Hz)/7.3(200Hz)，常驻桥在 8793 直接服务网页 dist。
未完成：文档/记忆已改完，代码改动还没提交；921600 仍需换普通 USB-TTL 接 U12。
下一步：提交 firmware（board.c 的 DL_BAUD + host_link.c 的 arg 宽容与节拍累加）+ bridge（protocol/state/sim）+ 4 个测试文件 + docs（上位机通信协议.md/固件接口规格.md/tools/console/docs/API.md）+ memory，然后把上面这组数字报给用户。

## 2026-10-04
完成：修掉真车上挖出的两个真 bug 并各自钉了测试：①桥发 T_CMD 只带 1 字节 arg（固件 len>=5 才读 arg ⇒ telem_hz 被钳成 1Hz）⇒ 新 cmd_payload() 发 cmd+pack_i32(arg)，固件额外容 len>=2 的老式帧；②固件遥测节拍用 now 重算被主循环 3.3ms 量化（100Hz 只出 75.5 帧/s）⇒ 改累加 + 落后超 2 拍重对齐。测试：test_protocol 的 pack_i32/decode_cmd_arg、test_sim 的 5 字节载荷、test_firmware_pipe 的真固件源码对拍（5 字节→telem_hz 200）、test_api 的 set_telem_hz 真到车 + 300 被钳到 200；`pytest -q`（workdir tools/console）= 167 passed。
下一步：以后写任何下行帧都从 protocol.py 的构造函数走，别再手拼载荷。

## 2026-10-03
完成：给上位机工具加了 J-Link VCOM 上限保护（提交 4989891）：新增 `tools/console/bridge/serial_safety.py`（认 pyserial/PnP 两种 hwid 与 "JLink CDC" description；`cap_bauds()` 过滤梯子、`check_baud()` 给 `--serial`；认不出的口不拦；`--force-baud` 越过）与 `tools/console/bridge/console_io.py`（重定向→UTF-8，终端→errors=replace），接进 `probe.py`/`bench.py`/`main.py`。验证：`pytest -q`（workdir tools/console）= **162 passed**（原 142 + test_serial_safety 13 + test_console_io 5 + test_probe 接线 2，另把 probe 三个汇总用例与本机硬件解耦）；带保护的 `python -m bridge.probe --port COM26 --seconds 2` 真的跳过 921600/460800 并打出警告、结论行不再崩。
未完成：真板实测仍没做：硬件侧只有探针的 CDC VCOM 能到 230400，而板上固件是 921600
下一步：等用户在 A（改 `firmware/Core/Src/board.c:556` 的 `DL_BAUD` 为 230400 重烧、桥 `--baud 230400`）与 B（USB-TTL 接 U12 脚6/5/4 保 921600）里选一个；选 A 就走 `make -C firmware -j8` → `tools/flash.ps1` → probe/bridge/bench 在 230400 上实测帧率/CRC/延迟。

## 2026-10-03
完成：复核硬件在场性并纠正判据：`Get-PnpDevice -PresentOnly -Class Ports` 里 `JLink CDC UART Port (COM26)` 现在是 **OK**（复合设备 `USB\VID_1366&PID_0105\000602711039` + MI_00 + MI_02 都在），`GetPortNames()` = COM4/COM12/COM26；不带 `-PresentOnly` 的那份列表里几十条 COM 口状态全是 `Unknown`（历史幽灵设备），不能当在场判据。用带保护的 probe 静听 COM26：@230400 收 2790 B、@115200 收 1399 B（正好 2:1，只跟听者波特率成正比）、两档均 0 合法帧 + 16 次重同步 ⇒ 线上有成串数据（约 12% 占空比，形状像 100Hz×64B 遥测），错的只是波特率。
未完成：探针是被我自己那次 `--bauds 115200,230400,460800,921600` 的扫档打进 bootloader 态的（已经物理拔插恢复）——已向用户说明
下一步：同上（等 A/B）。

## 2026-10-03
完成：补上目标里「允许提高遥测量率」在界面上的残口：遥测频率滑块 `max` 从 100 改成 200（固件 `CMD_SET_TELEM_HZ` 本来就钳 1..200），从 `/api/link` 同步来的值同样钳位；顺带改正 `firmware/App/Svc/host_link.c:1045-1047` 注释里写错的网页位置（「设置 → 遥测频率」→「控制 → 遥测频率」）。验证：vitest 174 passed / 19 文件、`npm run build` 干净、`make -j8`（workdir firmware）通过 text 115712/data 364/bss 33944/bin 116084B；**纯注释改动重建后 bin MD5 与板上已烧的 EDB8343723DA36C53AF030CA98B02300 完全一致 ⇒ 不需要重烧**（记住这条：改注释后重建 bin 逐字节不变，别为它跑 flash.ps1）。提交 edab555。
未完成：仍需用户动手：插上数据口（或选 A/B 降/换波特率）。
下一步：用户接上口后 `python -m bridge.bench --serial <口> --baud <波特率>`（workdir tools/console）出帧率/间隔/丢帧报告。

## 2026-10-03
完成：补齐 m20705 目标里剩下的两条：①「桥支持 SSE 合并」= `bridge/server.py` 的 `group_sse()` + `--sse-batch-ms`（默认 20ms，0 关，连续遥测攒成一条 `telem_batch`，单帧仍发 `telem`）+ 前端 `api.ts` 逐帧喂 `onTelem`；②「网页按 rAF 合并渲染」= `store.ts` 新增 `renderHot()`，热通知挂 `requestAnimationFrame`。`bench.py` 增 `expand()`/`batched`/`per_event`，攒批时间隔标成报文级。验证：`pytest -q`（workdir tools/console）= 142 passed；`npm test`（workdir tools/console/web）= 174 passed / 19 文件；`npm run build` 干净（dist 已重建，dist 进版本库）。实测 `python -m bridge.bench --replay --hz 100,200 --seconds 4`：帧率 100.4→100.2 / 199.3→199.8 帧/s 不变，报文 402→135 / 803→134，字节率 73.2→72.1 KB/s。提交 a8fb97f、3ac61cc。
未完成：目标里「烧录后实测帧率/crc/延迟」的真板那一半仍做不了：硬件不在场（`GetPortNames()` 只有蓝牙 COM4/COM12，USB 上没有任何串口芯片，`USB\VID_1366*` 不在总线上），且用户还没在 A（`DL_BAUD` 降 230400 重烧）/ B（USB-TTL 接 U12 脚4/5/6 保 921600）里选。
下一步：用户把数据口插上后（CH340 那条已改名为 COM11 的口，或 USB-TTL 接到 U12）跑 `python -m bridge.bench --serial COM11 --baud 230400`（workdir tools/console）出帧率/间隔/丢帧报告，CRC/重同步/波特率另看 `python -m bridge.probe --port COM11`。

## 2026-10-03
完成：新增承载量测工具 `tools/console/bridge/bench.py` + `tools/console/tests/test_bench.py`（14 项），并修好两处量测口径 bug（rx 基线与计时同窗口、CPU 报窗口均值）。验证：`C:\Python314\python.exe -m pytest -q`（workdir `tools\console`）= **132 passed / 37s**；`python -m bridge.bench --replay --hz 50,100,200` 三档实跑均与喂帧率一致（50.1/100.0–100.6/199.8 帧/s，丢 0–1 帧），100Hz 单条 740B、73KB/s、桥 CPU 1.6%（两遍复现）；`--sim` 量出"发指令→遥测看见"= 16–33ms（p50 30.6ms，n=6）；错误路径 `--attach 9` / `--serial COM99` 都给 rc 2 + 人话诊断。
未完成：m20705 的"真车实测帧率/crc/延迟"仍需硬件：帧率/承载这一半现在无硬件也能用 `--replay` 出数，但 CRC/波特率那半必须等探针或 USB-TTL。
下一步：提交 bench.py/test_bench.py + memory；文档已在 `memory/PROJECT.md`（东西都在哪）写了用法与口径；探针仍不在 USB 总线上（等用户物理拔插）、等用户选 A（固件 DL_BAUD 降 230400）或 B（USB-TTL 接 U12 保 921600）。

## 2026-10-03
完成：查清"921600 新固件已烧进去却一帧都收不到"的真因：不是固件/接线，是车上这颗 J-Link 的 VCOM 上限只有 230400 —— 对它配置 ≥460800 会让探针崩溃并枚举成 PID_0101 引导态（须物理拔插恢复），所以链路从未真正通。验证：`tools/console/bridge/probe.py` 低→高扫档（115200 ✓ 有字节、230400 ✓ 有字节、460800 ✗ OSError 121、921600 ✗ PermissionError 31）；崩后 JLink.exe 输出 "Replacing firmware ... Oct 12 2012 BTL" + "Cannot connect to J-Link"。另修好一处工具自身缺陷：probe 的 main() 以前在所有档都打不开端口时会误报"有字节但解不出帧"，现改为 return 2 并直说"所有档都打不开端口"（已提交 138b7db，pytest 118 passed）。
未完成：探针当前不在 USB 总线（软件重枚举把它彻底搞掉了），COM26 不在；m20705 的"真车实测帧率/crc/延迟"仍被这一环卡住。
下一步：等用户在两条路里选：A) 我把 firmware/Core/Src/board.c:556 的 #define DL_BAUD 改成 230400 重烧（tools/flash.ps1）+ 桥 --baud 230400，拿到原始 115200 的 2×；B) 用户接普通 USB-TTL 到 U12 脚4(GND)/脚5(PC1)/脚6(PC0)，固件保持 921600，桥 --baud 921600。两条都先要物理拔插探针（它现在不在 USB 上）。

## 2026-10-03 · 固件-验证
完成：新增链路体检工具 `tools/console/bridge/probe.py`（`python -m bridge.probe --port COM26 [--bauds 921600,460800,230400,115200] [--seconds 2] [--hello] [--list]`，workdir `tools/console`）：不启动桥、不写任何字节（默认只收不发），按波特率梯子各静听几秒，报每档「字节数/合法帧/CRC 错/重同步」并翻译成人话。判定语义：0 字节 ⇒ 板子没发/线没通/探针这档不通；有字节但一帧都解不出 ⇒ 波特率错配（典型乱码）；有帧且零 CRC 错 ⇒ 这档可用（`Result.usable`）。返回 `Result`（baud/rx_bytes/frames/crc_errors/resyncs/hello），`main()` 汇总「最高可用波特率」或「所有档 0 字节 ⇒ 不是波特率的事」。为什么需要它：921600 下桥报 rx:0 时，能立刻区分「波特率不匹配（乱码）」与「线/探针/固件（干净 0 字节）」，并一次扫完回退梯子。验证：`C:\Python314\python.exe -m pytest -q`（workdir `tools\console`）= **113 passed**（新增 `tools/console/tests/test_probe.py` 10 项，用合成字节钉住 decode_bytes/verdict：合法帧、纯乱码、坏 CRC、半截帧、乱码+合法帧混合、histogram、usable）；`python -m bridge.probe --list` 实跑 exit 0，列出本机仅剩的两个蓝牙串口 COM12/COM4 并提示没给 --port。
未完成：真车实测仍卡在物理拔插 J-Link（探针 USB 栈卡死后 `VID_1366&PID_0105` 复合设备 + COM26 全变 CM_PROB_PHANTOM，本机只剩蓝牙串口，没有第二条到车的串口路径）
下一步：用户物理拔插探针 → 确认 COM26 回来 → 先跑 `python -m bridge.probe --port COM26`（看 921600 是乱码还是 0 字节）→ 再起桥看 /api/link 的 rx

## 2026-10-03
完成：2026-10-03 深夜：921600 提速改造已全部落盘并提交（`406b67b` 代码+文档+dist，`1776be5` 追加前端回归测试）。**烧录成功**：`tools/flash.ps1` 回读 116084 字节 `firstDiff=none`，板上已是 921600 新固件。**但真机链路还没通**：停掉旧桥后用新代码（默认 921600）起在 8793，`GET /api/link` = `connected:true / baud:921600 / rx:0 / fw:null` —— 零帧。同时 J-Link 探针 USB 栈卡死（`JLink.exe` 日志停在 `JLINK_OpenEx(...)`，`VID_1366&PID_0105` 复合设备连 `JLink CDC UART Port (COM26)` 一起变 `CM_PROB_PHANTOM`，`Disable/Enable-PnpDevice` 软件复位无效，`GetPortNames()` 只剩蓝牙 COM4/COM12）⇒ 0 帧可能只是探针半死，**921600 可用性尚无结论**。前端 `vitest` 现 169 项（新增 `tools/console/web/src/lib/perf.test.ts` 钉住 meta 快照/50ms 合并/单通道快照三条），pytest 103，固件主机测试 13/13。
未完成：上位机↔下位机链路 921600 的真车实测（帧率/crc/延迟）没做；旧内置长流程转成任务后的真板实跑（m19448）没做；讲任务编排模型后用户还没在 A/B/C/D 里挑（m20490）
下一步：等用户物理拔插 J-Link USB（必要时连车板电源一起断）→ 确认 `VID_1366&PID_0105` 与 COM26 回来 → 用新代码起桥 `python -m bridge.main --serial COM26 --no-browser --port 8793`（workdir `tools/console`）看 rx；仍为 0 就 460800 试一次（收到乱码也说明板子在发）→ 跑 RTT 看 `HOST: rx/tx/crc/txdrop/rxover`；确认 VCOM 吃不下 921600 就改 `firmware/Core/Src/board.c:553` 的 `DL_BAUD` 走 460800/230400/115200 梯度重烧

## 2026-10-03
完成：上位机+链路提速（用户 m20705）代码全部落盘并验证：固件 LPUART1 921600 + 收 DMA1_Ch2 循环 2048B + 发 DMA1_Ch3 槽位队列 8×144B（TASK_TX_PER_POLL 1→8、DUMP_PER_POLL 4→8、telem_hz 默认 100）；网页加 useMeta() 元快照 + ring.snapshotField() 单通道快照 + 热节流 50ms，Params/Settings/Control 日志卡改订阅元快照；桥默认波特率 921600 并删掉没人订阅的 frame 事件。验证：make -j8 通过（text 115712/data 364/bss 33944，bin 116084B）、firmware/test/run_tests.ps1 13/13、pytest 103 passed、vitest 166 passed、tsc --noEmit 干净、dist 已重建。
未完成：①未烧录：板上仍是旧的 115200 固件，跑了 8793 的桥（PID 11904，21:43 启动）也是旧代码 ⇒ 两者必须一起换；②COM26 是 JLink CDC UART Port(VCOM)，能不能跑 921600 未验证，不行就按 460800/230400/115200 梯度改 DL_BAUD 重烧
下一步：烧 firmware/build-app/haixin-app.bin（tools/flash.ps1）→ 立刻用新代码重启 8793 真车桥（python -m bridge.main --serial COM26 --no-browser --port 8793，默认 921600）→ 跑 RTT/网页确认 tx 约 100Hz、crc=0、txdrop=0、操控延迟下降

## 2026-10-03
完成：界面去英文缩写（9 个文件：ui.tsx 的 Badge/Stat 加 title、Shell.tsx 状态栏改成「收到·丢弃·校验错 / 实时推送 / 模式」、Settings/Overview/Control/Tasks/Drive/Sensors/Params 的中文标签 + 悬停解释 + Settings 新增缩写对照卡）；查清并修掉「操控不了车」的根因（桥 server.py 新增 _serial_port_ok 提前拒非法串口名、Settings.tsx 端口只在串行模式同步 + 回落列表第一个口，回归用例加进 tools/console/tests/test_api.py）。验证：npx tsc --noEmit exit 0；npx vitest run 17 文件 166 项通过；pytest tools\console\tests 103 passed；npm run build → dist/assets/index-DBynlDh_.js 341.54 kB + index-C0kvFr6m.css 37.34 kB
未完成：带 server.py 新校验的桥尚未重启；真车链路当前是好的（COM26，rx 在涨）
下一步：提交这批改动（含 dist 产物）；桥的改动要重启 8793 那个桥进程才会生效（改的是 bridge/server.py，进程里还跑着旧代码）

## 2026-10-03
完成：操控页（#/drive）三条报障全改完并回归：①速度滑块从「mm/s ±3000」改成固件速度刻度 0..7200（lib/heading.ts 新增 PWM_MAX=7200 / DEAD_SCALE=1000），设置里新增 driveSkipDead（默认 true）让滑块从 1000 起步；②倒车打轮翻号（heading.ts 的 stepDrive 里 yawRate = base<0 ? -turnRate : turnRate）⇒ S+D 车尾往右摆；③mock/server.mjs 的 /api/drive 夹限幅从 ±3000 放到 ±7200、turn 单独 ±540。验证：npx tsc --noEmit exit 0；npx vitest run 17 文件 / 166 项通过（新增 keys「倒车组合键 S+D/S+A」与 heading「倒车翻号」两个用例）；npm run build 出新 dist（assets/index-Bi-eADAk.js 339.39 kB、index-CfDRAvCv.css 37.16 kB，旧 asset 已删）；pytest tools\console\tests 103 passed；make -j8 text 114292 / data 364 / bss 31040（本轮固件只动了 host_ext.h 的注释）。文档同步：tools/console/docs/API.md（/api/drive 量纲 + 操控页两条约定）、tools/console/web/README.md、firmware/App/Svc/host_ext.h 的模式注释。
未完成：真车上按 S+D 的实际方向还没实测（不敢自己给真车发驾驶指令，轮子会转）；真车桥在 8793 跑的是新 dist，浏览器刷新即生效
下一步：请用户架起车（或手扶）按 S+D 试一下车尾摆向；本轮的代码/文档/dist 与之前那批串口脚号更正一起提交

## 2026-10-03
完成：**真车串口链路打通**（用户在 U12 上把两根线反过来之后）：`C:\Python314\python.exe out\_linkprobe.py COM26 115200 3` → 静听 0.5 s 收 1684 B 遥测（`A5 5A 03 00 40 …` = type 0x03 TELEM2 len 0x40）、发 3 帧 HELLO_REQ 后收 3677 B / 53 个同步帧，含车回的 `type=0x40 (hello)` ⇒ 双向通。随后起真车桥（后台作业 pwsh-2869，workdir `tools/console`）：`python -m bridge.main --serial COM26 --no-browser --port 8793`；`GET /api/health` = `sim:false, mode:serial, link_open:true, frames_seen:312`；`GET /api/state` = COM26@115200 connected、`rx 313 / tx 2 / crc 0 / resync 0`、`telem_hz 50`、`rtt_ms 23.3`、握手 `quandi-car v0.2.0 proto 2 caps 0xFF`、实时遥测 `vbat_mv 11213 / ibat_ma 117 / yaw 158.9 / pitch 6.6 / roll 8.6 / gray=[0,0,0,132,250,226,91,0] / task_id 255 / mode 0 / gyro_ok true / fault false`，参数表 48 项回读（`param_crc 577`），`tasks: []`（真车目录为空）。桥同时把 `tools/console/web/dist` 挂在 8793，用户可直接开 http://127.0.0.1:8793 用真车。
未完成：真板实跑旧长流程任务（m19448）；sim 桥（pwsh-2807）已被杀掉换成真车桥，要回仿真得停 pwsh-2869 再起 `--sim`。
下一步：让用户开 http://127.0.0.1:8793 确认页面在真车上读数正常（电压/灰度/陀螺仪），然后决定是否把 `tools/console/web/src/lib/tasks/renwu-lap.json`（旧全程·12障碍，2127 B）传到真车跑真板验证（m19448 剩下的那步）。

## 2026-10-03
完成：用户**物理拔插 J-Link USB** 后探针回来（重新枚举为 `USB\VID_1366&PID_0105\000602711039`，带 CDC 口 COM26），`tools/flash.ps1` **一次烧成功**：CubeProgrammer 步 `Download verified successfully`，J-Link 步 `Bank 0 @ 0x08000000: Skipped. Contents already match`，回读 114664 字节 `firstDiff=none`，脚本结尾「烧录完成：回读一致（DEMCR 已清、程序已 go）」exit 0。随后 RTT 抓 12 s（`--elf firmware\build-app\haixin-app.elf`，日志 `out\_rtt_app_r5.log`）看到 telemetry 周期性输出、`fault=0 / over=0 / ISR max 12us`、`oled tx_done=16 err=0` ⇒ 新固件（含删掉任务A/B 残留页的 `7c628479`）在真板上跑起来了。
下一步：请用户按一下板上首页「任务」确认显示「待机 / 车上 N 个 / 左键返回」（目录未上传过应为 0 个）；若要跑旧全程长任务，用上位机 0x67 RUN 或先 COMMIT flags=1 开机自启。

## 2026-10-03
完成：确认这版**没烧进去**：`tools/flash.ps1` 两次都卡死（本次 bin `firmware/build-app/haixin-app.bin` 114664 B）。诊断链：①CubeProgrammer 步返回 1 且随后 J-Link 步无输出卡住（600s 后进后台 pwsh-2836）；②查到孤儿 JLink.exe PID 38108（2026-10-03 09:07:42 启动，`-CommanderScript ...\_jl_banner.jlink`，CPU 41293s / 墙钟 41448s ≈ 满 CPU 11.5 小时）占着探针，杀掉；③重跑（pwsh-2840）后 `STM32_Programmer_CLI`（PID 38880，`-c port=JLINK -d …haixin-app.bin 0x08000000 -v`）311s 只耗 0.77s CPU 再次阻塞，杀掉并手动清掉残留 `JLink.exe`；④最小探针脚本（`device STM32G474RE / si SWD / speed 4000 / connect / mem32 0x08000000,4 / qc`，加 `-Log`）日志停在 `JLINK_OpenEx(...)` 不返回 ⇒ 卡在打开 USB 设备；⑤`pnputil /restart-device` 重启 J-Link 设备 + 其父 Realtek 集线器后，JLink 37 秒干净返回 `Cannot connect to J-Link.`，但 `pnputil /enum-devices /connected` 里**再无任何 VID_1366 设备**。
未完成：烧录未完成；`firmware`/`web` 的其它验证都已在提交 7c628479 前后跑绿，不依赖探针。
下一步：等用户**物理拔插 J-Link USB**（必要时连板子/集线器电源一起断）后，重跑 `& .\tools\flash.ps1`（bin 114664）并确认 savebin 回读一致；在此之前车上仍是旧固件（屏上还有 任务A/任务B）。

## 2026-10-03 · KK_UI 界面
完成：修掉用户 m19865 报的「固件上还有 A B 两个任务」：真因是固件菜单残留（k_task_items 的「任务A/任务B」两页，中键只翻 g_live.task_state 这个没人再读的标志位）。firmware/App/Inc/app_ui.h 删 PAGE_CUSTOM_TASK_A/B + PAGE_MENU_TASK、新增 PAGE_CUSTOM_TASK 5、PAGE_COUNT 17→15、CUSTOM_COUNT 9→8、删 app_live_t.task_state；firmware/App/Src/app_ui.c 删 k_task_items 与 k_menu_pages 的「任务」行、k_routes 顺移（PID 1/速度环 2/角度环 3/循迹环 4）、首页「任务」= PAGE_CUSTOM_TASK、draw_task_page(void) 画 TaskRun_IsRunning()/TaskRun_Id()/TaskRun_Block()（空闲数 TaskStore_Present() 报「车上 N 个」）、中键只调 TaskRun_Stop()。验证：make -B -j8 text 114292 / data 364 / bss 31040、bin 114664（118K 余 6168）、run_tests.ps1 通过 13 失败 0、check_static.ps1 52 文件 4 告警 0 error、check_warnings.ps1 待处理 0 条、check_charset 对 60 个候选字只缺「状 U+72B6 / 启 U+542F」（所以文案避开它们，没重生成字模）。
未完成：屏上观感只有用户能看到（没接板、没烧这版）
下一步：用户烧这版（& .\tools\flash.ps1）后看首页「任务」页；若还要在屏上启动任务，再单独加一次确认

## 2026-10-03
完成：把 `53b5fc0` 里的 `renwu.c` 21 个可达段转成新任务格式 `tools/console/web/src/lib/tasks/renwu-lap.json`（15 状态 / 190 块 / 名字 95 B / 镜像 2127 B，名字「旧全程·12障碍」），单任务上限从 2048 提到 4096，「两轮直给」放回模式下拉；提交 `1af6824`。验证：`tools/console/web/src/lib/tasks/renwu-lap.test.ts`（7 项，含 2127 B 尺寸护栏与关键动作抽查）、`tools/console/tests/test_firmware_pipe.py::test_lap_program_is_accepted_by_the_real_firmware`（真固件 18 片 PUT → COMMIT → LIST → GET 逐字节回读）、真 HTTP 端到端 `out/_lap_http_e2e.py --leave`（8793 桥 + 假车，PUT/COMMIT/LIST/GET/RUN/STATUS/STOP 全绿，跑完故意不删，仿真车槽 0 上留着这条任务 flags=1）；三层回归 pytest 103 passed / vitest 17 文件 164 项 / run_tests 13 通过 / tsc 0 / `npm run build` / bin 114604 B / check_static 52 文件 4 告警 0 error。
未完成：蜂鸣器/RGB 没有对应块（转换时丢弃）；角度环「左右基准速度分开」不可表达；角度环倒车不可表达（退化成两轮直给）；旧 `Load()` 是开环 PWM，新「两轮直给」走速度环闭环；旧默认循迹是两级 `motor_tracing_pid_speed`，新下拉只有三级串级。
下一步：用户上车（真板 + 真 Flash 页 59..62）跑这条 12 障碍流程，看中途丢线判据与各段动作是否与旧车一致；若体积还要涨，先看能不能合并状态而不是再抬上限。

## 2026-10-03
完成：按用户选择去掉「跳到状态」目标里的「继续往下跑」：tools/console/web/src/lib/taskedit.ts 新增 FieldSpec.allowNoState（只给 WAIT_COND.p3）+ 纯函数 stateChoices(cur,stateCount,allowContinue)（真实状态 + 可选「继续往下跑」+ 当前值不在表里就临时补「（旧）继续往下跑」/「状态 N（不存在）」），GOTO 的 doc 补一句目标只能是真实状态；tools/console/web/src/pages/Tasks.tsx 的 kind==='state' 渲染改用 stateChoices；removeState 加了 fixRef() 只修 0..N-1 的引用。验证：npx tsc --noEmit exit 0；vitest 16 文件/157 项通过（taskedit.test.ts 新增一条用例：field 标志、stateChoices 三种取值、0xFFFF/越界兜底项、removeState 不动 0xFFFF）；npm run build 后 dist/assets/index-CKcSQ2WB.js 337.70 kB，8793 首页已在托管它。提交 753a3a0。
未完成：下拉里的选项列表只有 stateChoices 的单元测试盯（项目没有 jsdom/puppeteer，SSR 不渲染 select 且无法截图），浏览器里的观感只有用户能看到
下一步：等用户刷新 #/tasks 看新下拉；之后剩下的大项还是真车上车对拍（真板 + 真 Flash 页 59..62）

## 2026-10-03
完成：修掉用户报的「拖一个模块但没放下，蓝条就不消失」：根因是 tools/console/web/src/pages/Tasks.tsx 的 dropAt 只在 DropLine 的 onDragOver 置位、onDrop 清位，块丢到空白处/参数区、按 Esc 或拖出窗口时浏览器只发 dragend ⇒ 蓝条挂住。改法是在 TasksPage 加一个 window 级 dragend/drop 兜底 clear 的 useEffect（带 removeEventListener 清理）。验证：npx tsc --noEmit exit 0、vitest 16 文件/155 passed、npm run build 后 dist/assets/index-CKMdvvq8.js 336.28 kB，8793 首页已托管该文件。提交 07a10c7。
未完成：没有自动化 UI 拖放测试；截不了图（Chrome/Edge 的 --headless=new --screenshot 在本机都不落盘，项目也没有 puppeteer/playwright），视觉只有用户自己看
下一步：用户刷新 http://127.0.0.1:8793/#/tasks 确认观感

## 2026-10-03
完成：按用户选择改完 #/tasks 两处观感：①tools/console/web/src/lib/taskedit.ts 的 MODE_OPTIONS 由 5 项砍到 3 项（停车/角度环/循迹），新增 MODE_LEGACY + modeLabel()/modeChoices() 让老文件的旧值仍能显示，defaultsFor(SET_MODE) 与内置「常态循迹」范例都改用 TRACE_GYRO；②tools/console/web/src/pages/Tasks.tsx 状态机卡改成状态标签条（主页排第一带徽标、显示块数、在跑打绿点、可点击也可当拖放落点）+ 只渲染一个 StateCard，「加状态」后自动跳到新状态；③docs/任务编程.md:47 的 p0 语义按固件改齐并注明上位机只暴露 0/1/3。验证：tsc --noEmit exit 0；vitest 16 文件/156 passed（taskedit.test.ts 的 5 项断言改 3 项 + 新增 modeChoices/modeLabel/defaultsFor 用例）；npm run build 后 dist/assets/index-Bgo4TFb0.js 337.45 kB + index-GuJrE8M8.css 37.12 kB，8793 返回的正是新 bundle；SSR 预览 out/console/ssr/tasks.html（62035 B）里「进门（可空）」「循环体（末尾要能跳走）」各只出现 1 次 ⇒ 确实一屏一个状态。提交 9a9d626。
未完成：用户还没看过新版界面；模式下拉里「循迹」现在指三层串级，而三层串级在赛道上从没验过（PROJECT.md 记着它是死代码），上车时这一点要单独确认
下一步：等用户刷新 #/tasks 的反馈；之后剩下的唯一大项是真车上车对拍（真板 + 真 Flash 页 59..62）

## 2026-10-03
完成：第 2 轮收口：固件遥测两字段改按「解释器在跑」判 0xFF（host_link.c:443/:472，t_hostlink 新增 17c）、CMD_START_A/B/C 回 ST_UNSUPPORTED(0x08)+文本、CMD_STOP 真停解释器、COMMIT/DEL/RUN 三处 ST_BUSY 守卫；删 renwu.c/renwu.h/build.log/tools/renwu_states.py；桥 JSON renwu→task_id、前端三个「启动任务」按钮换成任务编排链接。验证：make text 114232/data 364/bss 26944、bin 114604（余 6228）、run_tests 13/0（t_hostlink 292 项）、check_static 52 文件 4 告警 0 error、pytest 102 passed、tsc exit 0、vitest 16 文件 155 项、8793 真 HTTP 验收 out/_round2_accept.py 全绿。
未完成：真车链路未验；0x69 状态帧的 running 与遥测两字节在真板上的表现只在仿真里对过
下一步：上车对拍：真板 + 真 Flash 页 59..62 走一遍烧写/回读/运行（当前真车只读链路待硬件）

## 2026-10-03
完成：真 HTTP 验收抓到并修掉仿真侧缺守卫的 bug：tools/console/bridge/sim.py 的 _task_commit/_task_del/_task_run 补 ST_BUSY 早退、web/mock/server.mjs 的 commit/delete/run 补 400 busy（并删掉「删正在跑的槽就顺手 running=false」）；新增 test_sim.py::test_flash_write_refused_while_task_running 与 test_api.py::test_task_write_refused_while_running，pytest 100→102 passed，重启 8770 mock（pwsh-2726）与 8793 桥（pwsh-2727）后验收脚本 D2 从 FAIL 变全绿。
下一步：把这类「仿真必须复刻固件安全规则」的检查当成改协议时的固定动作

## 2026-10-03
完成：任务编排模块端到端打通并交付：车上存储 firmware/App/Svc/task_store.c（页 59 目录 + 页 60..62 程序区 6144 B、最多 8 条、单条 ≤2048 B）、解释器 task_run.c、协议帧 0x60..0x69 + 能力位 bit7、桥端点 /api/tasks*（真桥 tools/console/bridge/server.py + 假后端 web/mock/server.mjs 同契约）、页面 #/tasks（含「开机自启」开关 → COMMIT flags bit0）。验证：pytest 97 passed、npx tsc exit 0、vitest 16 文件/155 passed、pwsh -File firmware/test/run_tests.ps1 通过 13/失败 0（t_hostlink 272 项）、make -B -j8 后 text 114216/data 364/bss 26944、bin 114588 B（离 118K 余 6244 B）、tools/check_static.ps1 扫 53 文件 0 error；另外用 out/_task_flags_http.py 对 8793 的真 HTTP 桥跑 9 项检查（flags=1 落目录、flags=0 复位、回读逐字节、坏 CRC 被拒、删空目录）全绿。
未完成：还没上车对拍（真板 + 真 Flash 页 59..62）；firmware/App/Svc/renwu.c 只留作参考、尚未删除。
下一步：上板验证：烧写一个任务 → 掉电重启看是否按 TASK_STORE_FLAG_AUTO 自启；顺带用真车确认 0x61/0x65/0x69 的回帧节奏在 115200 下不掉帧。

## 2026-10-03
完成：踩坑记录：改了 tools/console 的 Python 桥或 web/mock/server.mjs 之后必须重启那个进程，否则测试/页面用的还是旧代码（这次 vitest 先红一次，就是因为 8770 上跑着 14:49 起的旧 mock；桥 8793 同理，重启后 flags 往返立刻全绿）。固定跑法：`node mock/server.mjs`（workdir tools/console/web，8770）与 `python -m bridge.main --sim --no-browser --port 8793`（workdir tools/console）。
下一步：无需后续动作，仅作下次改动时的提醒。

## 2026-10-03
完成：链路层任务帧 T_TASK_*（0x60..0x69）+ T_HELLO 能力位 bit7 HOST_CAP_TASK 落盘并提交 90a1856：host_link.c 的 8 个 case（LIST/PUT/COMMIT/GET/DEL/RUN/STOP）、pack_task_status、task_tx_poll/task_status_poll 在 host_link_poll 末尾每拍推进一帧；新增 firmware/test/stub/host_taskstub.c 与 t_hostlink.c 24)..32) 共 272 项检查 0 失败。随后提交 9eaccbe：tools/console/tests/test_firmware_pipe.py 的 SOURCES 补真 task_store.c/task_prog.c/task_run.c（不补则整条管道 exe 链接失败、11 个既有用例全 ERROR —— 这个回归是 90a1856 自己造成的），HELLO 能力位断言补 CAP_TASK，并新增 4 条跨语言用例（上传/目录/读回/删除往返、RUN/STOP/STATUS、错误码、驾驶中拒绝起任务），镜像用上位机编码器的金样本 fixtures/task.golden.json、帧用桥自己的 frame_task_*。
未完成：真车只读链路仍缺 J-Link 20 脚排针 pin5/pin17 到 U12 第 5/6 脚的两根线（待硬件）；上位机 Imu.tsx 的能力位图例只列到 bit6，漏了 bit7 任务。
下一步：把「开机自启」打通：给 COMMIT 加一个 flags 字节（固件 host_link.c 的 T_TASK_COMMIT、bridge/protocol.py 的 payload_task_commit、bridge/state.py、web/src/lib/taskapi.ts 四处一起改），否则 TASK_STORE_FLAG_AUTO 永远置不上。

## 2026-10-03
完成：文档 docs/任务编程.md 按实现口径重写并提交：§4 存储（8 条记录 + 页 59 目录 + 页 60..62 程序区 + 先擦目录页语义 + bin 114572B/余 6260B）、§5 帧表（每帧精确载荷 + 空帧收尾/GET 不回 ACK/每拍一帧的理由）、§7 落地顺序、§8.2 证据与待办。先前量出的「118K 只剩 8 字节余量」是树里混着旧 .o 的假象，整份重建后 text 114200/data 364/bss 26944、bin 114572B ⇒ 离 118K 余 6260B。另实测 -Os 备选（text 102368，省约 18KB）记录在案但没有采纳。验证：make -C firmware -j8 exit 0；firmware/test/run_tests.ps1 通过 13/失败 0；tools/console/tests 全量 pytest 94 passed；tools/check_static.ps1 扫描 53 文件 0 error；tools/check_warnings.ps1 待处理告警 0。
未完成：TASK_STORE_FLAG_AUTO 没有下发通路；main_app.c:31 的 include 注释仍写「3 个槽位（页 60/61/62）」，与现在的 8 槽 + 页 59 目录不符（父代理文件的注释漂移）。

## 2026-10-03
完成：固件侧任务存储落地并验证完：`firmware/App/Svc/task_store.c/h`（页 59 目录 + 页 60..62 程序区、8 条记录、CRC16、运动中拒写、手写 FLASH->CR 擦页、擦后复验 0xFFFFFFFF、写后 memcmp 回读）+ `firmware/test/t_taskstore.c`（532 项检查 0 失败，13 节）+ main_app 摘旧 renwu 与无条件 TaskRun_Tick + renwu.c 废弃注释 + docs/任务编程.md §7/§8/§8.2 更新。验证：`pwsh -File firmware/test/run_tests.ps1` = 通过 13 / 失败 0；`make -B -j8`（firmware/）= exit 0，text 114200 / data 364 / bss 26944，bin 114572B（离 118K 余 6260B）；6 项变异（不发出擦除 STRT、Commit CRC 恒真、扫描镜像 CRC 恒真、扫描 Validate 恒真、暂存不够放行、拒写判据全失效）全部被咬住，源码 SHA256 还原为 E0C7D12E…；`tools/check_static.ps1` 扫 53 文件 0 error（task_store.c 零告警）。
未完成：host_link 侧的 T_TASK_* 帧与 t_hostlink/host_taskstub 是并发写者的产物（本会话只改了其中一行坏注释），未提交；桥 tools/console 的 /api/tasks* 端点、pipe_hostlink 对拍、COMMIT 的 flags 字节（开机自启下发通路）都还没做。
下一步：把本批固件侧结果回报父代理，并让用户裁量是否清理 DECISIONS.md 里算错地址的旧页分配条目。

## 2026-10-03
完成：#/tasks 任务/状态机编辑器整条落地：web/src/lib/taskedit.ts（块库 10 种 + 条件 9 种、拖拽插入/移动/跨状态、状态增删复制与跳转改道、名字按 32 字节截断、localStorage hxb.tasks.v1 读写、compileInfo 算 len/CRC16(覆盖 [12,len))/占用%)、web/src/pages/Tasks.tsx（三栏：任务列表 / 状态机卡片 / 块库；编译与烧写卡含红错黄警、烧写后回读逐字节比对、开始-停车-500ms 运行监视）、web/src/lib/taskapi.ts（/api/tasks* 客户端，404/501 或文案匹配不支持时切提示态）、接线 router.ts(PageId+PAGES 10 页)/App.tsx/Shell.tsx 图标 Workflow/render.test.tsx/ssrPreview.test.tsx。另建反向对拍链 tests/c/taskcheck.c + taskentry.ts + emit_tasks.mjs + tests/test_task_firmware_e2e.py（8 项）：上位机 encodeTask 编的镜像交给真 firmware/App/Svc/task_prog.c 校验并逐字段比对。mock/server.mjs 按 API.md §3.6 实现了全部 /api/tasks* 端点（内存假 Flash，commit 校验 magic/版本/状态数/CRC）。验证：npx tsc --noEmit 干净；npm test 16 文件 155 passed（含 contract.test.ts 里一条走真 HTTP 的烧写→回读→运行→错 CRC 必须被拦→删除闭环）；pytest tools/console/tests -q 77 passed；firmware/test/run_tests.ps1 12/12；npm run build 干净 dist/assets/index-B3ScAoku.js 335.35 kB(gzip 115.55) + index-WB_aXEht.css 36.92 kB；SSR out/console/ssr/tasks.html 含 状态机/块库/编译与烧写/任务列表/data-dropline×11，无 NaN。
未完成：真桥（tools/console/bridge/server.py）还没有 /api/tasks* 端点，页面在真桥上是提示态；固件还没有 T_TASK_* 帧（0x60–0x69）与 task_store.c 页 60..62 存储。
下一步：父代理按 API.md §3.6 在桥里转发 /api/tasks*（T_TASK_GET/PUT/COMMIT/RUN/STOP/STATUS），并做 App/Svc/task_store.c 存页 60..62。

## 2026-10-03
完成：车上任务程序第 1~2 步落地并提交（3ec77b7，18 文件 +2985）：firmware/App/Svc/task_prog.{h,c}（magic 'TK'、ver 1、12B 头+8B 状态表+10B 块+名字区、CRC16/CCITT-FALSE、Validate 的末尾块规则只约束 body）、firmware/App/Svc/task_run.{h,c}（非阻塞解释器：enter+body、主页、GOTO、9 类等待条件判据与超时改道、里程按 dt 积分、横线上升沿、单拍 32 块防自环、运行中拒换程序）、firmware/Core/Src/main_app.c 在 renwu 不跑时 TaskRun_Tick()、firmware/test/t_task.c 130 项、tools/console/web/src/lib/task.ts 上位机镜像 + 生成式金样本链（tests/c/taskgen.c → tests/fixtures/task.golden.json → task.golden.ts → task.golden.test.ts）+ tests/test_task_bytes.py 新鲜度对拍、docs/任务编程.md。验证：make -C firmware 干净（text 116344/data 364/bss 17752）、主机测试 12/12 通过、4 个变异各被 1/2/1/8 处断言抓住、check_static 0 error、check_warnings 待处理 0、npx tsc --noEmit 干净、vitest 的 task/金样本用例全过、pytest test_task_bytes.py 5 项通过、重编真 task_prog.c 与 fixture/TS 逐字节一致
未完成：存储/协议/桥/界面未做。另：同一工作树里还有正在被别人写的 tools/console/web/src/lib/taskedit.ts + taskedit.test.ts（13:45 新建，我未提交），其中 2 项用例红：sanitizeBlock({op:SET_SPEED,p0:99999}).p0 期望 7200 实得 65535；compileInfo 对「body 没收尾块」期望 warnings 里有'没有'但只有 errors 里有'最后一块'
下一步：第 3 步：新建 App/Svc/task_store.c 把任务程序存进页 60..62（0x0801E800..0x0801F7FF，6KB），带自己的 magic/CRC 与掉电保护；随后是 T_TASK_* 协议帧（0x60..0x69）、bridge 端点、#/tasks 拖拽界面

## 2026-10-03
完成：IMU 页按用户 m17387/m17401 的要求换成「手写 CSS 3D 车模型 + 偏航/俯仰/横滚三张波形」，朝向滑块保留原样。验证：npx tsc --noEmit 干净；npm test 12 文件 78 passed（含新 lib/attitude3d.test.ts 10 项）；pytest 64 passed；npm run build 干净（dist/assets/index-DOmEJl8X.js 298.60 kB/gzip 102.09、index-CEiZztkN.css 35.15 kB）；SSR out/console/ssr/imu.html 含车姿态（3D）/三张波形卡/car3d-scene + 66 个 car3d-face + 4 个 rosa 刻度标签，无 NaN/undefined，旧 AttitudeGauge 已消失。变异验证：把 carTransform 的偏航符号取反 ⇒ 10 项里 3 项失败；把 stageTransform 的 el 符号取反 ⇒ 10 项里 2 项失败；还原后全绿。commit 7adbdaf。
未完成：3D 的车长相本机无法像素自证（headless Chrome 仍然不产出 PNG），只能靠用户肉眼；目标轮仍 paused（revision 3）
下一步：真车到手后核对三轴方向（+偏航=左转、+俯仰=抬头、+横滚=向左倾），哪个反了就改 web/src/lib/attitude3d.ts 里一行符号

## 2026-10-03
完成：IMU 页「陀螺操作」三个按钮换成「朝向控制」一个 −180°..+180° 滑块（实时下发 `{mode:"base_turn", base:0, turn:offset}`，100ms 节流 + 尾部补一帧，松手保持，「回到 0°」/「交还车上任务」）；操控页转向改成 °/s 角速度（滑块 0..180）+ 前端积分成偏移（±540°），A 的符号修正为左转；Control 页「陀螺校准」死按钮删除；`tools/console/bridge/sim.py` 补角度环闭环 + 基准只在进入时锁存；新增 `web/src/lib/heading.ts`（+13 项用例）与 `tests/test_sim.py` 的角度环用例。提交 `cdff422`（15 files, +891/-345，含 dist 重建与 docs/API.md、web/README.md）。
未完成：真车（串口/烧过的固件）上没验过：角度环的 Kp/限幅只调了仿真常数（ANGLE_KP_RATE 1.5、RATE_MAX 120、DV_PER_RATE 13.4），真车参数要等硬件接上；用户 m17036「校准实际上是干啥的」还没答（已准备好解释：静止采角速度零偏 + 参考朝向重置；0x05 未实现）
下一步：把「校准 = 零偏 + 参考朝向」的解释交给用户，并请他上真车试一次朝向滑块（转向打死点、松手保持、按空格交还）

## 2026-10-03
完成：vitest 11 files / 68 passed（含 SSR 预览重写 9 页、喂 82 帧遥测）；pytest 64 passed（+1 新 sim 用例）；`npm run build` 干净 → `tools/console/web/dist/assets/index-SkZbUweK.js`（295.70 kB / gzip 100.81）+ `index-DME16mg_.css`（32.70 kB）；对 8793 仿真桥端到端：base_turn base=0 turn=+30 后 yaw 从 −51.00 收敛到 −21.00（最小偏差 0.10°、末点 err −0.4），保持 2s 不漂、改成 turn=−30 收敛到 −81.00（末点 err −0.1），base=400 时边走边回正（vl 333 / vr 467），off 后 `drive = {mode:0, enabled:false}`；`mode:9` 被桥拒（驾驶模式不对）。变异验证：sim.py 的「每帧重锁基准」和「去掉角度环」两处变异都被新用例抓住，还原后哈希一致。
下一步：无（本轮验证已闭环）

## 2026-10-03
完成：踩坑记录：用 Python 脚本改仓库文件做变异验证时，`pathlib.write_text` 在 Windows 会把 LF 写成 CRLF，`sim.py` 的 diff 会从 51 行炸成 460/411 行；改回按字节 `read_bytes`/`write_bytes` 后 diff 恢复 51/2。
下一步：以后 `out/_mutate_*.py` 一律按字节读写

## 2026-10-03
完成：操控页改造完成并提交 8b4bc1a：新建 `tools/console/web/src/lib/keys.ts`（WASD/方向键→角色的纯函数映射 + `keyId`/`keyRoleById`/`isTypingTarget`/`rolesToDrive`），`tools/console/web/src/pages/Drive.tsx` 整体重写为键盘操控（只在指令变化时 POST `/api/drive`，失焦/切页/松键立刻交还 off），删掉三模式/快捷动作/预设档与 `settings.drivePresets`。验证：vitest 55 passed（+`keys.test.ts` 11 项）、pytest 63 passed、固件主机 通过 11/失败 0、`npm run build` 干净（index-Acbq5qec.js）；变异（翻转 A/D 转向符号）⇒ keys.test.ts 5 项立刻失败，还原后全过；对 8793 仿真桥打页面同款 POST（base_turn base=600 turn=-500）⇒ ACK OK、遥测 flags.host_drive=true、mode=2，off 后 mode=0/enabled=false。
未完成：真实键盘手感（按住/松开的即时性、失焦兜底、点过滑块后 WASD 还能不能开车）只能由用户在有浏览器的机器上点；真车只读链路仍待硬件；目标轮仍 paused（revision 3）。
下一步：等用户 Ctrl+F5 后在操控页按 W/A/S/D 与 W+A、A+D 核对；若要继续开发，需用户先在界面上 resume 目标轮。

## 2026-10-03
完成：修掉用户 m16508 截图的四条报障并提交 9d7e57f：①舵机页/设置页白屏根因 = 桥 preset_list() 把动作表存档 actions.json 原样吐出，Servo.tsx:209 的 p.keys.join() 抛 TypeError 导致 React 整树卸载（只有这两个页面读 presets，与现象一一对应）；②index.css 漏定义 .seg-item，Tailwind preflight 把裸 button 清成透明无边框 ⇒ 驾驶页三个分段按钮黏成一行；③循迹柱顶"黑条"不是 bug 而是到满量程 255 的未填充余量（186/204 = 73%/80%），把亮边从轨道顶部挪到填充顶部并加图例；④顺手修 mock 的 gray 量纲错（ADC→u8）。验证：pytest 63 passed（+1 用例）、vitest 44 passed / 9 files（+presets.test.tsx 4 项）、npm run build 干净（index-BaUugex3.js）、两处变异都能复现旧故障（api.ts 去掉归一 ⇒ 同一 TypeError @ Servo.tsx:209；state.py 换回 out.append(dict(o)) ⇒ KeyError: 'name'）、SSR 9 份重生成后灰度为 u8（25..180）柱高 9.8%~70.6%。
未完成：goal-30756115-0724-4b6f-849c-327d4bac51ea 仍为 paused（revision 3），只能由用户在界面上 resume；真车只读链路待硬件；像素截图本机不可得（用 out/console/ssr/ 9 份 SSR 替代）
下一步：等用户 Ctrl+F5 复核这四条（桥在 http://127.0.0.1:8793，已确认服务新 dist）；复核通过后若要继续，需要用户先把目标轮 resume

## 2026-10-03
完成：协议层固化闭环落地：管道对拍构建加 `-DHOST_PIPE_REAL_NVRAM`（链真 nvram.c + trace_cal.c，host_appstub.c 的假 nvram_touch/trace_cal_level 用 `#ifndef` 让位，host_pipestub.c 补 g_cfg/app_ui_*/trace_read_channel），pipe_hostlink.c 加 `--flash/--dump-flash` 与 nvram_init/poll/service；新增 pytest test_param_save_survives_a_power_cycle（改 pivot_kd=321 → T_PARAM_SAVE → 页 63 出现记录头 "HXB1"+version 5 → 第二个进程带镜像开机 dump 出 321，不带镜像的对照组回默认）与 test_param_change_alone_does_not_touch_flash（只改不固化且够不着 1.5s 防抖 ⇒ 页 63 全 0xFF）。提交 9fbc1a9。验证：pytest tools/console/tests -q ⇒ 62 passed；firmware/test/run_tests.ps1 ⇒ 通过 11/失败 0；两次变异（拿掉 host_ext.c:226 的 nvram_touch()、不载入 Flash 镜像）都被用例抓住。
未完成：管道构建里的 nvram 只覆盖 "参数固化"；动作表固化的协议层等价用例（T_ACTION_SAVE）还没单独断言落盘内容
下一步：用户要先自己看效果/看代码，目标轮已暂停（goal revision 3, phase paused）；他说继续时再用 update_goal resume 接着找下一个缺口

## 2026-10-03
完成：真 nvram.c（全工程唯一擦写 Flash 的模块）有了主机测试 firmware/test/t_nvram.c（42 项检查，已提交 2deb921）：①空 Flash 不加载；②改 g_cfg/参数表/动作表 → nvram_touch → poll 得 BUSY → service 得 DONE 落盘 → 把 RAM 全清掉再 nvram_init() → nvram_loaded()==1 且 trace_speed/line_kp_x100/speed_kp_x100/轴映射档/HP_PIVOT_KD/HP_GYRO_ANG_KP/HP_GYRO_SETTLE_DEG/HP_PIVOT_KP + 三个动作角度(170,12)/(30,60)/(0,0) 全回来，app_ui_settings_loaded 被调 1 次；③值一致时 poll+service 不擦页（记录外 1900 偏移的哨兵 0x5A 还在）；④1.5s 防抖（1.4s 仍 IDLE、1.6s 才 BUSY）；⑤运动中 poll 不排队、已排队再动起来 service 撤回（1950 哨兵 0x3C 还在）、改回上次存过的值不算变化（1951 哨兵 0x5A 还在）；⑥payload 翻一位或 version=9 → loaded==0、不被坏记录盖。验证：pwsh -File firmware/test/run_tests.ps1 → 通过 11/失败 0/编译失败 0（原 10 个全绿）；C:\Python314\python.exe -m pytest tools/console/tests -q → 60 passed；变异验证：把 nvram.c:280 的 Servo_SetActionPose 回填改成 (void)0 → t_nvram 8 处失败，还原后全绿。
未完成：host_hal.c 的擦除仿真只覆盖'PER|STRT'这一种写法；若将来有代码用 HAL_FLASHEx_Erase 或 MER1/MER2 全片擦，主机上不会生效（目前无此代码）。
下一步：把真 nvram.c 也链进 firmware/test/pipe_hostlink.c 的字节管道（host_pipestub.c 补 g_cfg/app_ui_* 等 5 个符号、关掉 host_appstub.c 的假 nvram_touch、加 nvram_poll/service 调用与 --flash/--dump-flash 文件参数），做协议层端到端：T_PARAM_SAVE → 真落盘 → 重开进程带上 flash 镜像 → 参数 dump 出新值。

## 2026-10-03
完成：真固件源码与上位机编解码器的字节级对拍落地并提交 9717fa0：新增 firmware/test/pipe_hostlink.c（stdin 每字节喂真 host_link.c，推进 5ms 假时钟，回帧从 stdout 出）、firmware/test/stub/host_pipestub.c、tools/console/tests/test_firmware_pipe.py（9 项）、protocol.py 的 param_table_crc(items)，以及让管道构建链真 servo.c 的配套 ifdef 与假 HAL 头补齐（__HAL_TIM_SET_COMPARE + 旧拼法）。验证方式：C:\Python314\python.exe -m pytest tools/console/tests -q ⇒ 60 passed；pwsh -File firmware/test/run_tests.ps1 ⇒ 通过 10 / 失败 0。硬证据：参数 dump 43 项 id 集合与默认值全部等于 bridge/params.py、按 dump 顺序复算的 param_crc == 固件 HELLO 报的 577（按界面 PARAMS 顺序算得 30734，证明顺序敏感）、动作表运行值 == servo.c 的 k_action_default、角度 >180 与中位偏差 ±999 被协议层回 ST_OUT_OF_RANGE 且不落表、驾驶静默 960ms 触发看门狗自动停车。
未完成：真车只读链路仍待硬件（用户决定）；本机拿不到像素级截图，只能用 out/console/ssr/ 的 SSR 快照替代。桩 host_appstub.c 里的 k_def[] 仍是旧台架值，t_hostext 测的是桩自己的动作表（出厂表由 test_firmware_pipe.py 覆盖）。
下一步：把这一轮结果告诉用户（怎么自己复核：pytest tools/console/tests/test_firmware_pipe.py -q；没装 gcc 的机器会自动 skip），然后按目标清单找下一个还没自主验证的缺口。

## 2026-10-03
完成：回放录制闭环做完并验证：设置页新增「回放录制」卡（文件下拉/倍速 0.25–8×/刷新/播放），桥新增 GET /api/recordings 与 /api/link/connect 的 replay/speed 转发，switch() 改成先建新链再停旧链（失败保留原链路）。修掉一个真 bug：tools/console/bridge/link.py 的 ReplayLink.poll() 绕回时写 self.t0=now_ms，而录制 jsonl 的 t_host 是绝对 epoch 毫秒 ⇒ 第二遍永远不出帧（波形放完就静止）；改成 self.t0 = now_ms - rows[0].t_host/speed。验证：pytest 51 passed（新增 test_recordings_and_replay 端到端 + test_replay_link.py 绕圈）、vitest 40 passed（新增 lib/replay.test.ts）、npm run build 干净、真仿真桥手测 connect replay speed=4 ⇒ frames_seen 57→122 且可切回 sim。提交 4d3a2d2。
未完成：真车只读串口链路待硬件（用户选定「先放着」）；像素级截图在本机拿不到（Edge headless 各种模式都不出 PNG），以 out/console/ssr/ 的 SSR HTML 快照替代。
下一步：用户机器上打开 http://127.0.0.1:8793（仿真桥在跑）按代看清单过一遍：控制页点「开始记录」→ 设置页「回放录制」选文件 + 倍速 → 播放，看波形/仪表是否连续循环。

## 2026-10-03
完成：参数闭环修好并提交 8c36b05：tools/console/bridge/state.py 的 param_set 只改桥缓存并按「与车上值是否相同」标脏、param_push 才真下发（T_PARAM_SET + 等 T_ACK/T_PARAM_REPORT，成功后写回 car_values 清脏）、param_reload 是唯一读回全表、param_defaults 只改本机；server.py 的 /api/params/push|defaults 透传 ids/keys 且保存预设时 keys 回填；tools/console/web/src/lib/store.ts 的 setParam 补上此前从未被调用的 api.paramSet。验证：pytest 48 passed、vitest 37 passed、npm run build 绿（dist/assets/index-DhY0ZJzb.js 285.88 kB）、out\_closeloop.py 打仿真桥 8 步全绿、store.test.ts 变异验证（删掉 paramSet 调用即 2 项失败）。
未完成：①真车只读链路待硬件（排针 pin5/17 到 U12 的 5/6 脚连线，或 USB-TTL）；②像素级无头浏览器截图仍未成功（沙箱里 Chrome/Edge headless 失败），现以 out/console/ssr 下 9 份 SSR HTML 快照替代。
下一步：用户给出新 COM 口后：python out\_linkprobe.py <COM> 115200 3 看板子 rx 是否 +3，再 python -m bridge.main --serial <COM> 接通真车只读链路。

## 2026-10-03
完成：修好“浏览器里改参数永远到不了车”：①`tools/console/bridge/state.py` 的 `param_set` 改成只改桥缓存 + 按与车上值是否相同标脏，`param_push(ids/keys)` 才逐条下发并清脏，`param_reload` 才是读回全表，`param_defaults` 只改本机；`preset_apply` 先改缓存再一次性下发（应用预设才真的到车）。②`bridge/server.py` 的 push/defaults 透传 ids/keys，保存预设时 keys 按实际抓到的类别回填（原来不带 targets 存出来是空数组）。③`tools/console/web/src/lib/store.ts` 的 `setParam` 除本地乐观更新外会调 `api.paramSet()` —— 此前该函数从未被任何页面调用，「全部下发」是空转。验证：pytest 48 passed；vitest 37 passed；`out/_closeloop.py` 对仿真桥 8 步全绿（改→脏→下发→不脏→存预设→恢复默认→应用预设→从车读回 2811→固化→动作改/固化/播放/还原→删预设）；`out/_static_check.py` 证桥能把 `web/dist` 托起来（/index.html 200、js 290478 B、健康检查 web_built=True）。已提交 `8c36b05`（工作树干净）。
下一步：真车只读链路仍卡在硬件：J-Link 的 20 脚排线里没有 UART 线（SEGGER 官方文档：SWD 模式 pin 17=NC、pin 19=5V，偶数脚全 GND），所以 COM26 与板子 U12 的 PC0/PC1 无电气通路。需要用户把无线下载器插回 U12（或 USB-TTL 交叉接 U12 的 4=GND/6=PC0/5=PC1——**脚号按网表更正过，早先写的「5=PC0/6=PC1」是反的**），之后 `python out\_linkprobe.py <新COM口> 115200 3` 复验。

## 2026-10-03
完成：删掉旧 Qt 上位机并整仓清理：`tools/host/`（git 跟踪 28 文件、约 8256 行）已从 git 与磁盘删除（从提交 `5051a4d` 可取回），连只服务于它的草稿脚本 `tools/_patch3.py`、`tools/_render_field.py` 一起删；`.gitignore` 补 `firmware/test/build/`（run_tests.ps1 生成的 .exe）与 `.pytest_cache/`，并把已跟踪的 10 个 .exe 从索引移除。三个新提交：`dbda415` 固件 v2 扩展、`cf237eb` 新上位机 tools/console（67 文件）、`daef5ca` 清理；工作树干净。文档指向已改对（`docs/上位机通信协议.md`、`docs/全地形机器人比赛规则与场地.md:137`、`固件接口规格.md:422`）。回归：`pwsh -File firmware/test/run_tests.ps1` → 通过 10 / 失败 0 / 编译失败 0。
下一步：`memory/PROJECT.md` 的「怎么跑和怎么测」还写着“当前 9 个测试”（实为 10，新增 t_hostext）与「现状」里旧的 text 111012/data 368/bss 15264、烧录 111388 字节（现为 114732/364/15656、115104 字节），下次顺手订正。

## 2026-10-03
完成：把「真车串口收不到数据」定性到底：**J-Link 的标准 20 脚排线里根本没有 UART 线**，所以 `COM26 = JLink CDC UART Port` 与板子 U12 的 5/6 脚（LPUART1_RX=PC0 / LPUART1_TX=PC1）之间不存在电气通路 —— 依据是 SEGGER 官方接口文档（SWD 模式 pin 17 = Not used/NC、pin 19 = 5V-Supply、偶数脚全 GND；https://www.segger.com/products/debug-probes/j-link/technology/interface-description/）。固件侧无病：SWD 读到 `s_stats @0x20000CBC` rx=0/tx 以~50Hz 增长/crc=resync=0、LPUART1 CR1=0x2D（UE/RE/TE/RXNEIE）、BRR=0x5C3B2（=170MHz/115200）、PC0/PC1 = AF8+上拉、PC1 的 IDR 在翻转；清掉 ISR(ICR=0x1F) 后静置 1.5s、再让 PC 从 COM26 发 3 帧 HELLO_REQ，IDLE 不再回来且 rx/crc/resync 恒 0 ⇒ PC0 从未收到过字节。本机 J-Link 是 PLUS V9.70 / FW V9（S/N 602711039、VTref=3.309V，SWD 确实通）。
下一步：接真车串口只有两条路：把无线下载器插回 U12（它同时带 SWD+虚拟串口），或用 USB-TTL 交叉接 U12 的 4=GND / 6=PC0（车收，接适配器 TX）/ 5=PC1（车发，接适配器 RX）——**脚号按网表更正过**。插好后 `python out/_linkprobe.py <新COM口> 115200 3` 复验（车侧 rx_frames 应 +3），再 `python -m bridge.main --serial <新COM口>`。

## 2026-10-03
完成：查清「上位机串口收不到数据」的原因，并证明固件侧无辜。SWD 读寄存器：LPUART1 CR1=0x0000002D（UE/RE/TE/RXNEIE 全 1）、BRR=0x0005C3B2（=170MHz/115200，误差 6e-7）、GPIOC PC0/PC1 MODER=AF、PUPDR=上拉、AFRL=0x88（AF8）、s_stats 的 tx_frames 以 ~50Hz 增长且 PC1 的 IDR 在 0/1 间跳（真的在发）；反向证据：清掉 LPUART1 ISR(ICR=0x1F) 后静置 1.5s、再让 PC 从 COM26 发 3 帧 HELLO_REQ，IDLE 位不再回来且 rx_frames/crc_errors/resyncs 恒为 0 ⇒ PC0 上从来没有过一个字节。PC 侧 COM26 = JLink CDC UART Port（VID:PID 1366:0105），写出去的帧自己也收不到。工具（可复用）：out/_linkstats.ps1（-Mode read|probe|idle，靠 arm-none-eabi-nm 解析 s_stats 地址后 mem32 读数）与 out/_linkprobe.py（用 tools/console 的 bridge.protocol.frame_hello_req 发帧、先静听 0.5s 再发再收 1s）。
下一步：把无线下载器插到 U12（或 USB-TTL：U12 脚 6 = PC0 车收 ← 适配器 TX、脚 5 = PC1 车发 → 适配器 RX、脚 4 = GND）后，用 out/_linkprobe.py <新COM口> 115200 3 复验（车侧 rx_frames 应 +N），再 python -m bridge.main --serial <新COM口> 跑真机。

## 2026-10-03
完成：第十一轮（用户点名）：模块测试里加「舵机测试」页（PAGE_CUSTOM_SERVO 17）——两路角度可调（1°/长按 2/5/10/20）、「收回」=0/0、14 号「动作预设」占两行（下行动作名）、返回保持角度；顺带揪出真 bug：bsp_servo_init()（firmware/App/Bsp/bsp_board.c:227）全工程无调用者 ⇒ htim1.Instance=NULL ⇒ Servo_Init() 的 HAL_TIM_PWM_Start 返回错误、__HAL_TIM_SetCompare 写空指针，舵机一直没动过（renwu.c 20+ 处 Servo_Move 也是空动作）；修法=servo.c 的 Servo_Init() 开头调 bsp_servo_init() + include bsp_board.h。验证：SWD 读 TIM1（0x40012C00=1 CEN、0x40012C20=0x1111 四路 CCxE、0x40012C2C=0x4E1F ARR=19999、0x40012C34/38=0x1F4 CCR=500µs、0x40012C44=0x8000 MOE）；make -C firmware 无新告警（text 111012/data 368/bss 15264）；check_warnings.ps1 待处理 0；run_tests.ps1 通过 9/失败 0；flash.ps1 回读一致 111388 字节；字模补字校验缺字 0（舵 74→75、作/预 75→77、动作名一起提交 79 字形）；out/servo_page_mock.py→out/servo_page_mock.png 用同一套坐标假渲染 6 种选中状态，断言“选中项完整可见、无行越界”。
未完成：屏幕实际观感与动作语义未由我肉眼确认（几何自检+假渲染只能证明布局）；动作预设占两行后仍未在真实屏幕上看过
下一步：等用户上板按号试 14 个动作，特别是 5 号/9 号的中键完整播放；有角度或方向不对就改 servo.c 的 k_action_pose 一张表

## 2026-10-03
完成：按用户 m13303 在舵机测试页「收回」下面新增一项「动作预设」：`SERVO_TEST_N` 4→5（行序：舵机1/舵机2/收回/**动作预设**/返回）、新增 `SERVO_SEL_ACTION 3U`（返回挪到 4）、`s_servo_action`（1..14，进页复位成 1）。左/右在它上面按 ±1 在 1..14 循环，**实时把该动作的终点姿态发下去**（`Servo_ApplyAction()`，不走 5/9 号的中间延时，界面不被 HAL_Delay 卡住），并同步上面舵机1/舵机2 两行的数字（用户要的“对应改变”）；中键 = **完整播放**（`Servo_Move()`，含 5/9 号中间过渡，阻塞 0.2~0.5s，只有这样才能看到复合动作的中间过程），播完回读 `Servo_GetAngle()` 校正两行数字；行尾显示 `n/14`。同时把 `firmware/App/Bsp/servo.c` 的动作表重构成单一真值来源 `k_action_pose[14][2]`（`Servo_Move()` 改成查表 + 5/9 号特例，行为与原 switch 逐项一致），新增 `Servo_ActionCount/ActionPose/ApplyAction/GetAngle`（servo.h 里 `#define SERVO_ACTION_N 14`）。字模重生成（缺 作 U+4F5C / 预 U+9884）⇒ 字形 75→**77**、缺字 0。验证：`make -C firmware` 无新 warning、`check_warnings.ps1` 待处理 0、`run_tests.ps1` 通过 9 / 失败 0、`tools/flash.ps1` 烧录回读一致 **110908 字节** firstDiff=none。
下一步：等用户现场按号试 14 个动作（重点 5/9 号复合动作的中间过程），报哪个角度要改；角度只改 `firmware/App/Bsp/servo.c` 的 `k_action_pose[]` 一处（比赛流程与测试页同时生效）。

## 2026-10-03
完成：按用户 m13266 把舵机页的「回中（90°）」改成「收回（0°）」：`firmware/App/Src/app_ui.c` 里 `SERVO_SEL_CENTER`→`SERVO_SEL_RETRACT`、`SERVO_DEG_CENTER 90`→`SERVO_DEG_RETRACT 0`、`k_servo_names` 第 3 项 "回中"→"收回"、行尾数值改成显示 `0/0`（对应旧动作表 `Servo_Move(1)`：舵1=0、舵2=0）、注释同步；中键在「收回」上就是两路一起回 0°。字模复核 `缺字=0`（收/回 已在子集内，不需重生成）；重编无新 warning，烧录回读一致 **110596 字节** firstDiff=none。用户已现场确认：两路没接反、0° 正好是收回位置，舵机页能动了（前一版就修好了 `bsp_servo_init()` 从未被调用的根因）。
下一步：等用户按动作表逐条试舵机（尤其 5/9 号那两个带延时的复合动作），确认角度方向/大小；如果某些动作要改，改 `firmware/App/Bsp/servo.c` 的 `Servo_Move()` 表。

## 2026-10-03
完成：用户现场报障「进舵机测试页按了舵机不动」，定位并修掉真根因：**TIM1 从来没被初始化过** —— `firmware/App/Bsp/bsp_board.c:227` 的 `bsp_servo_init()`（配 PA8–PA11 = AF6_TIM1 + PSC=169/ARR=19999 + 四路 PWM 通道 + Start）**全工程没有任何调用者**，而 `firmware/App/Bsp/servo.c` 的 `Servo_Init()` 原来只调 `HAL_TIM_PWM_Start(&htim1, SERVO1/SERVO2_CHANNEL)` —— `htim1.Instance` 还是 NULL（全局零初始化）⇒ HAL 返回 HAL_ERROR、`Servo_SetPulse()` 里的 `__HAL_TIM_SetCompare()` 写在空指针上，舵机零反应（这也意味着**新板烧上后舵机从来没能动过**，`renwu.c` 那 20 多处 `Servo_Move()` 一直是空动作）。修法（对照 `motor.c` 的 `Motor_Init() -> bsp_motor_init()` 套路）：`servo.c` 加 `#include "bsp_board.h"`，`Servo_Init()` 开头调 `bsp_servo_init()`，并删掉原先那两句多余的 `HAL_TIM_PWM_Start()`。已确认不是 MOE 问题（HAL 的 `HAL_TIM_PWM_Start()` 对 TIM1 会自己 `__HAL_TIM_MOE_ENABLE`，查了 `stm32g4xx_hal_tim.c`）。验证：`make -C firmware` 无新 warning（text **110216** / data 364 / bss 15264），`tools/flash.ps1` 烧录回读一致 **110588 字节** firstDiff=none。
下一步：等用户再试舵机页：能动了就按他要的「一个动作一个动作试」确认哪路是几号舵机、方向对不对；还不动就查舵机 5V 电源轨/接线（软件路径已排除）。

## 2026-10-03
完成：按用户 m13059 做了「模块测试 → 舵机」调参页（整块新代码，2026-10-03 凌晨）：①`firmware/App/Inc/app_ui.h` 新增 `PAGE_CUSTOM_SERVO 17` / `PAGE_COUNT 17` / `CUSTOM_SERVO 8` / `CUSTOM_COUNT 9`；②`firmware/App/Src/app_ui.c` include `servo.h`、模块测试页从 3 项改成 4 项（蜂鸣器/电机/舵机/返回，舵机行尾显示 `s_servo_deg[0]/[1]` 两路当前角度）、新增整个舵机页（`SERVO_TEST_N 4` = 舵机1/舵机2/回中/返回；上/下换项、左/右拖角度（短按 1°，长按 1/2/5/10/20 连发）、中键在「回中」两路一起回 90°、在「返回」退出；改角度立刻 `Servo_SetAngle(1/2, …)` 直接到位，不缓动；退出保持最后角度，同电机页规矩）、`k_routes[]` 加 `[PAGE_CUSTOM_SERVO-1]`；③先踩了一个顺序坑：`s_servo_deg[2]` 原来声明在舵机页块里（在模块测试页之后）⇒ 模块测试页用它编译报 `error: 's_servo_deg' undeclared`，已把声明上移到电机页静态量旁边。验证：字模重生成（原来缺「舵」U+8235，KK_OLED 会静默不画）——`node tools/font/gen_font.mjs firmware/App/Fonts/kk_font_body.c wenquanyi_9pt 12 kk_font_body "舵机测试回中"` ⇒ 字形 74→**75**、缺字 0；`make -C firmware` 无 warning（text **109932** / data 364 / bss 15264）；`run_tests.ps1` 通过 9 / 失败 0；`check_warnings.ps1` 待处理 0；`tools/flash.ps1` 烧录 + 回读一致 **110304 字节** firstDiff=none。
下一步：用户上手试舵机页（他说「我先来试一试，大概告诉你」）——页面操控/角度方向/哪路是几号舵机由他现场确认；之后可能要把实测的舵机零点写进 `servo.c` 的 `s_dev[]`（软件中位偏差）或者修 `Servo_Move()` 的动作表。

## 2026-10-03
完成：第十轮（台架+代码）：①用户 m12856 把最终形态定成两种 —— 正常循迹 = 三级 PID 串级（循迹→角度→速度）、进障碍流程 = 只有速度环+角度环（两级）；核实到 `firmware/App/Svc/motion.c:149-176` 里 `MOTION_TRACE_GYRO → motor_tracing_cascade()` 是**死代码**（全工程 `Motion_SetMode()` 只有 motion.c 内部切 IDLE 与 `main_app.c:251` 的 `Motion_Stop()`），而 `renwu.c` 直行仍 97 处调 `motor_tracing_pid(4.5)` → 旧两级 `motor_tracing_pid_speed()`。②把 `firmware/App/Svc/PID_tracing.c` 的 `TRACE_ANGLE_PER_UNIT`/`TRACE_ANGLE_MAX` 改成运行期变量 + `PID_Tracing_SetCascadeGains/GetCascadeGains`（默认值不变，夹子 0.5~60°/格、1~90°，0/NaN/越界回落出厂值）。③`out/_tracetest.ps1` 加 `-Mode pivot3`（poke `s_mode=3`、motion `s_base=0`、`gyro_control_enabled=1`、`s_base_valid=0`、清角度环 8 个状态）、`-AnglePerUnit`/`-AngleMax`（只在阶段 2 生效）、`s_frames` 活性行。④14 次受控实验（偏置 ±0.8、每档 2 次）：4°/格 0.523/−0.256、6 0.184/−0.277、8 0.017/−0.187、**10 0.040/0.079**、12 0.091/−0.088、15 −0.087/−0.119、20 0.048/−0.074 ⇒ **10°/格 保持不动**，推演“应调到 5~7”被实测否证；每个轮的 `最大左轮目标` 都是 2000 mm/s = 角度环 `s_corr_limit` 顶满、全程没丢线。⑤写进 `docs/自主优化记录.md` 第 16 节（含 16.7 未做的 `renwu` 接线与 16.8 两个操作坑）。构建 `text 108692 / data 364 / bss 15216`，烧录回读一致 109064 字节。
未完成：`renwu.c` 接入三级串级还没做；直行串级的标定也没做（用户还没放开前进测试，pivot 的结论不能直接搬）。
下一步：把三个旧包装 `motor_tracing_pid()`/`motor_tracing_pid_0()`/`motor_tracing_pid_0_task()` 内部改调 `motor_tracing_cascade(trace_base_pwm*())` 并自动使能角度环（`renwu.c` 97 处一处不用改），旧路径留编译开关可一键回退，重编+主机测试，直行行为标“未验证”。

## 2026-10-03
完成：第九轮：按用户 m12763 要求自己台架标定 pivot（原地转圈）循迹 PID 并落地。①扫描：`out/_tracetest.ps1 -Mode pivot -Setpoint 4.5 -Offset 0.6 -OffsetMs 1000 -SettleMs 600`（先把线停到 4.5+0.6 静置、再阶跃回 4.5）；Kp∈{600,900,1200} × Kd∈{0.15,0.30,0.45}×Kp（Ki=0）网格 + 决赛圈 4 组各 3 次 + 偏置 1.0 / 设定值来回切换鲁棒性 ⇒ **选 Kp900/Ki0/Kd405**（保守备选 Kp600/Kd270）。②落地：`firmware/App/Svc/pid.c` 加 `PID_PIVOT_KP 900 / PID_PIVOT_KI 0 / PID_PIVOT_KD 405` 与 `PID_SetPivot(int)`（只在模式变化时清状态），`PID_Compute()` 按 `s_pivot` 选增益；`firmware/App/Svc/PID_tracing.c` 的 `motor_tracing_pid_speed()` 里加 `PID_SetPivot(base_speed == 0)`。③验证：`t_pid.c` 加第 7 组 5 项 ⇒ 61 项 0 失败；全套 `run_tests.ps1` 通过 9 / 失败 0；`make -C firmware` 无 warning `text 108580 / data 356 / bss 15216`；`tools/flash.ps1` 回读一致 108944 字节；**不带 -Kp/-Kd 覆盖**的实测（`out/_P1_fw_default.txt` / `_P2_fw_default.txt`）0.6 单位扰动 0.552→0.026 / 0.551→0.028、峰值命令 496/778（旧 2700 同协议是 2817）⇒ 常量确实生效。④写进 `docs/自主优化记录.md` 第 15 节（顶部引用块补“第九轮 = 第 15 节”）。
未完成：静摩擦死区：来回切换场景里约 1/3 次最后停在 -0.29 单位（≈1.5°）外不收；Ki=0 下只能靠比例项，属摩擦力问题不是增益问题（Kp 加大反而出现 -0.24 停偏）。
下一步：下一轮做“卡住检测 + 助推”：因为没有编码器测速，用“重心 data 连续 N 拍几乎不变且 |误差|>0.15”判卡住，给一个方向正确的助推、一动就撤（参考角度环 `s_boost_*`），再用同一套偏置/切换协议验证不引入极限环。

## 2026-10-02
完成：第八轮（台架，车在原地、只允许左右转）：①A1 = 温和增益（Kp600/Ki0/Kd90）下把目标值来回切 4.5→6.5→4.5→3.0→4.5，全程净转 **−0.24°**、跑完仍在线上（`out/_a1_small_sw.txt`）；对照第一次用固件默认 Kp2700 时切换后转了 **+212.76°** 并甩线（我的操作失误）。②A2 = 偏置 0.6 后启动环：Kp1200/Kd180 残差 0.097（转 −2.43°）、Kp1500/Kd225 残差 −0.017（转 −3.05°），两组都干净。③A3 = Kp600/Ki10.8/Kd90 + 偏置 0.5/300ms ⇒ 误差 0.427→**0.005**、转 −3.52°、无 windup（对比偏置 0.9/3000ms 时 Ki 把车甩飞 −131.9°）。④Kp 阶梯 200/300/450/600/900（Kd=0.15Kp）五金都未转飞，但 300/450 停在 0.31/0.23 个单位收不回去（静摩擦死区）⇒ 可用区间 Kp 600~900。⑤C 组“运动中不写 Flash”：`out/_steptest.ps1 -FlashArm` poke `s_saved[0]`(50→AF) + `s_change_ms=uwTick`，再跑 60° 原地转；3.5s 内 `s_state` 一直 00(IDLE)、`s_saved` 未被刷新 ⇒ **一次都没写**；停下 3s 后 `s_saved[0]` 回到 50 = `s_live[0]`（只有 `write_record()` 成功会这么做）⇒ 延后的保存自己补上了。验证：每轮收尾读回 `s_mode/s_enabled/s_fault/gyro_control_enabled` 全 0 + TIM8 四 CCR=0；`-Mode read` 复核 `LINE_KP/KI/KD` 仍是 2700/10.8/360（编译默认值一行未改）。
未完成：pivot 要不要单独做一组增益（建议 Kp 600~900）——等用户定夺；`motor_tracing_cascade()` 丢线仍保持 `aim`（有界，留作后续）。
下一步：把 A1/A2/A3 + C 的结论向用户报告，并问是否给 pivot 单独开一组参数（不动全局 LINE_KP）。

## 2026-10-02
完成：丢线保护修复的端到端验证（车被用户放回线上后）：`out/_tracetest.ps1 -Mode pivot -Setpoint 4.5 -Switches "2.0@1000,4.5@2200" -Samples 3000` 重跑同一场景（`out/_sw2_fix.txt` / `_samples.csv`）—— 线在 2.31s 丢掉后 `reqL` 从 +4385 → +537 → **0**（约 0.9s），yaw 停在 −83.97° 不再动 ⇒ **车停下来了**（修前同一位置是 `reqL` 卡 7200、yaw 绕 360°+ 转 4 秒以上）。短暂丢线期间仍正常找线（2.4~4.0s 的 data 还在 2.2/1.06/3.5/1.76/0.59/4.93 变）⇒ 原行为未破坏。第一阶段（目标 4.5、误差 −0.381）收到 0.095（最小 0.005）、只转 +2.80°。已写进 `docs/自主优化记录.md` 第 13 节（13.1 现象/13.2 根因/13.3 改法/13.4 验证/13.5 顺带结论）并把头部引用块改成“第七轮 = 第 13 节”。另新增分析脚本 `out/_lostcheck.py`（对比修前修后的丢线行为）。
未完成：pivot 增益还没测完（Kp1200/Kd180、Kp1500/Kd225、小 Ki + 短偏置）；4.5↔6/7/8 小幅来回切换还没跑；文档里“给 pivot 单独一组增益”的建议等你定。
下一步：车在线上：先跑小幅设定值来回切换（4.5↔7）看能否干净跟随，再跑两档增益对比；之后讲/跑剩下的待测项（循迹页按键、运动中不写 Flash、电池四项等）。

## 2026-10-02
完成：第六轮（循迹环原地 pivot）与丢线保护修复：①用 `out/_tracetest.ps1` 做了四组“先偏置、断电静置、再启动环”的对比 —— 出厂增益静止起步 0.756→0.032、0.66s、只转 −4.88°（用户要的“瞬间到中间”成立），但偏置 0.9 时转过头 −42° 甩线；Kp600/Ki0/Kd90 不甩线却卡在 0.19 残差（纯 P 死区）；加 Ki=10.8 ⇒ windup ⇒ −131.91° 甩线。结论写进 `docs/自主优化记录.md` 第 12 节，**编译默认值一行没改**。②按用户 m12506 做了“运行中切设定值”（新增 `-Switches '2.0@1000,4.5@2200'`）：4.5→2.0 时命令满舵 7200、线滑出探头后 `reqL` 卡 7200 达 4 秒以上、yaw 绕 360°+ ⇒ 揭露真实缺陷“丢线后满舵打转”。③修：`firmware/App/Svc/PID_tracing.c` 新增 `TRACE_LOST_DECAY_MS 500U`，丢线超 `TRACE_LOST_MS`(300ms) 后在 500ms 内把保持的转向线性收到 0（短暂丢线仍保持转向）。验证：`make -C firmware` 无 warning（`text 108520 / data 356 / bss 15216`）、`run_tests.ps1` 通过 9/失败 0、`check_warnings.ps1` 待处理 0、`tools/flash.ps1` 回读一致 **108884 字节**。
未完成：修复的端到端验证：用 `out/_losttest.ps1` 想直接造“丢线”失败（路面白底时 `all_sensors_lost` 恒 0），必须在线上重跑 `out/_tracetest.ps1 -Mode pivot -Setpoint 4.5 -Switches "2.0@1000,4.5@2200" -Samples 3000`，确认丢线后 `reqL` 0.3~0.8s 内归 0；写进文档第 13 节。
下一步：等用户把车放回线上后重跑该场景；然后讲解并跑剩下的待测项（Kp1200/Kd180、Kp1500/Kd225、小 Ki + 短偏置）。

## 2026-10-02
完成：第六轮（循迹环 pivot PID 实测，台架，2026-10-02 晚）：按用户要求（m12259–m12269 只允许原地左右转、验收=线在正中间 data=4.5；m12324 先偏一个角度再启动环看能否瞬间到中间）做了四组实测。新工具 `out/_tracetest.ps1`（read/pivot，支持 -Offset/-OffsetMs/-SettleMs 两段式：先让环把线停到 4.5+Offset，再断电静置，最后阶跃回 4.5）、`out/_grayread.ps1`（快速只读）、`out/_tr_stats.py`。结果：①出厂 2700/10.8/360，静止起步 0.756 单位误差 ⇒ 0.66s 收到 0.032、只转 -4.88°、无反号（就是用户要的“瞬间到中间”）；②同增益但先偏 0.9 再启动 ⇒ 净转 -42.01°（约 7 倍）并甩丢线（LIT=0）；③Kp600/Ki0/Kd90 ⇒ 只转 -13.03°、reqMax 525mm/s、跑完仍在线，但残差卡在 0.188（纯 P 死区：0.19 单位只给 114mm/s）；④同 C 加 Ki=10.8 ⇒ 偏置阶段积分 windup ⇒ 净转 -131.91°、甩线。机理：探头看不到线后 data 不刷新（冻值继续算）⇒ 转过头 >15~20° 就回不来；重心↔转角强非线性（单位/度 0.016~0.216）。结论：**编译默认值一行未改**，建议改成“pivot 单独一组增益”（需用户定夺）。已写进 `docs/自主优化记录.md` 第 12 节。
未完成：车被 `_trD` 一甩之后平移离开线所在区域（±6° 连续 14+20 步均 LIT=0），**靠旋转找不回来，需要人工把车放回线上**；Kp1200/Kd180、Kp1500/Kd225、“小 Ki + 短偏置”三组对比未做。
下一步：用户把车放回线上后，跑 `out/_tracetest.ps1 -Mode pivot -Setpoint 4.5 -Offset 0.9 -OffsetMs 800 -SettleMs 1500 ...`（先把静置时间从 400ms 加到 1500ms 验证“出厂增益+足够静置能否不甩线”），再跑 Kp1200/Kd180 与 Kp1500/Kd225。

## 2026-10-02
完成：第五轮上板（台架）角度环旋钮扫描：目标是查“转弯时 corr 在 ±2000 反复打满”能不能既平滑中段又不动“不晃”，判据=两头都赢才改默认值。改造 `out/_steptest.ps1` 支持 RAM 调参 `-RateKp/-RateKd/-CorrSlew/-CorrLimit/-Decel`（开始前读出原值、收尾写回，不会把板子留在改过的增益上）。扫了 10 种组合（每次 2200 点 ≈ 3.3s，统一 24ms 口径）。结论：**保持默认**（`s_rate_kp=250`/`s_rate_kd=4`/`s_corr_slew=300000`/`s_corr_limit=2000`）；中段打满是结构性的（速率环线性区只有 ±8°/s，巡航速率误差 30~110°/s），而 slew 限幅把翻转 9→5 的代价是到位后回摆 0.13~1.00° → 5.91°；Kp 降到 18 直接失稳（超调 -55.5°、尾部还在 25°/s 转）。另两个真实发现：①13 次转弯中 `out/_U6.txt` 卡在离目标 8.36°（比例命令被 2000 限幅、低于实测 ≈3450 的起转阀值），“提高限幅到 3600”实测既不快（起步 318ms 仍在 233~348ms 区间）又更差（残差 1.4~2.4° vs 0.4~1.0°）⇒ 不改；②扫参时 7 次同向跑导致车累计转了 ≈+472°、有线缠住的风险，已用 8 次 -Deg -60 回绕（净 ≈-8°）。图：`out/_K_tradeoff.png`；文档：`docs/自主优化记录.md` 第 11 节。
未完成：卡死事件（离目标 8.36°）的根因没定论（怀疑地面摩擦/打滑），也没找到既不伤收尾又能避免卡死的改法；循迹回归、运动中不写 Flash、`gyro_ok` 上界面、电池四项、任务流程 A/B/C 上车都还没做。
下一步：等用户定下一步：循迹回归（只能原地左右转）或继续别的功能项；若还要碰角度环，下一个值得试的是“卡死检测→持续最大助推”这种改代码的方案（而不只是旋钮）。

## 2026-10-02
完成：第四轮 + 上板实测（2026-10-02 晚，主板已连）：①离线：新建 `tools/check_warnings.ps1`（把主程序实编的 43 个文件用 -Wextra/-Wswitch-enum/-Wshadow/-Wmissing-prototypes 等再编一遍，白名单 28 条/待处理 0）；揪出并删除 `firmware/App/Bsp/motor.c:19` 的非 static `int abs(int)`（零调用但顶掉 newlib 的 abs）；`renwu.c` `Toggle_Gyro_Control` 改 static、`renwu()` switch 补 default（野值不再停在上一拍输出）；新 `firmware/App/Inc/oled_diag.h`；`tracing.h` 补原型；`app_ui.c` `k_int_bindings[]` 补显式 NULL。②上板：烧录回读一致 108836 B；RTT `ISR: avg=9us max=12us n=200~201 over=0`（约占 0.2%）；空闲 14s `i2c recover=0` 无误报；用 J-Link 把 PB9(I2C1_SDA) 从 AF4 改成开漏输出拉低把总线真拉死 ⇒ 十几秒后 `i2c recover=1`、控制中断计时不变、读回 `GPIOB->MODER=0xFFABAABF`（固件自己恢复的引脚）、屏继续刷、主页电量 67% 又有（INA226 可读）；屏蔽 USART3 中断（USART3_IRQn=39，`w4 E000E184,00000080`）+ `gyro_control_enabled=1` ⇒ 1s 内 `fault=1` 且 `gyro_control_enabled`/速度环 `s_enabled`/`s_mode` 全归 0（真的会停），恢复后 `fault=0`；`gyro_ok`(0x200007A1) 1→0→1。③软启动修正 A/B：新写 `out/_steptest.ps1`（1.4ms/样本，地址从 build-app/*.o 现查，收尾把 TIM8 四个 CCR 写 0 并回读 00/00/00/00），用 `-Ramp 300000` 复现旧的 `<=` 缺陷，做 2（±60°）×2（20000/300000）×3 共 12 次；新 `out/_csv2log.py --down 24` 把高采样数据降成 24ms 口径后统一用 `out/_angle_stats.py` 算：12 次 tail|v| 全 0、nBack 11/12 为 0、backDeg 0.13~1.51°（出厂基线 9 次是 0.58~3.84°）、摆 corr 5~10（基线 5~11）⇒ 软启动修正没让“不晃”变差；ramp=300000 那组确实更冲（peak 更高、minV 更深、backDeg 约两倍），证明修的方向对。图 `out/_AB_figure.png` / `out/_step_cmp.png`。④文档 `docs/自主优化记录.md` 新增第 9 节（告警集 + abs）与第 10 节（上板实测逐条结论 + 2×2×3 表）。
未完成：转弯过程中 `corr` 会在 ±2000 之间连续打满、角速度 0↔60°/s 起伏（出厂基线也如此，不是回归）——“转的过程一顿一顿”还没治。
下一步：下一轮候选：查 `s_law=3` 速率环（`s_rate_kp 250` / `s_corr_limit 2000` / 非对称斜坡）为什么会打满；或者铺循迹回归（车在地上只能原地转）。

## 2026-10-02
完成：第三轮自主优化（主板未连，全离线）：①发现 `firmware/Core/Src/board.c:286` 的 `board_i2c_bus_recover()`（手动打 9 个时钟 + STOP + 重配 I2C）**全工程无人调用** ⇒ 总线被从机拉死后永不自愈（屏黑、电流/灰度全读不到，只能断电）。②新增 `firmware/App/Svc/bus_health.h/.c`（纯逻辑可单测）：`BUS_HEALTH_FAIL_LIMIT 5U`、`BUS_HEALTH_RECOVER_MIN_MS 1000U`；`bus_health_report(ok)`/`bus_health_pending()`/`bus_health_take(now_ms, bus_idle)`/`bus_health_count()`/`bus_health_fail_streak()`。③`board.c` 三个薄封装 `board_i2c_report`/`board_i2c_watchdog`（take 成功后调 `board_i2c_bus_recover()`）/`board_i2c_recover_count`；报告点接在 `trace_cal.c` 的 `source_read()`（I2C 分支）与 `app_ui.c` 的 `battery_poll()`（INA226 两读），执行点接在 `trace_cal_poll()` 20ms 闸门后（`OLED_IsBusy()` 当空闲判据）与 `battery_poll()` OLED 闸门后；`main_app.c` 开机 `bus_health_init()` + RTT `ISR:` 行尾加 `| i2c recover=%lu`。④新测试 `firmware/test/t_bushealth.c`（33 项）与 `t_tracecal.c`（103 项）；主机桩 `stub/host_board.c` 加了 `host_uart4_reply_set()`/`host_uart4_reply_wobble`（模块会应答 0x57 请求 + 可造抖动）与 `board_i2c_report/watchdog/recover_count` 假实现 + `host_i2c_*` 观测口；`run_tests.ps1` 的 `$deps` 加 `t_bushealth`。验证：`run_tests.ps1` 通过 9 / 失败 0（全套约 12.3 万项）、`tools/check_static.ps1` 49 文件 0 error（仍只有 8 处故意 while(1)）、`make -C firmware` 无 warning `text 108460 / data 356 / bss 15216`。⑤写进 `docs/自主优化记录.md` 第 8 节（含待上板 3 条：拔模块看 recover 涨不涨 / 恢复那下是否阻塞约 12ms / 正常情况下 recover 应恒 0）。
未完成：I2C 恢复的真实效果、阻塞时长、正常工况下 recover 是否恒 0 —— 都要上板才能确认（已在文档标“未验证”）。
下一步：第四轮候选：`app_ui.c`（2090 行）的 `-Wmissing-field-initializers` 消噪 + 回调解引用审计；或把 `firmware/test` 接进一条 `make test`。

## 2026-10-02
完成：第二轮自主优化：①陀螺失联看门狗扩面 —— motion.c 判据由 Motion_GetMode() 改成 GyroControl_IsEnabled()（带上升沿 s_gyro_watch 宽限），失联时 MOTION_FAULT_GYRO_STALE + GyroControl_Stop/SpeedControl_Stop/Motion_SetMode(IDLE)，新增 Motion_ClearFault()；main_app.c 进任务前清故障、renwu() 回来若有故障就 s_renwu_active=0/renwu_flag=0/renwu_stop()。②firmware/test/t_motion.c 新增 2g/2h/2i，检查数 67→82。验证：t_motion 82 项 0 失败、全套 7 个测试‘通过 7/失败 0’、make -C firmware 无 warning（text 107884 / data 356 / bss 15200）。③新工具 tools/renwu_states.py（纯文本分析 renwu.c 的 A() 状态机）与报告 out/renwu_states.txt：从 0 出发 21 个状态可达，死分支 8/12/14/15/16/20/22；8/12/14/15/16/20 的调用都能在跳过它的分支里找到（动作已合并，只是死条件），22 的 GyroControl_TurnRight + PID_Tracing_Timer_Start/Stop 在 21 里没有，且 21 里对应几行是被注释掉的、末尾直接 black_num=23 ⇒ 21/22 是同一路口的两种走法，生效的是 21。结论写进 docs/自主优化记录.md 第 7 节（含‘未验证’标注）。
未完成：①看门狗扩面与 renwu 退任务都只有主机侧证据，要上板才能确认（任务流程转弯时拔陀螺）；②renwu.c:778 是否把 black_num=23 改回 =22（恢复那一段右转+循迹计时）要用户定夺；③renwu.c:974 的 HAL_Delay(10000) 仍会冻住主循环，改动需上板；④tools/renwu_states.py 只覆盖 A()。
下一步：第三轮：app_ui.c（2090 行）回调解引用与 -Wmissing-field-initializers 消噪，或 trace_cal.c 的 IIC 失败/中断恢复路径；顺手把 firmware/test 接一条 make test。

## 2026-10-02
完成：完成第一轮自主优化（主板未接，全部为离线证据）：修 3 个高危问题（gyro 失联会导致车冲出去→新增看门狗；运动中擦 Flash 拖住控制中断→nvram 拒写；NormalizeAngle 中断内死循环→fmodf）+ 8 处中低问题。新建 `firmware/test/` 主机侧单测基建（8 个测试、约 12.3 万项检查全绿、UBSan trap 模式）与 `tools/check_static.ps1` 静态分析基线（48 文件、0 error、仅 8 处故意 while(1) 告警）。构建 `text 107780 / data 356 / bss 15200` 无 warning。汇总文档 `docs/自主优化记录.md`。
未完成：文档 §5 的 6 条待上板验证（最要紧：软启动判据修正后重测 60° 台阶的"不晃"指标；拔陀螺仪看门狗测试；ISR 计时实际值）
下一步：下一轮：审计 `renwu.c`（比赛任务流程状态机/急停路径）、`app_ui.c` 消 `-Wmissing-field-initializers`、`trace_cal.c` 的 IIC 失败路径。

## 2026-10-02
完成：治好了角度环"在目标点左右晃两下"：根因是 `firmware/App/Svc/speed_control.c` 的全局目标斜坡 `SPEED_RAMP_DEFAULT 20000` PWM/s —— 角度环反打 ±2000 的差速换向要 200ms，这 200ms 里轮子还按旧方向使劲，车先倒转一下再回来。改法：①`speed_control.c` 新增非对称斜坡 `s_ramp_brake = SPEED_RAMP_BRAKE_MAX 300000`（往 0/反号方向立刻给），加速方向仍 20000；②`gyro_control.c:129` `s_rate_kd` 0 → 4.0f。实测（编译默认、无 RAM 覆盖，6 次 60° 台阶）：往回转 backDeg 3.95~9.16° → 0.58~1.64°、rate < −15°/s 的采样点 6~13 → 0~1、超调 −0.45~+0.12°（无超调）、进 ±2° 1.55~1.74s、收尾 0.31~1.50°、尾巴全 0。A/B 铁证：在运行中把 `s_ramp_brake`(0x200000C0) poke 回 20000（其余参数不变）→ backDeg 立刻回到 2.19/3.13/3.84°、nBack 0/2/5；再写回 300000。图 `out/_angle_nowobble.png`（log `out/_g_1.._g_6.log`、A/B `out/_ab_1..3.log`）。
未完成：循迹受非对称斜坡影响的实车验证未做
下一步：真车跑一次循迹：非对称斜坡是全局的，循迹差速反号也会变快，确认循迹没变差（车现在架着，测不了）

## 2026-10-02
完成：`out/_angle_stats.py` 成为"晃"的裁判：新增 `minV`（峰值后 min rate）、`nBack`（峰值后 rate < −15°/s 采样点数）、`backDeg`（峰值后倒转总角度 Σ(−v·dt)），加上早先的 `摆corr`/`摆rate`。表头：run/overshoot/peak/t(+2)/t(+1)/final/tail|v|/tail|corr|/摆corr/摆rate/minV/nBack/backDeg。`out/_plot_angle.py` 新增 `ANGLE_TITLE` 环境变量传标题 —— PowerShell 把中文当命令行参数传 python 会走 ANSI 代码页乱码，环境变量才靠得住。
下一步：以后每个角度环参数都用这套指标验收

## 2026-10-02
完成：按用户 m10055"好好修"改完并上板实测：`firmware/App/Svc/gyro_control.c` 新增 `volatile float s_settle_rate = 30.0f`（`:102` 附近，含扫值记录）+ `s_rate_kp 250`（`:97`）+ `s_ang_kp 3.5`（`:90`）；law3 带内释放 `:604`、锁存速率判据 `:660`（原 s_ff_rate*2）、锁存漂出解锁 `:558` 前。`make` text 106012 / data 348 / bss 15160，`tools/flash.ps1` 回读一致 106368 字节 firstDiff=none。实测 4 次 60° 原地台阶（compiled 默认，无 RAM 覆盖）：超调 −0.04/−0.07/0.03/0.38°、峰值 100/96/84/103°/s、进 ±2° 1624/1716/1672/1717ms、收尾误差 0.95/1.32/0.06/1.52°、**tail|v| 与 tail|corr| 全为 0**（对照：修前 4 次里 3 次尾巴 ±2000 PWM 一直抽、|v| 20~50°/s）。图 `out/_angle_final.png`（`out/_plot_angle.py` 画）。
未完成：残留：收尾误差最大 1.5°（比修前的亚度级差），`s_settle_rate` 是权衡旋钮；地面摩擦与架空台架不同，数值需现场复核。
下一步：现场（地面上、轮子吃地）复测一次，确认收尾误差与停死表现；若想更准可把 `s_settle_rate` 调小到 20~25 试。

## 2026-10-02
完成：扫参工具与踩坑：`out/_settle.ps1` 新增 `-SettleRate`（param `:49`、符号表 `:78`、写入 `:145`）；`out/_angle_stats.py` 一次给多个 log 出表（overshoot 列写死 Δ=60，Δ≠60 时该列无意义）。发现台架把参数留在 RAM 里（`-CorrLimit -1` = 不改 ⇒ 上一轮设的 4000 会带到下一轮，s5~s9 因此全是 4000、超调 18~20°），所以每组必须显式传全部关心参数。
下一步：后续角度环扫参一律显式传 -RateKp/-SettleRate/-CorrLimit/-AngKp。

## 2026-10-02 · 固件-验证
完成：按 m09993 烧录 s_law=3 并现场展示 4 次 60° 台阶：`firmware/App/Svc/gyro_control.c:89` 初值 0->3，`make` text 105968/data 344/bss 15160，`tools/flash.ps1` 回读一致 106320 firstDiff=none；证实新固件在跑：bin 偏移 0x19EA8 = 0x03、ROM 0x08019EA8 = 03、RAM 0x200000A8 = 03。4 次台阶（out/_d1.._d4.log，每次 2.6s、112 点、`s_law = 03`）：超调 -0.03/+0.54/+0.24/-0.01°、峰值 81~92°/s、进 ±2° 1.65~2.07s、到位误差 0.16~0.53°；每次收尾自动断环（TIM8 CCR 0/0/0/0、gyro_control_enabled=0、s_enabled=0），累计转 240°（Yaw 173.38 -> 49.36）。图 out/_angle_law3.png（重画脚本 out/_plot_angle.py 已支持传 log 路径 + --out/--title，标签去掉了前导下划线）。
下一步：决定 s_corr_limit 要否从 2000 抬到 4000（台架那次是 4000：峰速 110~124°/s、整定 1.1~1.5s、尾摆略大）；锁存只在收尾角速度 <6°/s 时才触发，4 次里只有第 4 次末尾归零。

## 2026-10-02 · 固件-验证
完成：修 `out/_settle.ps1` 长期报错：`$settle` 与 param 里 `[double]$Settle` 是同一个变量（PowerShell 大小写不敏感）导致类型锁，摘要行一直抛 MetadataError（"Cannot convert @{...} to System.Double"）；改名 `$settlePt` 后摘要/整定/末 5 点行正常输出。

## 2026-10-02 · 固件-验证
完成：角度环展示：新写 out/_plot_angle.py（解析 out/_g*.log 的采样表、按 ±180° 解卷绕、matplotlib 设 Microsoft YaHei），产出 out/_angle_demo.png（60° 台阶 5 次叠加：相对偏航角/误差/陀螺角速度/corr 四栏）。统计：超调 -0.3/-1.2/-0.7/-1.1/+2.2°，峰值 110~124°/s，整定 1.12~1.43s，到位误差 0.86~1.50°（4/5），第 5 次超调 2.2° 后误差爬到 3.17°。
下一步：车放到地上时录一组 s_law=3（带到位锁存）的 20°/60° 台阶 trace，补进 out/_angle_demo.png——现有 out/*.log 全是 s_law=01/02，查不到 law 3 的记录。

## 2026-10-02
完成：标定向导标明目标套：`firmware/App/Src/app_ui.c` 新增 `draw_cal_set_name()`（先 `OLED_SetFont(kk_font_body)` 再 `OLED_GetUTF8Width()` 右对齐），`draw_trace_page()` 末尾改成 WAIT 两态写全名（x_right=122）、其余保持 `draw_cal_set_mark()`（循迹页 x28 / DONE·FAILED x116）；顺带把两处提到“设置页标定套那一行”的过期注释改成新位置。字库 `普通亚克力` 缺字 0；`make` text 105968 / data 340 / bss 15160，已烧录；`out/_scr_wz_pu.png` 实测「全黑按中键」+ 右上「普通」不压字。
未完成：用户现场确认手感
下一步：亚克力那套的截图没抓（需把两组 82B 数据整体对调才能把 active set 变 1，不冒险按 `s_set`）—— 布局按宽度算：`亚克力` 3 字 ≈35px，x=87 起，与「全黑按中键」尾 x≈68 不撞

## 2026-10-02
完成：两轮全速空转电流实测（用户 m09795 把车架起来要求测）：推 `s_req=7200` + `s_enabled=1` 后采样 `s_bat_ma`（INA226 1Hz、τ≈32s 低通）—— 静止基线 95mA；8s 170 → 38s 346 → 68s 398 → 98s 413 → 128s 420 → 158s/188s **426mA**（已收敛）⇒ **两轮全速空转稳态 426mA @ 母线 11.43V**，轮速 390~440mm/s，`s_pi[].out` 顶到 7200。已停机核验：TIM8 CCR1~4 全 0、`s_enabled`=0、`s_mmps`=0/0。
未完成：地面负载下的真实电流仍未测（那是估算式真正需要的量）
下一步：若用户愿意把车放地上（或扶着蹭地）跑一次，同样手法可测地面电流，再把 1.5A 换成实测值

## 2026-10-02
完成：循迹页右键换套改成纯预览并上板核验：①`app_ui.c` 删 `cal_set_apply()`、新增 `cal_set_preview()`（只 `trace_cal_select_set()`）；②`nvram.c` 的 `snapshot()` 改成 `trace_cal_export(&p->cal, &p->cal_b, NULL); p->cal_set = 0U;`（不再跟随 s_set）；③删设置页「标定套」项 + watcher + `s_set_applied`；④删 TRACE_CAL_PICK/wizard_pick/_wiz.ps1 的 pick 用例。
未完成：用户现场按键验证（右键换预览、下键换视图、中键进标定）
下一步：等用户现场按右键确认手感（远程无法按键）；若还嫌"弹保存成功"，可再查是否还有别的路径写 g_cfg

## 2026-10-02
完成：判据（都是实机量出来的）：①**换套不再写 Flash**——按真实的 `trace_cal_select_set()` 语义把两组数据对调 + `s_set` 置 1 后等 2.6s，`s_live`(0x20000dd8,244B) **逐字节零差异**、`s_change_ms`(0x20000ce0) 一动不动（0x1E984），而正对照写 `g_cfg.trace_speed` 时 `s_change_ms` 立刻前进 ⇒ 检测通路有效、预览确实不落盘；②复位后 `s_set`=0、实时 blk[0..1]=172/167 与记录 `cal`(0x0801F85A) 一致、`cal_set`(0x0801F8FA)=0 ⇒ 开机默认就是普通；③设置页抓图 `out/_scr_settings.png` 仍是 3 行（循迹速度 6480 / 灰度接口 自动 / 自动判线 开）+ 滚动条，删项没破导航；④预览到亚克力的抓图 `out/_scr_prev_ya.png`（标题「循迹 亚 串口 中键标定」，无提示）。
下一步：交付后请用户现场按一次右键/下键/中键

## 2026-10-02 · KK_UI 界面
完成：主页两处按用户 m09509 改完并实屏核验：①中段下移 2px（分隔线 29..30、TERRAIN 墨迹 35..42、时间墨迹 23..30），量测 title 8..71/9..19、icon 78..96/9..19、pct 103..120/10..18、runtime 85..111/23..30、separator 8..71/29..30、terrain 19..108/35..42、start 40..86/46..56、箭头 8..15/112..119 ✓，电量两行间距 2->4px ✓；②剩余时间改成保守算法后屏上从 16H39M 变成 **50min**（读 RAM：`s_bat_mv=11530mV`、`s_bat_pct=57`、`s_bat_run=50`；实测电流 ~105mA 被 1500mA 地板接住）✓。抓 `out/_scr_homeG4.png`。构建 text 106316 / data 344 / bss 15160，`tools/flash.ps1` 回读一致 106668。
下一步：如果用户给出“车跑起来的平均电流”（比如 2-3A），把 `BAT_RUN_MIN_MA 1500` 换成实测值就更准

## 2026-10-02 · KK_UI 界面
完成：主页右列版面按用户 m09335 收口：`firmware/App/Src/app_ui.c` 的 `draw_battery_block()` 常量改为 `BAT_ICON_X 78` / `BAT_TEXT_X 97` / `BAT_ROW1_Y 9` / `BAT_ROW2_Y 17`（删除 `BAT_X0`/`BAT_GAP`）。实屏量测包围盒：title 8..71/9..19、icon 78..96/9..19（含凸点 95..96）、百分比数字 103..120/10..18、分隔线 8..71/27..28、时间 79..116/21..28 ⇒ 图标与标题同行高、时间底边与分隔线底对齐 ✓；标题→图标间距 2->6px、凸点→数字 6px ✓。构建 text 106300 / data 344 / bss 15160，`tools/flash.ps1` 回读一致 ✓，抓图 `out/_scr_homeF1.png`。
下一步：等用户看一眼这版主页；要更宽/更窄就只改这几个常量

## 2026-10-02
完成：循迹页按键语义按 m09335 重做并通过实屏核验：右键=切另一套标定（立刻生效+写 Flash）、下键=柱状图<->灰度原值检验视图、中键=直接进标定（原来的「全黑按中键」→「全白按中键」→「标定完成」不变）、左键/上键=返回（标定中=取消）。改的是 `firmware/App/Src/app_ui.c`：新增 `trace_key_scan()`（读 `app_ui_raw_keys()` 原始位图，bit3 右=切套、bit1 下=换视图，20ms 消抖、只要新按下边沿）+ `s_trk_raw/s_trk_stable/s_trk_changed`；`KK_UI_CustomOnInput` 里循迹页的 DOWN 分支改成**只吞事件**、OK 的 IDLE 分支改成 `cal_set_apply()` + `trace_cal_wizard_start()`；`KK_UI_CustomOnEnter` 里进页面时复位按键机；`KK_UI_CustomOnTick` 里标定中不扫（只同步位图）。构建 text 106300 / data 344 / bss 15160，`tools/flash.ps1` 回读一致 106652 firstDiff=none。核验方式：`out/_lcd.ps1 -Page 4` 抓到柱状图页「循迹 普 串口 中键标定」正常，`-View 1` 抓到检验视图「灰度原值 普 好8/8 … 位置4.7 黑231」正常（说明两个视图都还在、`s_trace_verify` 仍驱动检验视图）。**按键本身没法远程验证**（固件无按键注入钩子，`keys_raw()` 直读 GPIO），已请用户现场按。
下一步：用户现场确认右键切套（应弹"保存成功"、右上角 普/亚 跟着变）与下键换视图；再决定要不要把没入口的「选标定套」页删掉

## 2026-10-02 · KK_UI 界面
完成：按用户 m09248 的四条改主页并烧录核验（`out/_scr_homeE1.png`）：①电池图标内条从 `+2` 改到 `+1`、高 `ICON_H-2` ⇒ 内沿正好填满（原来顶上留一整行白）；②图标加大到 **17×11**（原 16×9），与数字之间真空隙 ~10px（凸点 91..92 → 数字 103 起），不再“挨住”；③剩余时间改 `16H39M`（分钟带 M）并以 `(BAT_X0+BAT_X_END-w)/2` 居中 ⇒ 右沿从 118 收到 114，不“特别靠右”；④分隔线只垫在 HAIXINBEI 下面（x=8..71）+ 2px 粗。实测包围盒：title 8..71/9..19、icon 74..90/4..14、数字 103..120、runtime ~77..114/17..24、hline 8..71/27..28、terrain 19..108/33..40、start 40..86/46..56、箭头 8..15/112..119。构建 text 106112/data 344/bss 15152、`tools/flash.ps1` 回读一致 106492。
未完成：图标左沿 74 与标题墨迹末 71 只隔 2px（屏只有 128px 宽、标题占 64px，想再大图标就得动标题）
下一步：用户目视确认；若嫌图标与标题太挤，可把 `HAIXINBEI` 改到 x=6 或缩小该字号

## 2026-10-02 · KK_UI 界面
完成：主页（PAGE_SPLASH）版面按用户 m09221「排布好、居中、别太靠两边」重排并烧录：左右内容边距统一 8px；顶栏 y=5..24 左标题右电量两列竖直中心都≈14；分隔线改成整宽 x=8..119、y=27；TERRAIN ROBOT 居中 y=29（墨迹 33..40）、开始游戏 居中 y=44（墨迹 46..56）；箭头内收到 x=8..15 / 112..119、y=46..58，与标题/电量块共用同一条边距（原来在 5..12/115..122 贴框）。验证=`out/_lcd.ps1` 抓图（`out/_scr_homeB3.png`）+ 逐元素墨迹包围盒实测：title 8..71/9..19、hline 8..119/27、icon+pct 82..118/5..14、run 89..118/17..24、terrain 19..108/33..40、start 40..86/46..56、箭头 8..15 与 112..119（完全对称）。
下一步：用户目视确认新主页版面；若要再调，改 app_ui.c 的 BAT_X_END/ROW1_Y/ROW2_Y 与 draw_splash_page() 里的三处 y（都已参数化并写了栅格注释）

## 2026-10-02 · KK_UI 界面
完成：主页电量列改成「图标 16×9 + 2px 间隔 + 百分比」整块右对齐到 BAT_X_END=119，下面一行剩余时间同右对齐；剩余时间格式从「约45分」改成纯 ASCII 短写法（<60 分 `45min`、≥60 分 `2H05`、估不出 `--`），`BAT_RUN_MAX`(>99分) 废弃删除。像素级发现并修掉：`draw_text()` 的 y 是 12px 字框顶边、数字墨迹从 y+4 才开始 ⇒ 图标只占 5..13 时按字框对齐会让百分比「沉」4 行，改成 y=BAT_ROW1_Y-3 后墨迹 6..14 与图标同心（已写进 memory/PROJECT.md 坑清单）。

## 2026-10-02 · 固件-验证
完成：循迹全黑帧黑度修正（用户 m08812 第 2 点「全黑时两边的 1 和 8 不会变成 1」）：`firmware/App/Svc/trace_cal.c` 的 `frame_compute_body()` 在 `shift = s_shift;` 后加 `if (whiteish == 0U) shift = 0;`（混合帧路径一字未动）。验证（原地不动车、同一全黑位置）：修前 `s_fl_lvl`=157 214 249 234 222 209 172 **111**（第 8 路 <128 ⇒ 底部读作 0）；修后 = 245 245 249 246 243 246 247 251（全 ≥128 ⇒ 8 路全是 1），检验视图给「好8/8 · 位置4.5 黑249」（重心 4.5 = 均匀全黑），抓图 `out/_scr_blk_v0.png`（8 根柱顶到头 + 底部 1 1 1 1 1 1 1 1）；**决定性复现**：修后把 `s_shift(0x20001308)` 写成 −58（0xFFFFFFC6）再读，黑度仍 245 244 242 244 243 244 245 251（冻结的 ŝ 影响不到全黑帧）。构建 text 106036/344/15152，`tools/flash.ps1` 回读一致。机理与判据已写进 `docs/灰度标定与检验.md` §3.4（含跨度表：第8路跨度仅 85）。
下一步：请用户把车从全黑挪回黑线跑几帧，确认混合帧（白底+黑线）的重心/0-1 与修前一致

## 2026-10-02 · KK_UI 界面
完成：电池容量按用户 m08812 答复（2×2500mAh）把 `firmware/App/Src/app_ui.c` 的 `BAT_PACK_MAH` 从 4400 改成 5000（其余不变：×BAT_USABLE_PCT 80% 折算、向下取整到 5 分）。
下一步：无（剩余时间显示待实际跑车时看数字合不合理）

## 2026-10-02 · KK_UI 界面
完成：主页电池显示改版：`firmware/App/Src/app_ui.c` 的 `kBv[21]/kBp[21]` 换成 3S 公开放电曲线（12600mV=100% 到 9000mV=0%，5% 一档）；`battery_poll()` 同一次 1Hz 里读电流（`BAT_CUR_EMA_SHIFT 5`），按 4400mAh×80% 算剩余分钟；`draw_battery_mark()` 换成 `draw_battery_block()`（图标+百分比整块居中 y=2、剩余时间居中 y=16：`约N分`/`>99分`/`--`）；`draw_splash_page()` 删掉 TERRAIN ROBOT 与分隔线，HAIXINBEI 居中 y=29、开始游戏 y=48。验证：`make` text 106032/344/15152 无 warning、`tools/flash.ps1` 回读一致 106384 字节；复位后读 RAM `s_bat_mv=11585mV`(0x20000704) / `s_bat_ma=105mA`(0x20000700) / `s_bat_run=999`(0x200006fc) / `s_bat_pct=59`(0x200006fa)，屏上抓图 `out/_scr_batB.png` = 「电池图标+59% / >99分 / HAIXINBEI / 开始游戏+左右箭头」全部居中。
下一步：容量假设 4400mAh（2×2200 3S2P）待用户确认

## 2026-10-02 · KK_UI 界面
完成：循迹选套页手感重做（用户 m08812）：新增 `s_pick_set`/`s_pick_entry` 与 `cal_set_apply()`（已是当前套就早退，否则 `trace_cal_select_set()` + `g_cfg.trace_cal_set`，会写 Flash）；`PAGE_CUSTOM_TRACE` 输入改成 右键=切另一套并立刻生效、左键=回到进来时那一套（已在那一套上则 `trace_cal_wizard_cancel()` 退出）、中键=`cal_set_apply()` + `trace_cal_wizard_start()`；选套页文案改成「选标定套 / 套名(y22) / 右键切换 左键退出(y36) / 中键开始标定(y49)」+ 右上角单字标记，字库查过缺字 0。验证：抓图 `out/_scr_pk_a.png`(普通+「普」) 与 `out/_scr_pk_b.png`(亚克力+「亚」)；**按键绑定本身没法远程验**（固件无按键注入钩子、`keys_raw()` 直读 GPIO），只能现场按。
未完成：待用户现场按键确认手感（右键切套、左键回退/退出、中键开标定）

## 2026-10-02
完成：主页（PAGE_SPLASH, app_ui.c 的 draw_splash_page/新增 draw_battery_mark）加了电池图标 + 百分比：`battery_poll()` 每秒读一次 INA226 母线电压（等 !OLED_IsBusy），1/8 低通后查 kBv/kBp 表得百分比，变了就 KK_UI_Invalidate() 重画（在 app_ui_tick 开头调用）。构建 text 105644/data 344/bss 15144，tools/flash.ps1 回读一致 105996 字节 firstDiff=none。抓图 out/_scr_bat_final.png：台架实测 s_bat_mv=11649mV（=11.65V）→ 屏上 62% + 图标内条同比例，标题左对齐、62% 右对齐到 124，不压字。刷新链路是**实测过**的：把 s_bat_mv 强写成 10000/mV、s_bat_pct 写成 5 后 1.5s，屏上自己变成 19%（随后 EMA 爬回 62%），说明 1s 轮询 + 变化重画确实生效；重置后首帧直接采用原始读数（11648mV）= 低通没有引入偏差。
未完成：真实放电曲线没测过：kBv/kBp 是照 3S 开路电压经验表定的，带载压降（大电流下 0.5V 很正常）会让读数偏低、百分比偏保守；用户还没上车看实际电量走势。
下一步：等用户上车跑一段，看电量是不是掉得太快/太慢；要更准就采 2~3 个真实放电点回来调 kBv 表（或再决定要不要做库仑积分）。

## 2026-10-01 · 固件-验证
完成：用户 m08079「做吧」= 两套标定（普通/亚克力）两条全都落地并上板核验：① **存储**：nvram 记录 v4 里两套各自独立存 8 路黑/白/阈值/抖动 + `cal_set`（`firmware/App/Svc/nvram.c`，v3 老记录按 `rec->length` + `offsetof` 迁移）；② **切换**：`trace_cal_select_set()`（切套时把非当前这套收进 `s_other`、清 `s_base`/`s_shift_ok` 重算几何；没标过的第 1 套先复制当前套，避免 span=0 野值）；③ **入口**：循迹页空闲按**中键** → 选套页「选标定套 / 普通|亚克力 / 下键切换 中键标定」，下键挪光标、中键确认后才切套并开标定向导、上键退出（`s_valid_before` 还原 `trace_cal_valid`），另加设置菜单「标定套」项；④ **可见性**：标题行只画一个单字 `普`/`亚`（循迹页 x28、向导页 x116、检验视图 x52）。抓图逐屏核验：`out/_scr_pk0.png`/`out/_scr_pk1.png`（选套页两套）、`out/_scr_l1.png`（循迹 普 串口 中键标定）、`out/_scr_l2.png`（灰度原值 普 好8/8）、`out/_scr_l3.png`（灰度原值 亚）、`out/_scr_l5.png`（循迹 亚）、`out/_scr_w1.png`（全黑按中键 + 右上 亚）。构建 text 105172/data 344/bss 15128，`tools/flash.ps1` 回读一致 105468。
下一步：把这一轮（含"换套不落盘"的真因与修复）讲给用户；等车回来让他按 docs/灰度标定与检验.md §1 的新流程实标两套（普通路面 + 亚克力段），并看循迹页那个单字标记跟不跟手

## 2026-10-01 · 固件-验证
完成：查出并修掉一个**自动保存静默失效**的回归（我加两套标定时引入的）：`firmware/App/Svc/trace_cal.c` 的 `trace_cal_get_data()` 逐字段 memcpy、**没写 `bad[8]`→`blk[8]` 之间的填充字节**（结构体偏移 17、payload 偏移 77）；`trace_cal_export()` 中间多了个栈上局部量 `trace_cal_data_t cur`，`*set0 = cur` 把这个字节的栈垃圾带进 payload ⇒ `firmware/App/Svc/nvram.c:372` 的 `memcmp(&cur,&s_live)` 每轮都不等、`s_change_ms` 每轮被重置、保存永远不触发。证据链：`s_live(0x20000dbc)` 间隔 2s 只差偏移 77 一字节；`s_live` vs `s_saved(0x20000cc8)` 差 77/173/238(cal_set)；`s_check_ms - s_change_ms` 恒 ≤409ms（甚至为 0）。修 `memset(out,0,sizeof(*out))` 后：`s_check_ms - s_change_ms` 涨到 5660ms（后一次实测 23785ms）不再抖动，Flash 记录里 `cal_set`（`0x0801F8FA` = 0x0801F800+12+238）随 `g_cfg.trace_cal_set` 双向跟着变（00↔01）✓。顺带核对结构体真布局：`trace_cal_data_t` = valid@0/pol@1..8/bad@9..16/**blk@18**/wht@34/thr@50/jit@66（size 82），payload sizeof=244=0xF4、`cal_set`@238。
未完成：「不稳!重按」这条 1.5s 瞬态提示**没法用 halt+读显存远程取证**（时间窗 1.5s，而 J-Link 一个会话就 1.2s + settle 0.7s，默认参数下抓图时已过 1787ms；把时间戳前挪到已过 1236ms 也还是"全黑按中键"，因为写完 state 的瞬间 age 为负、随后 halt 又打断了重画）。源码与反汇编已逐条核对（`objdump -d --disassemble=trace_cal_retry_hint` 的字面量 0x20001038/0x20001035 与判据一致）⇒ 定性地讲是采集方法限制，不是固件问题；要肉眼确认就在标定时故意手抖重按。

## 2026-10-01 · KK_UI 界面
完成：修掉标题压字并给「当前标定套」定版式：kk_font_body 汉字前进宽实测 ≈**11.3px**（旧记录 8.77px 是错的），所以整词（普通/亚克力）会撞掉邻字。新增 `static void draw_cal_set_mark(int16_t x)`（只画一个字 `普`/`亚`），落点：循迹页标题「循迹」后 x28（标题行布局 = 循迹 x2..24 / 标记 x28 / 数据源 x48 / 「中键标定」x78..123）、向导页右上 x116、检验视图 x52（左边「灰度原值」到 ~47、右边 x76 是「好8/8」）。向导四个状态的标题回到原样（`draw_title("全黑按中键"/"全白按中键"/"标定完成"/"标定失败")`），删掉不再用的 `char tbuf[28]`/`const char *setc`。
下一步：若以后标题行还要加字，先按 11.3px/汉字 算宽度再定 x

## 2026-10-01 · 固件-验证
完成：`out/_wiz.ps1`（远程把向导/选套页状态画上屏再抓图）的两个根因都修了：① 顶部地址**写死**过（`$S_STATE = 0x20000ECC` 是旧构建的值），改成一律 `Need '<符号名>'` 查 nm、`$S_STATE` 用 `SymTraceCalState()`（`objdump -d --disassemble=trace_cal_state` 取它的字面量），并加了"每轮只在 `oled_transfer_index==0` 且 `oled_async_pending==0` 时 savebin、最多 15 轮"的重试；② 新增 `-Case pick` 与 `-SetN`（写 `s_pick_set`）。核验：修好后第一次真抓到向导页 `out/_scr_w1.png` = 「全黑按中键」+ 右上「亚」。教训：**同名符号有多个**（`s_state`/`s_base`/`s_last_ms`/`s_step` 各 2 个，`s_state` 一个是 nvram 的 0x20000CB8、一个是 trace_cal 的 0x20001035）。

## 2026-10-01
完成：按用户 m07972 落地两条：① 边缘外推（`firmware/App/Svc/PID_tracing.c`：`TRACE_EDGE_SAT 0.9f`、`TRACE_BARW_INIT 3.0f`、`TRACE_BARW_TAU 0.02f`；判据 = 最外那一路黑度 ≥0.9×本帧最黑 ⟺ 黑带被阵边界切掉；黑带宽度只在没被切时 EMA 学、夹 1.0~6.5 格；左切 `p=0.5+cover-s_bar_w/2` 仅当 p<pos、右切 `p=8.5-cover+s_bar_w/2` 仅当 p>pos，pos 夹 ±2..11）；② 位置 3 帧中值（`TRACE_POS_MED_N 3U`、`s_ph[3]`/`s_ph_n`，替换逐帧直出）。构建 `make -C firmware` OK，text 104308/data 336/bss 14776，`pwsh -File tools/flash.ps1` 回读一致。实测证据：`node tools/pos_sim.mjs 3.8 2` 外推区间(线心<2.5或>6.5)误差 1.286 格→0.000；台架 `out/_pos_probe.ps1`（先把 HP_LINE_KP 与 HP_BASE_SPEED_SLOW 写 0 再 s_bg_on=1 跑 4s，跑完还原）读得 data=4.9726、trace_bar_w=2.938 格、trace_edge_extrap=0、trace_edge_cnt=0、trace_pos_frames=40、帧间跳变均值 0.00473→0.00174 格（压掉 63%），折算 aim 0.047→0.017°/帧。当前状态另外用 `out/_base_read.jlink`（地址表已按新构建重写）复核：s_fl_lvl=00 00 3E C7 FB B1 42 00、dbg 11 字节 shift=0 lvl_max=238 noise=3 whiteish=5 jolt=0 state=1 ped=3、s_base 只在前两路跟原始值。
下一步：实现用户 m07972 第 3 条：两套标定（普通/亚克力）——nvram v4 存两套 8 路 blk/wht/jit + 当前套号、循迹页上/下进子页选套、运行时 trace_cal_select_set()

## 2026-10-01
完成：缺字改用重新生成字库解决（不改 UI 文案）：`node tools/font/gen_font.mjs firmware/App/Fonts/kk_font_body.c wenquanyi_9pt 12 kk_font_body "普通亚克力路段切换标定两套选择当前适晃抖踪智校准"` → 586 字形、byte_count 14536（旧 14137，备份 firmware/App/Fonts/kk_font_body.c.bak）；`node tools/font/check_charset.mjs firmware/App/Fonts/kk_font_body.c "普通亚克力路段切换标定两套选择当前适晃抖踪智校准黑白太近数据不稳"` → 32 字缺字 0。

## 2026-10-01
完成：按用户 m07888 第 2 条加了“去零偏”：trace_cal.c 的 ④ 先减 `off = noise`（夹到 lvl_max）再归一化（text 103296/data 328/bss 14728，烧录回读一致），并把零点暴露成 `trace_cal_frame_dbg.ped`（0x2000125C 第 9 字节）。验证：同一帧上 PC 重算 (lvl-3)*255/236 与板上 `s_fl_lvl` = 0 0 62 200 251 177 62 0 逐项一致；lvl_max=236/noise=3/ped=3/state=1。实测效果：重心 4.974（不去零偏）→ 4.971，差 0.003 格 ≈ 0.03°，因为 ŝ 已经把白路压到 0 附近 —— 它的价值是“标定抖动大/白路有小零偏时”的保险，不是性能提升

## 2026-10-01
完成：用户 m07689 的“自适应优化 + 参数靠标定算 + 实时性”已落地并上板核验：①`firmware/App/Svc/trace_cal.c` 新增几何缓存 `s_sp/s_rq/s_dr/s_an` + 三条阈值 `s_jolt/s_step/s_minlvl`，由新函数 `s_recalc_geom()` 在标定变化时（`sample_finish()`/`trace_cal_set_data()`/`trace_cal_init()`）重算一次；新增公共 `s_median()`；删除全部热路径除法（黑度=`(b*s_rq)>>12`、归一化用整帧倒数 `nrq`）；②`frame_compute()` 拆成计时壳 + `frame_compute_body()`，用 `DWT->CYCCNT` 记 `trace_frame_cycles`/`_max`；③`trace_cal.h` 新增 `trace_geom_t trace_geom`（jolt/step/minlvl/sp_med/jit_med/rq_shift）+ 两个周期数 extern + 算法说明⑧⑨。实测（车停线上）：`trace_geom` = 37/18/60/150/1/12；s_sp=107 150 170 160 151 146 133 92，s_rq=9761 6963 …11353，s_dr=26 37 42 40 37 36 33 24，s_an=16 18 21 20 18 18 16 16；`s_fl_state=1`、`s_fl_lvl`=0 0 108 233 254 164 71 0（行为与改前一致），dbg shift/shift_raw=-2、lvl_max=226、noise=3、whiteish=5、jolt=0；周期 2377/最坏 2940（14.0/17.3µs）。make text 103240/data 328/bss 14728，烧录回读一致 103576 B。新增/重写 `out/_base_read.jlink`（全套地址；**不可写行内 `;` 注释**，字节数 0x 前缀）

## 2026-10-01
完成：回答用户“不用自适应是不是也用这份标定值”：是，同一份 s_blk/s_wht/s_thr；实测 A/B（`w1 0x200000b8`）：写 0 → s_fl_state=0、s_fl_lvl 全 0、中点阈值读出 224 241 288 256 293 298 286 273（= (s_blk+s_wht)/2 逐路），写回 1 → s_fl_state=1 立即恢复。已写进 docs/灰度标定与检验.md 的 §3.1/§3.2/§3.3

## 2026-10-01
完成：修用户 m07626 报的「两三个传感器压在黑上只认得出一个」：`firmware/App/Svc/trace_cal.c` 的白底基线学习加了四道条件（a 本帧黑度<最黑的 1/4；b 相对"标定白值+ŝ"的黑度<跨度/8，用标定白值当锚不跟基线漂；c whiteish>0；d 硬夹子 s_base 只准在标定白值±跨度/4 内，每帧都夹所以学歪能自愈），新常量 `TRACE_BASE_DRIFT_DEN 4`/`TRACE_BASE_DRIFT_MIN 24`；`trace_cal.h` 头部补第 ⑦ 条说明。台架实测（车停在线上同一位置）：raw=270 296 290 192 234 294 326 308 → s_fl_lvl=0 0 138 255 249 141 35 0（上一版是 0 0 0 0 0 255 0 0），屏上 0/1=`0 0 1 1 1 1 0 0`、检验视图「位置4.6 黑226」，截图 out/_scr_fix1.png / out/_scr_fix3.png。旧版基线被学到黑线上（ch3 基线 198 vs 标定白 336 = 86% 跨度）的证据也读出来了。烧录：make text 102672/data 316/bss 14568，回读一致 102996 B；地址未变（s_base 0x20001128 等）。docs/灰度标定与检验.md §3 已改写（四道条件表 + 新实测表 + 承认下午那两行 126/107 不是假权重而是真实部分覆盖）

## 2026-10-01
完成：早先的设置菜单「自动判线」开关（m07343）+ nvram v3 按长度兼容老记录，已端到端验证（关掉→复位后仍是关）

## 2026-10-01
完成：用户 m07343 的两件事都做完并上板验证：①设置菜单新增「自动判线」（开/关，掉电保存）；②修「轻轻一晃就全变、等好久才恢复」——ŝ 改中位数 + 限速 16/帧 + 跳变>60 判扰动丢帧 + 改标定/切开关时基线重起头。验证（SWD 读写 + 实屏抓图）：`out/_scr_set.png` 设置页第 3 行「自动判线 开」、`out/_scr_set2.png` 改 0 后「关」且弹「保存成功」；g_cfg.trace_adapt@0x20000044 写 0 → trace_adapt_enable@0x200000ac 变 0、s_fl_state=FALLBACK(0)、复位后仍为 0，写回 1 后 s_base_ok/state=OK 恢复；单路基线跳 −200（out/_joltA.jlink）→ shift_raw=0/state=OK、s_fl_lvl 几乎不变（老的最大值算法会被拉走）；8 路一起跳 −200（out/_joltB.jlink）→ 40ms 时 shift=33/shift_raw=189/jolt=1/state=WEAK，1.2s 后 shift=137/state=OK。固件 102780 B 已烧录（回读一致）。docs/灰度标定与检验.md §3 已同步（中位数、⑤抗抖、设置菜单开关、基线重起头）。
未完成：真车带循迹+角度环联调（P5）还没做
下一步：让用户上车在亚克力段开关「自动判线」对比一次，确认「晃一下不再甩位置」的手感；若仍嫌丢帧多可把 TRACE_ADAPT_SHIFT_STEP 调大或 JOLT 调小。

## 2026-10-01
完成：单帧自适应落地并上板验证：`firmware/App/Svc/trace_cal.h`/`trace_cal.c` 加 `trace_cal_frame()`（三态 OK/WEAK/FALLBACK）+ `frame_compute()`（ŝ → 每路黑度 → WEAK 闸门 60 与 2×噪声 → 本帧归一化）+ `s_base[8]` 白底基线跟踪；`PID_tracing.c` 的 `read_tracing_data_pid()` 改连续加权重心（WEAK 丢线、无模拟量回落老数字口路径）；`app_ui.c` 的 `draw_trace_verify()` 末行显示 `位置4.5 黑240`。实测同一帧：v1 黑度 0 0 126 255 255 107 19 21 / 重心 4.618 → 加基线后 0 0 0 255 236 1 2 3 / 重心 4.515。验证方式：烧录后 `out/_base_read.jlink` 读 RAM（白路 s_base 已跟到观测值、压线两路不动、s_fl_lvl 与手算逐路一致），再 `out/_lcd.ps1 -Page 4 -View 1` 抓屏看到 `位置4.5 黑240`（out/_scr_adapt5.png）。`make` + `tools/flash.ps1` 回读一致 103260 字节；A/B 开关 `trace_adapt_enable@0x200000a8`（SWD 写 0 即回到旧算法）。
未完成：还没在赛道的亚克力段实车验证自适应（台架只是静态读数）；docs 里的自适应算法说明与实测对照表已写。
下一步：真车在亚克力段跑，对比 `trace_adapt_enable`=0/1 的循迹表现。

## 2026-10-01
完成：在实板上把 NVRAM 保存通路端到端验了（用了新工具 out/_nvtest.ps1 + out/_nvchk.py）：①现状干净：s_state=IDLE、s_live==s_saved、静置 60s 无自发保存（Flash 页哈希不变）；②Flash 记录有效：magic=31425848 "HXB1"、version=2、length=152、payload CRC 复算一致；③注入一次改动（g_cfg.trace_speed 6480→6580，地址 0x20000004）→ 3.4s 后 s_state=DONE、live==saved、Flash CRC 由 A1DFCF22 → 23AE2CC8（**真的重新擦写落盘了**）→ 回写 6480 后 CRC 又回到 A1DFCF22（逐字节还原）⇒ 保存通路健康，m03645 那个“擦 bank2 用 HAL_FLASHEx_Erase 被静默忽略”的根因确已修好。
未完成：用户报的“一直保存”在当前固件上**复现不了**（没有待存改动、不会自发循环）。按代码能解释的现象：每改一次参数的屏幕保存提示总时长 ≈ 1.5s 防抖 + 0.6s “正在保存”（NVRAM_NOTICE_MS）+ ～30ms 擦写 + 1.2s “保存成功”（NVRAM_DONE_MS）≈ **3.3s**；慢拧旋钮（每格间隔 >1.5s）会一格存一次，连着看就像“一直在存”。待用户确认看到的是哪个词、什么时机再决定改不改。
下一步：若用户想改：a) 缩短提示（NVRAM_DONE_MS 1200→600、NOTICE 600→300）b) 改成“退出编辑对话框再存”（减少擦写次数与提示次数，需在 app_ui 里判对话框）。

## 2026-10-01
完成：查清了模块「位置」「偏移量」两个字段的真实语义（依据 out/gray_sdk 厂家源码，不是猜）：**位置** 1 字节 = 低 5 位压线通道数（0 = 出线、8 = 八路全压线，厂家例程拿 >= ADC_N-4 = 4 判“路口”）、bit5 = 偏左(0)/偏右(1)、bit6 = 左出线(0)/右出线(1)；**偏移量** 16 位大端 = 横向偏差的**大小**（无符号），方向在 位置的 bit5 里；厂家 track_PID1 直接 `error = ±偏移量` 喂 P 环（P 取 0.01~0.1）。帧尾区分内容：0x02 数字量(3B) / 0x04 位置+偏移量(5B) / 0x11 模拟量(18B) / 0x15 全部(22B)。另外澄清：SDK 里 H_SETPOINT=4500 那套（Σvᵢ·(i+1)/Σvᵢ×1000）是“只用模拟量自己加权算重心”的算法，与模块的偏移量不是一回事。已按此修正 firmware/App/Svc/trace_cal.h:8-30 的注释（原来写“压线位图低 5 位 + bit5/6 出线标志”，不准确：低 5 位是压线**通道数**）。
未完成：固件目前仍只取 8 路模拟量，位置/偏移量读完就丢（trace_cal.c:198-224）。
下一步：若决定吃模块输出，读 5 字节 0x04 帧就能同时拿到 位置+偏移量。

## 2026-10-01
完成：用户就“用模块自带校准还是我们自己标定”表态：**倾向用我们自己标定的阈值**，理由是赛道有一段亚克力，灰度模拟值在那段会乱，模块内部固定阈值可能就不准。已向用户澄清两点：①我们的两点标定同样是一份**固定阈值**（黑/白各按一次算 8 路中点），不会自动按路段变；“亚克力段单独一套阈值”需要新增多套阈值表 + 切换机制，目前还没做；②亚克力会让模拟值整体平移/反射抖动，任何绝对阈值（包括模块的）都会受影响，用模块的不会更准。
未完成：待用户选方案后再改：a) 横向偏差改用归一化重心（对整体增益/偏置变化免疫，工作量最小）b) 多套阈值表按赛道位置切换 c) 亚克力段交给角度环按陀螺直行通过 d) 在线自适应阈值（滑窗 min/max）。
下一步：等用户选 a/b/c/d（我推荐 a+c）再动手。

## 2026-10-01
完成：灰度标定流程可检验化并烧录上板：按下瞬间同步采 32 帧（单点抖动 >80 整点作废、提示重按）、结束逐路给原因码（TRACE_BAD_SPAN/JITTER/NOSAMPLE）、trace_cal_quality() 出「好 n/8」；循迹页右键新增「灰度原值」检验视图（8 路原始值 + 无效路 '!' + 现 lo~hi 差 d）。验证：`make`（text 100776/data 308/bss 14472）+ `make flash`（回读一致 101092 字节、firstDiff=none）；用 out/_wiz.ps1 抓实板显存核验了四态文案——「全黑按中键」「标定失败+黑白太近」「标定失败+无模拟量」「标定完成+0/8!」；右上「未标定」+ 8 路原值 289 323 318 201 / 241 316 352 330 + 「现201~352 未标定」也抓图确认。
下一步：让用户现场按中键标一次：黑处按一下、白处按一下，看「标定完成」右侧是不是 8/8；顺便故意手抖确认标题会变「不稳!重按」（这条提示串远程没抓到，见 out/_wiz.ps1 头部注释的 4 个 SWD 陷阱）。

## 2026-10-01
完成：界面文案查缺字：用 tools/font/check_charset.mjs 查出 kk_font_body 缺 跨/小/抖 三字（实屏「标定失败 度太」为证），改写成 差/黑白太近/不稳!重按；改完把 firmware/App/Src/*.c 里所有会上屏的字面量过了一遍（58 条、248 字，缺字 0）。
下一步：以后加文案前先跑 check_charset.mjs，别等实屏发现缺字。

## 2026-10-01
完成：OLED 显存抓图链路打通：out/_lcd.ps1（先不 halt 读 stable/transfer/pending 选面，再 halt+savebin，落空就换另一面）配合 out/_lcd.py（默认 180° 旋转、中文字形）能把实屏渲染成 PNG；确认这块面板看到的是显存的 180° 旋转（out/_lcd_orient.py 四向对比里只有 rot180 正着）。另查明：halt 落在 OLED/I2C 传输中间会把提交通路卡死、之后抓到的都是旧帧，用 connect+r+go 软复位恢复。
下一步：要再抓图时直接跑 out/_lcd.ps1；抓到明显过期的帧先软复位再抓。

## 2026-10-01
完成：out/_wiz.ps1 建成：不按键、不采样、不写 nvram，用 SWD 拨 trace_cal 的 s_state/s_retry_ms/trace_cal_mode 就能把标定向导的各种提示画上屏再抓下来。头部注释记了 4 个 SWD 陷阱（别改 uwTick 清零时间：kk_ui.c:745 是有符号比较绝对时限会冻屏；软复位后首个 JLink 会话写内存静默失效；kk_ui.current.page 要独立会话写；想看“刚发生”的提示要先写时间戳再写 state）。
下一步：「不稳!重按」这条提示串的屏上取证还没成功（判据与反汇编都核对过），留给现场按键确认，不建议再花远程取证时间。

## 2026-10-01
完成：J-Link 连通性已验证：`C:\Program Files (x86)\SEGGER\JLink\JLink.exe`（**不是** Program Files，那个路径会报 "The term ... is not recognized as a name of a cmdlet"）+ `si SWD; speed 4000; device STM32G474RE; connect` → "Found Cortex-M4 r0p1, Little endian"，`VTref=3.293V`（说明 MCU 是吃 J-Link 的 3.3V 在跑）。实测读回 ram 确认固件跑的是第三轮默认：`s_law` 0x20000A64=0、`s_corr_limit` 0x20000098=0x44FA0000=2000.0、`s_param`(0x20000C00 即 [13..16])=150/0/15/1000。
未完成：**现场跑不出效果：电机电源没上**。证据：控制链正常（err 60°→PID 9000→夹到 2000、req -2000/2000）且 PWM 确实在涨（TIM1 CCR 0x40013434 = 4061→5797），但 Yaw 恒 -0.03°、编码器 0/0 mm/s，**INA226 母线 `vbus_mv`（符号 `vbus_mv.2` @ 0x20000A94，uint16 mV）= 0x0219 = 537 mV** ⇒ 电池/总开关没开。下次电机不动先查这个电压，别去怀疑控制器
下一步：要现场跑演示图：先确认电机电源，再 `pwsh -File out/_settle.ps1 -Delta ±60 -Ramp 100000` → `out/_trace.ps1 -Csv` → `python out/_plot.py`

## 2026-10-01
完成：新增 `out/_plot.py`（matplotlib 3.11.1；`python out/_plot.py -i <csv> -o <png> -t 标题`）：吃 `out/_trace.ps1 -Csv` 的 `out/_settle_trace.csv`，画 4 面板（Yaw+目标线 / err+±2° 带 / rate / corr+req），自动标出真锁存点（判定：从某点起 **ReqL 与 ReqR 两列**恒 0 且后面 ≥10 点——单列 `Req` 不是它，踩过；只看“首次过零”会把未锁存的轮次也标上）。已生成两张实例图：`out/_demo_ki0.png`（末点 0.06°、1206ms 锁存、尾段 3286ms rate/req 恒 0）、`out/_demo_ki10.png`（Ki=10.8：err 悬在 -2.08°、rate ±10~15°/s 永远抖、req ±200 来回翻、从头到尾锁不上——“Ki 必须为 0”的直观证据）。中文字形用 `Microsoft YaHei`（不设会变方框）。
未完成：交付配置的 rate 面板在运动段（300~900ms）仍有 4~5 个 **50↔140°/s 锤齿**——不是控制器造成（corr 单调下降、无变号），是静摩擦 stick-slip 本身，即文档里记的物理限制；若用户要“真平滑”就得回到“低误差助推”那条没试完的路

## 2026-10-01
完成：第三轮（用户 m05505「继续优化」）把默认角度环定型为：s_law=0（连续位置 PID 150/0/15 + damp·Gyro_Z）+ 各律共享到位锁存 + s_corr_limit=2000。已 build+flash（text 99796 / data 308 / bss 14464；make flash 回读一致 100112 字节 firstDiff=none），并**从 RAM 读回确认** s_law=0x20000a64=0、s_corr_limit=0x20000098=0x44FA0000=2000.0、s_param[13..16]=43160000/00000000/41700000/447A0000=150/0/15/1000。
未完成：末点残差 0~3° 散（P 要 1696 才顶开静摩擦，150·err 得 err>11.3°；车靠滑行进带）⇒ 生产 ramp 100000 下交替 4×±60° 实测末点 +2.87/−1.83/+2.87/−1.94°、无超调、req 归零、尾段峰峰 0.00°、无累积漂移；锁存带 2.0° 与 HP_TURN_TOLERANCE 一致，残差 ≤3° 可接受但比 2° 差
下一步：如果还要把末点残差压进 2°，唯一没试过的方向是低误差助推（复用 s_law=3 的 s_boost_* 那套脉冲+冷却），纯 Kp 需要 >850 会自激

## 2026-10-01
完成：验收与一致性全绿：交替 4×±60°（生产 Ramp 100000、固件默认值）末点 err +2.87/−1.83/+2.87/−1.94、req 归零、尾段 Yaw 峰峰 0.00°、净漂移≈0.1°；运动段 CV 39~47%、停顿 0~1、变号 0、maxΔcorr 329~479。python tools/host/check_param_table.py → n=30 crc=0x57E2（上下位一致）；python tools/host/tests/run_all.py → 102 用例 0 失败。
未完成：文档/工具同步已完成：固件接口规格.md §5.1 新增第三轮段（含为何换回 law 0、Ki=0 机理、CL 下限 ~1700、共享锁存两级判据、实测表、已知限制、复位提醒）；docs/上位机通信协议.md :206（0x0102 默认 0 + 迁移说明）；tools/host/protocol.py :341 注释与 :368 默认值 0.0；tools/host/tests/test_protocol.py :189 断言 0.0
下一步：无需继续（除非要压残差）

## 2026-10-01
完成：角度环默认律改成 s_law=3（= 原 law 2「连续速率环」+ 到位锁存），已 build+flash（99832 字节，回读一致），复位后 RAM 读回 s_law=03。锁存判据：|err| < s_settle_deg(2.0) 连续 ≥ s_settle_ms(150ms) 且 |v_f| < 2·s_ff_rate(6) ⇒ s_settled=1，此后 corr=0 彻底松手，target_angle 一变（比较 s_settled_target）即解锁。验证：out/_settle.ps1 -Delta 60 -Law 3 -Floor 0 -Settle 2.0 -Ramp 400000 → 超调 0.00°、峰值 118°/s、约 1.3s 进死区锁存，之后 4.9s 完全静止（err 恒 1.59~1.60°、rate 0.0、corr 0、req 0/0）；out/_trace.ps1 尾段晃动峰峰 0.02°、corr 突变>500 共 0 次、方向反转 4 次。交替方向 4×±60° 连跑（-129.32→-70.12→-130.29→-70.96→-132.76）每步末点 err ≤1.81°、末 5 点 req 恒 0/0，无累积漂移。python tools/host/check_param_table.py（n=30 crc=0xB1E0 一致）与 tools/host/tests/run_all.py（102 用例 0 失败）全绿。
下一步：运动段「一卡一卡」若要再动：只能从硬件侧（降低起转阈值/提高刹车权限）入手，控制侧已证明无解（见 DECISIONS）。

## 2026-10-01
完成：当日试过并全部被数据否掉的角度环替代方案（别再试）：①允许速度下限 s_v_floor=35 + 停车带 1.5° ⇒ 超调 5.00°、±3~4° 永不锁存；②下限 30 + 停车带 3.0° ⇒ 仍 ±2~3° 来回摆、corr 在 ±4000 之间砸；③减速度前馈 corr = s_kacc·Δv_ref/dt + 比例（两版）⇒ ±23°、峰值 174°/s 剧烈极限环，机理是 v_ref 被 ±s_rate_max 夹住后误差过零、差分出的前馈自己变成 ±4000 的继电器；④√ 刹车曲线（s_decel=500~600）+ 满权限反刹继电器 ⇒ 巡航段 13 次一拍突变>30°/s。s_v_floor 保留成台架旋钮但默认 0 关闭。
未完成：s_decel 现在只剩 s_law=2（对照）的 √ 曲线用；out/_settle.ps1 的摘要段在「整段都没进 ±1° 且无锁存点」时会抛 MetadataError（line 240 空数组索引），只影响摘要行、不影响采样数据。

## 2026-10-01
完成：角度环调完并烧录验证：默认 s_law=2（速率 P 80 + 起转助推 60000→3600 + 低通 0.35 + 到位死区 2.0° + 助推冷却 250ms），motion.c 的 gyro 模式把速度环斜坡放开到 100000，jy61p.c 的 Gyro_Z 改取 0x52 字节 6-7。台架实测 out/_settle.ps1（-Ramp 100000 -ErrStop 2.0 -HoldMs 1500，每次读当前 Yaw 再 +Δ，可当台阶用）：20° 过冲 1~3°、整定 0.8~1.6s、尾摆 0~1.4°；60° 过冲 0~2°、整定 1.1~1.5s、尾摆 0~0.6°（5 次里 4 次）。改造前同台架：20° 过冲 6~12°、整定 1.2~3s 且尾巴常摆。验证手段：make -j8 → text 98140/data 292/bss 14432 无新警告；make flash → OK：回读一致 firstDiff=none；python tools/host/tests/run_all.py → 102 用例 0 失败；check_param_table.py → n=30 crc=0xB1E0 一致；文档 固件接口规格.md §5.1 已写清三支律/参数/命令与传感器坑。
未完成：60° 大步长仍有约 1/5 概率尾部 ±3° 游走（助推冷却 250ms 只能压掉大部分）；台架都是原地转（base=0），真车行进中转角度未验证；out/_anglesteps.ps1 地址表仍是旧的，统一改用 out/_settle.ps1（nm 符号表驱动）
下一步：上车联调：循迹 + 角度环一起跑（P5 未做），重点看 90°/180° 掉头的实际过冲与停位、以及 motion 路径下 s_base≠0 时的表现

## 2026-09-30
完成：角度环默认增益重定并落盘四处：`host_link.c` SPECS `HP_GYRO_KP 720→150`/`HP_GYRO_KD 72→15`、`gyro_control.c` `GYRO_KP_FALLBACK 150`/`GYRO_KD_FALLBACK 15`（`GYRO_CORR_LIMIT 2500` 保留）、`app_ui.c` g_cfg `.gyro_kp_x100=15000`/`.gyro_kd_x100=1500` + `pull_pid_params()` 兜底、`app_ui_settings_loaded()` 开头加旧值一次性迁移（`CFG_GYRO_KP_OLD_X100 72000`/`CFG_GYRO_KD_OLD_X100 7200` → 15000/1500，必须在 `push_pid_params()` 之前）。构建 text 97104；`make flash` bin 97344、`OK：回读一致 97344 字节，firstDiff=none`。验证：① 烧完复位 J-Link 读 `s_param` 0x20000b80/b84/b88 = 150.0/10.8/15.0，g_cfg +28/+36 = 15000/1500，bool 字 +60 = 0x00010101（beep_key 未被冲掉）；② 台架验收 Δ=20° 过冲 6.38°/末态误差 0.64°/尾晃 0.01°/峰值 110°/s，6×15° 转累计 90° 末态误差 0.42°、各步尾晃 ≤0.54°、无振荡；③ 收尾 s_enabled=0、gyro_control_enabled=0、四路 CCR 全 0。
未完成：真车联调（循迹 + 角度环）未做。
下一步：循迹模块线到货后做真车联调（基准速度≠0 时角度环与循迹环叠加的相位裕度）。

## 2026-09-30
完成：存档迁移路径真机端到端证明：先用 J-Link 把 g_cfg 的 gyro_kp_x100/gyro_kd_x100 写成 72000/7200（0x20000020=0x11940、0x20000028=0x1C20）→ 8s 后 Flash 记录 0x0801F800 的 crc 变成 **E1B42D5F**、payload 0x0801F820=0x11940、0x0801F828=0x1C20（= 用户板上原本的旧默认值存档，crc 与事故前完全一致）→ J-Link `r`+`go` 复位 → g_cfg 读回 15000/1500、8s 后存档 crc 回到 **E046D35A** 且 payload = 15000/1500 ⇒ 迁移与自动回写都生效。
下一步：无（已闭环）。

## 2026-09-30
完成：① `python tools/host/check_param_table.py`：固件 host_link.c SPECS n=30 crc=0xB1E0 与上位机 protocol.PARAMS 一致（OK）。② `python tools/host/tests/run_all.py`：6 个文件 102 用例全过。③ 文档同步：`docs/上位机通信协议.md`、`tools/host/protocol.py`、`tools/host/tests/test_protocol.py`、`固件接口规格.md`、`docs/旧码到新板移植对照.md` 都把角度环从「旧值 ×7.2」改成实测值 150/10.8/15 并写明理由。④ `tools/flash.ps1` 的 `FLASH->SR` 检查用真机输出验证：J-Link 输出行 `40022010 = 00000000`，脚本正则 `40022010\s*=\s*([0-9A-Fa-f]{8})` 匹配成功、SR=0、掩码 0xC37A 未命中（⇒ 检查确实在跑，静默 = 通过）。
下一步：无。

## 2026-09-30 · 固件-验证
完成：按用户 m04020 做「原地角度环」实测（不改固件、不烧录，只用 J-Link 在 RAM 里改 s_enabled(0x20000de3)/s_req(0x20000e24)/gyro_control_enabled(0x200009e4)/target_angle(0x200009d8)/s_param[13..15](0x20000b80..b88)，base_speed 保持 0 ⇒ 只会差速原地转）。共 7 组阶跃，用自建台架 out/_angletest.ps1（J-Link 脚本采样 uwTick/Yaw(0x20000bc8)/TIM8 CCR(0x40013434,4)，约 26.6ms 一点）。结果：①100/0/10 +20° 过冲 6.96°、稳态误差 4.75°、尾部晃动 0.18°、118°/s；②100/10.8/10 +20° 过冲 1.65°、误差 1.02°、晃 0.03°、63°/s（最好）；③200/10.8/20 +20° 过冲 11.7°、误差 0.12°、晃 0.29°、132°/s；④200/10.8/20 −20° 过冲 7.0°、误差 0.09°、晃 0.02°（正反向对称 ✓）；⑤720/10.8/72 +3° 过冲 0.14°、误差 0.18°、晃 0.02°、峰值仅 19°/s ⇒ 小信号（循迹工况）非常稳、无极限环；⑥720/10.8/72 +6°、⑦同增益 +10° 均失控：冲到 +17.7°/+42° 后反向 −15°/−64°，峰值 6773°/s 和 1512°/s，车原地甩了好几圈。标定了静摩擦死区模型：速度目标 corr 的稳态输出 = 0.662×corr+2327（实测 FF(2000)=3651），与起转阈值 ~2700 相交 ⇒ 静止时角度死区 ≈ 563/Kp 度（Kp=100→5.6°，实测 4.75° 吻合）；开环符号标定（s_req=±800）确认 Kp>0 是负反馈方向。诊断：大误差失控不是相位裕度问题（PROJECT.md「下一步 4」的疑问）——角度环输出直接是速度目标、只夹在 ±7200 全量程，Kp=720 时误差 ≥6° 就命令接近满速的轮速，冲过头后刹不住。收尾已全部还原并验证：s_param 回 720/10.8/72、s_enabled=0、gyro_control_enabled=0、四个 CCR 全 0、g_cfg.beep_key 写回 1、NVRAM 记录 crc 回到 E1B42D5F（与事故前那条记录逐字节相同）✓，车停在 Yaw≈99.9°。
未完成：循迹模块的线还没到，真车循迹/A 流程联调（P5）仍未做。
下一步：决定是否把角度环输出限幅从 ±7200 收到 ±2000~2500（或把 Kp 降到 100~200）以消除大误差下的极限环，然后原地复测一遍。

## 2026-09-30
完成：按用户 m03936 复查 tools/flash.ps1 的「下载不擦设置」：逻辑通读（默认路径 `-c port=JLINK -d $Bin 0x08000000 -v` 不显式擦除，靠 CubeProgrammer 按镜像范围擦扇区 0..47；两道守卫 bin 末端与末扇区；J-Link 兜底 loadbin + savebin 回读 + verify_readback.ps1 逐字节比对）＋真机验证：`make flash` 前后 0x0801F800 恒为 `31425848 00980002 E1B42D5F 00001950`、0x08018000（bank2 内 app 之外）恒为 FFFFFFFF ⇒ 「不擦设置」成立。发现并已加固 flash.ps1：① 文件头补上「内容相同时 CubeProgrammer/J-Link 会打印成功却什么都不写（Skipped. Contents already match），成功信息不算数」；② J-Link 脚本里加 `mem32 0x40022010, 1`，烧完读 FLASH->SR，粘着错误标志（掩码 0xC37A）就警告；③ 擦除范围注释改成 2 bank × 32 页 × 2KB + 地址线性编号（OPTR 的 DBANK 位不可信）。改完已用 PowerShell 解析器验过语法 OK，**尚未真机跑过**（板子当时已失联）。
未完成：板子 SWD 失联（`Cannot connect to target`，VTref=3.290V 正常）：我在审计时用 J-Link 直接写 FLASH_CR/ACR 做擦除实验，之后擦写对调试器和应用全部失败（FLASH->SR=0xA0=PGAERR|PGSERR，CR=0xC0000872 有残留 PER|PNB=14|BKER），随后连连接都失败。整片擦 `-e all` 从未执行成功（在 connect 阶段就失败）⇒ 芯片里的 app 与设置记录应当还在。板子用独立电源（USB 里只有 J-Link，没有板子自己的 USB）⇒ 只能让用户断电重上电。
下一步：用户断电重上电后：J-Link `connect`（必要时 CubeProgrammer `mode=UR reset=HWrst`）→ 需要时 `-e all` 整片擦 → `make flash` → 读 0x0801F800 确认记录在/被正确重写 → 用 RAM 改 `g_cfg.beep_key`（`stm32-dev.py write --elf firmware\build-app\haixin-app.elf 'g_cfg.beep_key=1'`，工具回显会报 CP1252 编码错但赋值生效，以读回为准）等 4s 看页面 crc 是否变，验证自动保存真的能写 ✓。

## 2026-09-30
完成：修好用户 m03645 报的「自动保存一直保存、过一会儿弹一个提示」：根因是写 Flash 的**页寻址**错。本板实测 Flash 是 2 bank × 32 页 × 2KB，NVRAM 页 0x0801F800 = bank2 页 31，必须 BKER=1 + PNB=31 才能擦；而 HAL_FLASHEx_Erase() 按 FLASH_OPTR 的 DBANK 位（本机读 0，与硬件行为不符）清掉 BKER 再用 PNB=63 擦 ⇒ PNB 越界被硬件**静默忽略**（SR 不置错、不置 EOP、页不变）⇒ 页永远擦不掉 → 在没擦的地方编程得 PROGERR → write_record() 返回 0 → nvram_service() 每 5s 重试并弹「正在保存」（正是用户看到的现象）。改法：firmware/App/Svc/nvram.c 新增本地 nvram_page_erase()（清 PG|PER|PNB|BKER|MER1|MER2 → SET BKER → PNB=31 → SET PER → SET STRT → 轮询 FLASH_SR_BSY 50ms 超时 → 清 PER|PNB|BKER；擦除期间关数据 cache、擦完复位再开，防回读命中旧 cache），write_record() 改调它并删掉临时 g_nvdbg 诊断；nvram_service() 失败退避改成 5s→10s→20s→40s→60s 封顶。验证（全真跑）：make -j8 过（text 97032/data 232/bss 14416，只剩已知 ld RWX 警告）；make flash 回读一致 97272 字节 firstDiff=none；**端到端实测**：J-Link 写 RAM 的 g_cfg.beep_key=1 → 4s 后 0x0801F800 = 31425848 00980002 E1B42D5F 00001950（应用自己写出的记录，crc 与被我擦掉的那条老记录**逐字节相同** ⇒ 用户设置已复原）；再写回 0 → 页变成新 crc 14D12C9D（证明「擦掉已写过的页 + 重写」也行）；J-Link 硬复位后 s_loaded=1、g_cfg.beep_key=true、trace_speed=6480、use_gyro/beep_marker=true、s_state=0、页未被无谓改写 ⇒ 记录开机加载并生效、不再反复保存。python tools/host/tests/run_all.py 102 项 0 失败。
未完成：① 100 档实测只到 80 的 A/B 决策仍等用户给「架起来还是地上测 + 满油门电源电压」（m03397）；② 数字俯仰符号是否跟方框翻（m02505 未答）；③ 真车联调循迹/A 流程未做。另：这次实验把 NVRAM 页擦过一次，旧记录虽被逐字节复原，仍应在回复里向用户说明。
下一步：把结论交给用户复测：改任一菜单项后 1.5s 应只弹一次「正在保存」+ 一次「保存成功」，关机再开设置还在（若再看到反复弹提示就是另一处失败，读 s_fail_until_ms 是否在推进）

## 2026-09-30
完成：按用户 m03397 第 2 点改掉「烧录会清设置」：`tools/flash.ps1` 默认不再整片擦（去掉 `-e all`，靠 CubeProgrammer 按镜像范围擦扇区 0..47），新增 `-MassErase` 开关与 NVRAM 页越界断言；`firmware/Makefile` 的 flash 规则优先调 `pwsh`（`PSFLASH := $(if $(shell where pwsh),pwsh,powershell)`）。验证链：先用 `stm32-dev.py write` 把 `g_cfg.trace_speed` 植成 4000（≠默认 6480，页记录 `48 58 42 31 02 00 98 00 30 04 15 C7 A0 0F 00 00`、nonFF=164），改时间戳重编译出新镜像（md5 FB5AA6091101F50A47623286C5DF07D6 / 97548 B）后 `make flash`：`Download verified successfully` + `OK：回读一致 97548 字节，firstDiff=none`，烧后 NVRAM 页与植入时逐字节不同=0，且 app 里 `g_cfg.trace_speed` 读回 4000。期间误用 J-Link `erase 0x0801F800 0x0801F800` 把整片擦了（0x08000000 读回全 FF、板死），已 `make flash` 重烧恢复：头 64 字节 vs bin 0 差异、NVRAM 页 nonFF=0、uwTick 5985→12913 在跑、trace_speed=6480 默认。另外修回 `firmware/Makefile` 的编码（一次 GBK 往返把它写成混合编码，9 行 invalid UTF-8）→ 现在 0 行无效、`make -n flash/erase` 展开正确。
未完成：「100 档只到 80」的解释已发（不是换算错，是输出顶在 ±7200 的物理上限），用户还没回报测试条件（地上还是架起来）与满油门电源电压
下一步：等用户上车看「100 档实测 80」是不是物理（架起来再测一次满油门）；若要「100 档显示 100」，按空载实测值改 SPEED_FULL_COUNTS + 显示改百分比

## 2026-09-30
完成：「电机测试」页按用户 m03300/m03156 改成：①滑条=**闭环速度环目标**（`motor_test_apply()` = `SpeedControl_Stop()` 当两路 0，否则 `SpeedControl_Enable(1U)+SetTarget`；进页面 `KK_UI_CustomOnEnter()` 先无条件 Stop + `SetRamp(20000.0f)`）；②标题行「返回/清零」用左/右切换。验证：`make -j8` text 97312 / data 228 / bss 14416（`out/build_cl1.log`，只剩已知 ld RWX 警告）；`make flash` `OK：回读一致 97548 字节，firstDiff=none`（`out/flash_cl1.log`）；`python tools/host/tests/run_all.py` 102 项 0 失败；J-Link 快照 `uwTick=84914`（板在跑）、`s_enabled=0`、`s_req={0,0}`、`s_motor_level={0,0}`、`s_motor_sel=0`、`s_ramp=20000`（刚烧完未进页 = 环处停止态，符合预期）。`固件接口规格.md` §3.2 电机页小节已整段改成闭环口径，`memory/PROJECT.md` 补了「速度环使能时 Load() 无效」与自定义页返回约定例外两条。
未完成：闭环行为本身要用户上车验收（我按不了按键）：左/右各划到 40~60 看两个数字是否贴合；档位 <21（目标 <1500）是静摩擦粘滑区，追不上属物理下限
下一步：等用户试机回复；若他说低档追不上或反向，先查 `按「清零」是否真停`（应走 Stop）

## 2026-09-30
完成：电机测试页按用户 m02868/m02897 改成「双极性速度滑条 + 清零」（`firmware/App/Src/app_ui.c`）：速度档 `s_motor_level[2]`(int8_t, -100..+100, ±100 = ±MOTOR_PWM_MAX 7200)、四个选中项 左/右/清零/返回（左上「返回」、右上「清零」）、上/下换项、左/右拖滑条（短按 1 格、长按 600ms 起连发 120ms、步进 1→2→5→10→20）、中键在清零上两路归零、在返回上退出，退出不停电机；本页自己读五向键原始位图（`app_ui_raw_keys()` + `motor_key_scan()`，20ms 消抖）并显式丢弃 kk_ui 的 UP/DOWN 事件。字模 `kk_font_body` 重生成到 572 字形 / 14136 B（含「清零」，脚本 `out/_extract_ui_chars.mjs` 扫出 416 字，`check_charset` 缺字 0）。验证：`make -j8` 过（text 97248/data 228/bss 14416，app_ui.c 无警告，只剩已知 ld RWX 警告）；`python tools/host/tests/run_all.py` 102 项 0 失败；`make flash` 回读一致 97484 字节 firstDiff=none；J-Link 快照 uwTick=32650 板在跑、`s_motor_sel=0`/`s_motor_level={0}`/`s_motor_show={0,0}`/`s_mkey_stable=0`。
未完成：用户上电验收滑条手感（长按加速、清零、返回）；`固件接口规格.md` §3.2 电机页小节已同步改完。
下一步：等用户回报真机手感，再决定要不要调 MOTOR_KEY_FIRST_MS/REPEAT_MS 或档位步进表。

## 2026-09-30
完成：旧码增益类参数按 7.2× 重标并落盘（m02112 同步的最后一类数值；用户没答「要不要 ×7.2」，我按证据自行定案）：固件 `host_link.c` SPECS（HP_LINE_KP 2700/HP_LINE_KI 10.8/HP_LINE_KD 360/HP_LINE_ILIMIT 50 不变、HP_BASE_SPEED 6480、HP_GYRO_KP 720/HP_GYRO_KI 10.8/HP_GYRO_KD 72/HP_GYRO_ILIMIT 1000 不变）、`app_ui.c` 的 g_cfg 默认值（trace_speed 6480、line_kp_x100 270000、line_ki_x100 1080、line_kd_x100 36000、gyro_kp_x100 72000、gyro_ki_x100 1080、gyro_kd_x100 7200）+ `pull_pid_params()` 兜底值、`pid.c:15-17` 与 `gyro_control.c` 的 *_FALLBACK、`motion.c:13/20` s_base 6480、`PID_tracing.c:236` 兜底 6480；上位机镜像 `tools/host/protocol.py`（line_kp 2700/line_ki 10.8/line_kd 360/base_speed 6480/gyro_kp 720/gyro_ki 10.8/gyro_kd 72）与 `simulator.py` 参考值（375→2700、ki/10.8）、`tests/test_protocol.py` 断言、`tests/test_link.py:65`。判据：这两个环输出的是直接加在左右轮上的 PWM 偏置（`PID_tracing.c` 的 l = base - pid_output; r = base + pid_output），旧板满量程 1000 → 本板 7200，照抄旧值等效只有 1/7.2 力度；积分限幅不折算（量纲是误差本身：灰度重心/度）；基准速度必须到 6480，否则 base-output 掉到静摩擦死区（~2500~2900）以下把内轮堵死；HP_BASE_SPEED_SLOW 故意保持 2500（旧 200×7.2=1440 仍在死区下）。验证：`make -j8` 过（text 93656/data 228/bss 14368，只剩已知 ld RWX 警告）；`python tools/host/tests/run_all.py` → **102 项 0 失败**（新增 1 项固件/上位机参数表一致性用例）；`make flash` → 回读一致 93892 字节 firstDiff=none；J-Link 快照（`uwTick=7947` 板在跑）`s_param = {2700, 10.8, 360, 50, 6480, 2500, 6480, 20, 2, 0, 3000, 0.662, 2327, 720, 10.8, 72, 1000, ...}`、`g_cfg = {trace_speed 6480, line_kp_x100 270000, line_ki_x100 1080, line_kd_x100 36000, line_ilimit 50, gyro_kp_x100 72000, gyro_ki_x100 1080, gyro_kd_x100 7200, gyro_ilimit 1000}`，与上位机表逐项一致。文档：`固件接口规格.md` §5.1「仍未同步」段改写成「已按 7.2× 重标」+ 自动化核对入口；`docs/旧码到新板移植对照.md` §3.2/§3.3/§4-1 改成已定案（并作废旧文档里 line_ilimit 50→360、gyro_ilimit 1000→7200 的错误换算）；`docs/上位机通信协议.md` §6 参数表默认值全部更新；`memory/PROJECT.md` 坑里那条也改了。
未完成：真车联调未做（P5 仍是 ⛔）；`HP_BASE_SPEED_SLOW` 2500 与旧板 200 的等效关系只是「都在死区附近」，需要实车确认慢速档够不够用。
下一步：让用户上真车跑循迹/A 流程，按手感在菜单或上位机在线微调（默认值已按旧板刻度等比放大，但死区与摩擦这台车和新车不同）；若发现车抖得厉害，先降 line_kd（360 = 旧 50×7.2，D 项在 5ms 拍上最敏感）。

## 2026-09-30
完成：陀螺仪页方框轴映射从 4 档扩成 8 档（用户 m02505 说 M1..M4 列不全所有情况），默认档改成他实测的组合：firmware/App/Src/app_ui.c:105 `s_gyro_axis_mode = 2U`（M3 = r=-SP、p=+SR），新增 `gyro_view_angles()`（app_ui.c:958）按位映射，draw_gyro_page() 改成「数字走 gyro_angles()、方框走 gyro_view_angles()」（app_ui.c:986-987），中键 `& 7U` 循环（app_ui.c:1322），setter 与 app_ui.h:120 注释、nvram.c 字段注释与 :115/:149 掩码都改成 0..7。验证：make -j8 过（text 93624/data 228/bss 14368，唯一警告是已知的 ld RWX）；make flash 回读一致 93860 字节 firstDiff=none；J-Link 单次快照 uwTick=18501（板在跑）、s_gyro_axis_mode=2、g_live{gyro_ok=1, yaw_x10=-873, pitch_x10=56, roll_x10=65}；用 python 枚举 8 档确认两轴符号组合 8 种互不相同且全覆盖；python tools/host/tests/run_all.py 101 项 0 失败；文档：固件接口规格.md §3.5 补了 8 档全表 + 默认档 + 「数字不随档位变」，docs/旧码到新板移植对照.md §4 第 3 条改写。
未完成：等用户上电验收：陀螺仪页默认显示 M3，低头/左右倾斜看方框方向对不对；若不对，中键循环 M1..M8 报出正确档号；另外问他右侧「俯仰」数字符号要不要跟着方框翻（现在两者相反）
下一步：用户报正确档号后，把 s_gyro_axis_mode 的默认值改成那一档并重新烧录

## 2026-09-30
完成：把旧码的比赛任务流程同步进新固件并让它真的能跑：① renwu.c 45 处旧板刻度数值（Load/Toggle_Gyro_Control，900/920/935/-700 等）包成 RW(v)=v*HP_TASK_SPEED/900（新增参数 0x0304 task_speed 默认 6480）并新增 renwu_stop()；② PID_tracing.c 旧接口 motor_tracing_pid/_0/_0_task 写死的 1000/200 改成读 HP_BASE_SPEED 3000 / HP_BASE_SPEED_SLOW 2500；③ 修三处 htim2/htim4 别名缺口：PID_Tracing_Timer_Start/Stop 改软件开关 + PID_TRACE_BG_DIV 20 分频（旧板 TIM2 10Hz）、Gyro_Timer_Start/Stop 改空实现（原会停 TIM7 舵机缓动）、Motion_Task 的 MOTION_IDLE 分支补跑角度环 + 后台循迹环；④ main_app.c 主循环加任务闸门（renwu_flag != 0 时先 Motion_Stop/GyroControl_Stop/SpeedControl_Stop 再每圈 renwu()，归零时 renwu_stop()）。验证：make -j8 过（text 93596 / data 228 / bss 14368，只剩已知 RWX 警告）、make flash 回读一致 93832 字节 firstDiff=none、J-Link 读到 uwTick 8581（板在跑）renwu_flag 0、arm-none-eabi-nm 里 PID_Tracing_BackgroundTask@0x08003880 / renwu_stop@0x08005008 / task_pwm@0x08004f5c 都在。另建 docs/旧码到新板移植对照.md（电机/编码器/舵机动作表/按键/蜂鸣器/RGB/PID/循迹/角度环/任务流程/界面/主循环时基逐模块对照）供后续查证。
未完成：① 增益类参数（line_kp/ki/kd/ilimit、gyro_*）是否按 7.2× 重标还没定，等用户确认；② 陀螺仪图案与实际不符（m02128）等用户报正确档号 M1~M4；③ 任务流程未上真车联调；④ 速度环低速稳态误差 R@1250 / L@1000
下一步：用户确认增益类后按 7.2× 重标（line_kp 375→2700 等），或先跑上位机 CMD_START_A 让 A 流程上真车走一遍

## 2026-09-30
完成：旧码(Haixin/)的PWM刻度同步已完成并全程验证：① firmware/App/Svc/renwu.c 的 45 处 Load(...)/Toggle_Gyro_Control(1,...) 数值全部包成 RW(v)=v*HP_TASK_SPEED/900（新参数 0x0304 默认 6480=旧板900=90%占空比），另新增 void renwu_stop()（关角度环+Load(0,0)）；② firmware/App/Svc/PID_tracing.c 的 motor_tracing_pid/_0/_0_task 从硬编码 1000/200 改成读 HP_BASE_SPEED(默认3000)/HP_BASE_SPEED_SLOW(默认2500)；③ 关键发现 renwu() 以前全工程没有调用点（renwu.c 955 行是死代码），现在接在 Core/Src/main_app.c 主循环：renwu_flag!=0 时先 Motion_Stop+GyroControl_Stop+SpeedControl_Stop 再每圈调 renwu()，归零时 renwu_stop()；④ 参数表 0x0301 1000→3000、0x0302 200→2500、新增 0x0304=6480，上位机 tools/host/protocol.py 与 docs/上位机通信协议.md 同步。验证：make -j8 BUILD OK（text 93512/data 228/bss 14368，+7.4KB 因为任务流程从死代码变被引用）；out/_param_crc_check.py → fw/host 各 30 项 crc=0x0B56 MATCH；python tools/host/tests/run_all.py → 101 项 0 失败（test_ui_smoke.py 偶发 flaky，单独跑通过）；make flash → OK：回读一致 93748 字节 firstDiff=none；J-Link 快照 uwTick=9908（在跑）、renwu_flag=0、s_param={375,1.5,50,50,3000,2500,6480,20,2,0,3000,0.662,2327,...} 索引 4/5/6 正是 3000/2500/6480。文档：固件接口规格.md 新增 §5.1（根因/对照表/定时器时基核对/7.2× 重标清单）。
未完成：① 增益类参数仍是旧板 1000 刻度标定值（line_kp/ki/kd/ilimit、gyro_kp/ki/kd/ilimit），在新板等效增益小 7.2 倍，等用户确认是否重标；② 陀螺仪图案与实际不符（用户 m02128）仍等用户给出正确档号（M1..M4 只影响方框、四档共用 yaw）；③ 整机联调（循迹/任务 A/B/C）未做。
下一步：用户确认增益类后按 §5.1 的 7.2× 清单重标（或先跑一次 CMD_START_A 看 6480 是否合适）

## 2026-09-30
完成：查清「之前那个代码」（Haixin/ = STM32F103VET6 HAL+FreeRTOS 旧板工程）里 PWM 数值的真实含义：旧板 TIM8 是 Prescaler=71/Period=1000-1（Haixin/Core/Src/tim.c:203-205），Load() 把 abs(moto) 直接写进 CCR（Haixin/Hardware/motor.c:35,46），所以满量程只有 1000，motor.c:3 的 #define PWM_MAX 7200 根本够不着 —— 旧码意思全是占空比千分比：循迹设定速度 1000（Haixin/Hardware/PID_tracing.c:45）=满油门，慢速 200=20%，任务流程那串 Load(900,900)/Load(920,900)（Haixin/Hardware/renwu.c:82,109,124,…）=90% 占空比，原地转 ±500~700=50~70%。新板满量程是 7200（firmware/App/Bsp/bsp_board.h:28 + bsp_board.c:176 的 htim8.Init.Period = MOTOR_PWM_MAX-1U，PSC=0@170MHz→23.6kHz），换算系数 7.2。验证方式：直接读两边源码 + 与本车实测死区口径（固件接口规格.md:122-124，起步死区 2500~2900 = 35~40%）对照。
未完成：firmware/App/Svc/renwu.c 的任务速度数值仍未重标（当前按现刻度跑不动车）。
下一步：等用户拍板：firmware/App/Svc/renwu.c 里那串照抄旧板的 900/920/-700 在新刻度只有 12.5% 占空比（低于死区，任务流程推不动车），是把它按 7.2 倍重标，还是改成读「循迹速度」（HP_BASE_SPEED / g_cfg.trace_speed）参数。

## 2026-09-30
完成：电机测试页（PAGE_CUSTOM_MOTOR）按用户 m01975「界面有点丑」重画成两行卡片式仪表盘，见 firmware/App/Src/app_ui.c 的 draw_motor_page()：标题 y=1 + OLED_DrawHLine(0,14,128U) 分隔线（与陀螺仪页同款）；下面两行卡片 y0 = 16 / 39、行高 23，每行 = 履带图标（dir!=0 用 OLED_DrawRBox 实心表示在转、停用 DrawRFrame 空心，x=5,y0+6,18x10,r=4）+ 左/右（x=28,y0+5）+ 停/正/反（x=42,y0+5）+ 速度条（框 OLED_DrawRFrame(58+s_dx,y0+7,42,9,2)，填充 OLED_DrawRBox(59+s_dx,y0+8,mag*40/100,7,2)）+ 读数右对齐 126-bw（y0+5）；选中行用 XOR 空心圆角框 OLED_DrawRFrame(1+s_dx,y0-1,126,23,5)。同时把 MOTOR_TEST_PWM 4000 改成 6000（实测 4000 只有 2083/1900 计数每秒 = 满速 42%，读数就是 40；6000 约 3650 计数每秒 → 读数约 73），并在 KK_UI_CustomOnInput() 的模块测试页与电机页两处 OK 分支前加了 `if (event.action == KK_UI_INPUT_OK && event.source == KK_UI_INPUT_REPEAT) return;`（否则按住中键会把方向档 停→正→反→停 一路刷过去）。验证：make -j8 过（text 86144 / data 228 / bss 14352，只剩已知 RWX 链接警告）；make flash 回读一致 86380 字节 firstDiff=none；J-Link 单次读 uwTick=13123（CPU 停在 HAL_Delay 里，板子在跑）、s_motor_sel=0 / s_motor_dir={0,0} / s_motor_show={0,0}；python tools/host/tests/run_all.py 101 项全过；布局自查用 PIL 按同一套坐标画 128x64 示意图（out/motor_page_mock.py → out/motor_page_mock.png，逐元素核对无压字、无出界）。
未完成：用户侧肉眼验收新版面与读数刻度。
下一步：让用户上电走一遍：进 设置→模块测试→电机，看两行卡片版式、左/右按中键切 停/正/反、读数是否约 73（新占空比）、按住中键不再连刷、退出后电机继续转。

## 2026-09-30
完成：修好「模块测试页按中键进不去电机测试页」：根因是 kk_ui 的导航策略在 firmware/Middlewares/kk_ui/src/kk_ui_nav.c:104-109 直接 `return KK_UI_NOT_ALLOWED` 拦掉「自定义页→自定义页」压栈，而 firmware/App/Src/app_ui.c 的中键处理写的是 `(void)KK_UI_NavigateTo(PAGE_CUSTOM_MOTOR, HAL_GetTick())` —— 返回值被丢掉，所以按键完全没有反应（屏不换、电机也不转），比报错更难查。改法：删掉那 5 行守卫并写明理由（应用确有两级自定义流程；框架已有 leave_custom 回调 + 类型无关弹栈）。验证：make -j8 过（text 86016 / data 228 / bss 14352，只剩已知 RWX 链接警告）、make flash 回读一致 86252 字节 firstDiff=none、J-Link 单次快照 uwTick=29107（在跑）且 kk_ui.transition.active=0 / blocked_keys=0 / keys[].stable=0 且 armed=1（无卡键）/ stack_count=0 / error.code=KK_UI_OK / current.page=10（PAGE_SPLASH，刚烧完重启）。
未完成：用户上电复测：设置→模块测试→选「电机」按中键应能进入电机测试页（并确认页内左/右 正/反/返回 与编码器读数）。
下一步：若在模块测试页按键仍无反应，用 J-Link 快照 kk_ui.keys[].pressed_at 与 kk_ui.current.page 区分「按键没进来」还是「页面状态卡住」——kk_ui 是全局符号（0x200010c8，size 0x1a4），可直接读。

## 2026-09-30
完成：重新生成 12px 字模修好「屏上汉字静默不画」（用户 m01831 报：菜单里前馈斜率/前馈摩擦是黑的）。做法：先 node -e 扫 firmware/{App/Src,App/Inc,App/Svc/trace_cal.c,App/Src/oled_diag.c,Middlewares/kk_ui/{src,include}} 里所有 "..." 字面量的汉字得到 267 字（存 out/_ui_chars.txt），再 `node tools/font/gen_font.mjs firmware/App/Fonts/kk_font_body.c wenquanyi_9pt 12 kk_font_body $ui` → 459 字形 / 11015 B（旧 346 / 7917）。验证：check_charset 对那 267 字**缺字=0**、对 '前馈斜率前馈摩擦' 缺字=0；`make -j8` text 86036 / data 228 / bss 14352（只剩已知 RWX 警告）；`make flash` → 回读一致 86272 字节 firstDiff=none；J-Link 快照 uwTick=14927（板子在跑）、k_app.route_count=16 / custom_page_count=8 / fonts.body_font=kk_font_body。同时把 tools/font/gen_font.mjs 改成自动保留旧文件的 #include 行。另：查清并写进 固件接口规格.md §3.1 —— ilim 3000 限的是积分累加器（单位「计数/5ms×拍」），经 ki=2 折算最多贡献 ±6000 PWM，既不是编码器量程也不是 7200 输出限幅。
未完成：make flash 是 mass erase，这次又把 NVRAM 里的菜单设置和黑白阈值标定清回默认了
下一步：让用户上电进「PID→速度环」看前馈斜率/前馈摩擦两项是否显示出来

## 2026-09-30
完成：模块测试页的「电机」项改成进独立页 `PAGE_CUSTOM_MOTOR`（`firmware/App/Inc/app_ui.h`：16 / `CUSTOM_MOTOR` 7 / `PAGE_COUNT` 16 / `CUSTOM_COUNT` 8；`firmware/App/Src/app_ui.c` 的 `k_routes`、`KK_UI_CustomOnEnter/OnLeave/OnInput/OnTick/OnDraw` 各有分支，新增 `draw_motor_page()`：车体+两条履带+选中侧 XOR 圆角框+右上角「返回」+底行左/右读数）。左/右各自用中键在 停→正→反→停 循环（`MOTOR_TEST_PWM 4000`），**离开页面不停电机**（删掉 `KK_UI_CustomOnLeave()` 里原来那句无条件 `Load(0, 0)`），方向存在 `s_motor_dir[2]` 跨页保留，进页面若速度环使能先 `SpeedControl_Stop()` 再 `motor_test_apply()`。读数口径按用户 m01700 改成「最近 20ms 窗口的计数之和」：新增 `int SpeedControl_WindowCounts(int wheel)`（`firmware/App/Svc/speed_control.c:312` 返回 `s_win_sum[wheel & 1]`；该求和在 `SpeedControl_Task()` 里位于 `if (!s_enabled) return;` **之前**，未使能也有值），满速 ≈100（`SPEED_FULL_COUNTS 25.0f × SPEED_AVG_TICKS 4` = 100），倒转为负；里程表 odometer 方案加过又全删。模块测试列表 `TEST_N 4→3` = 蜂鸣器/电机/返回，电机行尾显示 运行/停。验证：`make -j8` 过（text 82940 / data 228 / bss 14352，只剩已知 RWX 警告）；`make flash` 回读一致 83176 字节 firstDiff=none；J-Link 单次快照读到 `k_app.route_count=16`、`k_app.custom_page_count=8`、`k_app.int_binding_count=16`、`s_motor_sel=0`、`s_motor_dir={0,0}`、`s_motor_show={0,0}`，两次读 `uwTick` 12311→19388 证明板子仍在跑；`python tools/host/tests/run_all.py` 101 项全过；字模核对 `'左轮右轮电机测试返回停正反运行计数'` 只缺「轮」→ 屏上文案只用「左」「右」。`固件接口规格.md` 的测速口径小节已补这一条。
未完成：屏上布局（车/履带/选中框）只有用户眼睛能验收；副作用：进 设置→保存 擦 Flash 会停 CPU 几十毫秒而 PWM 照转，属已知可接受。
下一步：让用户上电走一遍：进 设置→模块测试→电机，左右各按 正/反，看轮子方向与屏上读数（4000 占空比约 ±40）；再退到电流页确认两轮还在转。

## 2026-09-30
完成：回答了「菜单里 PID 速度环还是旧值」：前馈常数以前只是 firmware/App/Svc/speed_control.c 里的 static 变量（菜单和参数表里从来没有它们），菜单显示的 kp/ki/kd/限幅 我从未改过，所以当然是旧值。已把前馈做成参数：① firmware/App/Svc/host_link.h 加 HP_SPEED_FF_SLOPE 0x0605（默认 0.662，0..2）+ HP_SPEED_FF_FRIC 0x0606（默认 2327，0..5000），默认值做成 #ifndef 宏以便编译期 A/B；host_link.c 的 SPECS[] 加这两项（顺序紧跟 0x0604，指纹从 n=27/0x661D 变成 n=29/0x3886）。② speed_control.c 删掉 s_ff_slope/s_ff_fric，speed_pi_step() 新增 ff_slope/ff_fric 形参、由 SpeedControl_Task() 每拍从参数表取，Set/GetFeedforward 改走 host_param_set_local/host_param_get。③ app_ui.c 的 k_pid_speed_items 加「前馈斜率」（int binding 下标 14，decimals=3，所以 662 显示成 0.662）/「前馈摩擦」（下标 15），k_int_bindings 从 14 项变 16 项，push_pid_params/state_pull 都带上这两项。④ nvram.c payload 末尾追加这两个字段、version 1→2、v1 记录按 length 前缀迁移。验证（都是真跑过的）：make -j8 过（text 82112/data 228/bss 14344）；out/_param_crc_check.py 复算固件 SPECS 与上位机 protocol.PARAMS 逐项相同、crc16 都是 0x3886；python tools/host/tests/run_all.py 101 项全过；make flash 回读一致 82348 字节 firstDiff=none；用 stm32-dev.py read 做单次现场快照读到 g_cfg.speed_ff_slope_x1000=662、g_cfg.speed_ff_fric=2327、s_param[10]=0.662、s_param[11]=2327、k_app.int_binding_count=16 —— 即菜单里那两项确实指向控制环真正在读的那份值。
未完成：两条新发现的坑：① make flash（tools/flash.ps1）是 CubeProgrammer **mass erase** + 编程，而 nvram 用的是 Flash 最后一页，所以**每次烧录后菜单设置和黑白标定都回到默认**（n 迁移只在非整片擦除的烧录路径下才真正被用到）——刚标定完别重烧。② firmware/App/Src/app_ui.c 的 k_app.int_binding_count / bool_binding_count 原来是字面量（14 / 4），加绑定忘了改就会让菜单项索引越界、编辑框打不开，已改成 APP_UI_ITEM_COUNT() 宏。
下一步：请用户上菜单「PID→速度环」肉眼确认多出「前馈斜率 0.662 / 前馈摩擦 2327」两项，并当场改一个数看电机反应（1.5s 防抖后会自动存 Flash 最后一页）。

## 2026-09-30
完成：速度环精度验收通过（口径：高速段 ≤2%，低速段不做要求）。生产默认前馈 `SPEED_FF_SLOPE 0.662` / `SPEED_FF_FRIC 2327` + kp 20 / ki 2.0 / ilim 3000，8s 长窗口 n=1400 拍、每点复跑一遍，实测最大偏差：1500 −1.7%；2000 0.9%；3000 0.8%；4000 0.3%；5000 0.1%；（低速）1250 +5.3%；1000 −8.4%。两遍复跑数字几乎逐位重现。依据 `out/bench_ff6.log`、`out/bench_ff9.log`，表格已写进 `固件接口规格.md` §3.1。正式固件已 `make flash` 烧回：`build-app/haixin-app.bin` 81868 B、回读一致。
未完成：低速段（<1500）仍是静摩擦粘滑极限环，−8%~+5%，靠调参消不掉，靠上层限速规避。
下一步：实车确认外环（角度环/循迹环）相位裕度；以及把 `SpeedControl_SetFeedforward()` 接到上位机参数表，方便换硬件后重标。

## 2026-09-30
完成：摩擦前馈的定标方法走通：台架 BENCH_MODE 0 先开环扫静态特性（PWM→计数/秒），再按闭环工作点拟合 `out ≈ 2327 + 0.662×目标`。关键验证——目标 5000 时前馈给 5637，闭环实际稳定输出就是 5637 左右，即 **PI 的稳态贡献≈0，前馈自己落在工作点上**。中途踩过一个大坑：窗口之间不复位（`SpeedControl_Stop()`）会把上一窗口前馈攒下的积分叠进来，输出顶到 7200 饱和，整窗数据全假（`out/bench_ff1.log` 作废）。
下一步：换电机/减速比/电池电压后必须重新标定这两个常数。

## 2026-09-30
完成：**验收「20ms 滑动平均测速」到底有没有改进（用户 m00692 要求实测）。** 做法：给台架 `firmware/App/Bsp/motor_bench.c` 加量化指标（每个闭环点 `STAT` 打 `mean/p2p/rev/stall/outp`，`outp` = PWM 指令峰峰抖动），再用命令行宏覆盖做**同一份固件只差一个宏**的对比：`make MOTOR_BENCH=1 -B -j8 C_DEFS="-DUSE_HAL_DRIVER -DSTM32G474xx -DMOTOR_BENCH -DSPEED_AVG_TICKS=1"`（关平滑）vs 去掉最后一项（默认 4 = 开平滑），然后 `make MOTOR_BENCH=1 C_DEFS="..." flash` + `python …\stm32-dev.py rtt --elf firmware/build-bench/haixin-bench.elf --device STM32G474RE --seconds 80 --out out\bench_<tag>.log`。**按 A-B-A-B-A 交替跑了 5 次**（日志 `out/bench_s1_avg1.log`、`s4_avg4.log`、`s2_avg1.log`、`s4b_avg4.log`、`s1c_avg1.log`，每组 n=400 拍）。结论：① **目标 ≥3000 时 PWM 输出峰峰抖动降到约 1/3–1/2**，且「3 次关平滑」与「2 次开平滑」两组**取值范围完全不相交**（组均值，左/右：5000 目标 269/290→88/112；4000 290/296→155/160；3000 322/364→196/289）——真效果，不是噪声。② **目标 ≤1250 测不出差别**，因为那里的摆动是粘滑极限环本身（关平滑三次复跑目标 1000 的均值 358/510/386、输出峰峰 1882/809/1222）。③ **稳态跟踪零代价**：两版均值在每个目标点上一致；≥2000 稳在 ±5% 内，1500 约 ±10%。④ 机理自洽：每拍计数的 ±几拍随机抖动经 kp=20 放大成 ±100 量级 PWM；4 拍平均压掉不相关部分≈2–3 倍，而真实机械脉动（齿轮/齿槽）相关时间 >20ms 平均不掉——所以**速度读数的峰峰（1200–2200）两版一样，只有 PWM 输出抖动下降**。⑤ 已把结论写进 `固件接口规格.md` §3.2（含群延迟代价与外环风险）。⑥ 验收后已 `make -B flash` 烧回正式固件（`build-app/haixin-app.bin` **81804 B**，`OK：回读一致 81804 字节，firstDiff=none`）。
未完成：外环（角度环/循迹环）在 20ms 反馈延迟下会不会振，台架测不了，要实车。
下一步：上实车看循迹/角度环在 20ms 平滑下的相位裕度；或者按用户示意回退平滑（删窗口那 4 行即可）。

## 2026-09-30
完成：**踩到的两个方法论坑（下次做台架 A/B 直接照做）。** ① `make` 的 `C_DEFS='...'` 是**完全替换** Makefile 里的默认定义，漏掉 `-DUSE_HAL_DRIVER -DSTM32G474xx -DMOTOR_BENCH` 就编不过；带上才是干净的 A/B。② **低速段的台架对比不可靠**：粘滑极限环是混沌的，同一份固件两次复跑目标 1000 的均值差 42%、输出峰峰差 2.3 倍——单次对比在那里没有任何分辨力，必须 A-B-A 交替多次、看两组取值范围是否相交再下结论。另：`make -B` 的输出极长（89 个文件），要 `*> file` 重定向，别直接回显；`stm32-dev.py rtt` 打印的 head/tail 常是主循环的 `FRAME:/MOTION:` 行，**台架行一定在 `--out` 文件里**，要用 `Select-String -Pattern '^STAT|^CLOSE|^STEP'` 去文件里捞。

## 2026-09-30
完成：按用户拍板的方案实现了 20ms 滑动平均测速并烧录。`firmware/App/Svc/speed_control.c`：新增 `#define SPEED_AVG_TICKS 4`、静态量 `s_win[2][SPEED_AVG_TICKS]`/`s_win_sum[2]`/`s_win_n`/`s_win_pos` 与 `static void win_reset(void)`（在 `SpeedControl_Init()`、`SpeedControl_Enable()` 的 0→1 边沿、`SpeedControl_Stop()` 三处调用）；`speed_pi_step()` 的反馈形参从 `int counts` 改成 `float counts`（不取整，保留亚计数分辨率）；`SpeedControl_Task()` 里做环形窗口更新（`s_win_sum[0] -= s_win[0][s_win_pos]; s_win[0][s_win_pos] = dl; s_win_sum[0] += dl;` 右轮同理）后 `avg_l = s_win_sum[0] / s_win_n`（s_win_n 至少 1，不会除零），PI 和 `s_mmps[]` 都改吃 avg，**控制节拍仍是 5ms**。`firmware/App/Svc/speed_control.h` 同步了头部注释与 `SpeedControl_Counts()` 的注释。验证：`make -j8`（workdir=firmware）编译通过，只有已知的 RWX 链接警告；`make flash` 回读一致 81804 字节、firstDiff=none、程序已 go。**按用户要求没做台架 A/B 实测** —— User said (m00613 附近): 「不用测试了就用你说的方案」。回退成本很低：删掉窗口那几行、把 `avg_l/avg_r` 换回 `(float)s_counts[0]/(float)s_counts[1]` 即可。
未完成：平滑后的低速表现没上板验证（用户明确说不测）；`SPEEDFF_GAIN`/kp/ki 仍是台架整定值，量纲没变所以未重调。
下一步：在菜单/上位机侧把速度目标限到 ≥1500，然后联调循迹（灰度）+ 舵机。

## 2026-09-30
完成：2026-09-30 用 J-Link 把电机+编码器标定通过。① 新建 `make MOTOR_BENCH=1` 台架固件（`firmware/App/Bsp/motor_bench.c`，`#define BENCH_MODE` 0=完整台架/1=转向目视+起步脉宽/2=手推标定）；② 确认两轮编码器都工作、互不串扰、正反转对称；③ 手推标定 + 目视确认解出极性模型；④ 修 `motor.c`（`Load()` 第二路取负）与 `encoder.c`（左路取负）；⑤ 复测：`Load(+,+)` 两轮都朝车头（用户目视确认），速度环目标 1500–5000 跟到 ±5%；⑥ 结论写进 `固件接口规格.md` §3.1/§3.2；⑦ 烧回正式固件 `build-app/haixin-app.bin`（81612 B）。验证：RTT 日志 `out/bench_fix2.log`（CLOSE 分段实测）、`out/bench_dir4.log`（起步脉宽 + ±6000 计数）、`out/bench_spin.log`（手推累计符号）、`out/bench_probe2.log`（修前对照）。
未完成：速度目标 <1500 的粘滑极限环只做了文档化，没做控制律补偿；`SPEED_MIN_ACTIVE 500` 与真实死区 2600 仍然不匹配。
下一步：循迹（灰度）+ 舵机联调；上层给速度目标加 <1500 下限；补测五向键左/中。

## 2026-09-30
完成：把 SVD 换成 CLT 自带版（G4 v3.0）并验证：stm32-dev.py doctor 退出 0；svd TIM8.TIM8_CNT --all 读到 0x00001a18 @0x40013424；随后 RTT 3 s 抓到 77 行、HOST tx 从 1719 涨到 32827（目标未被挂住）。寄存器名不一定带前缀（I2C1=CR1，TIM8=TIM8_CNT）；svd 子命令读寄存器会因 JLinkGDBServer 不支持 detach 而让退出码为 1，属已知无害噪声。
下一步：无。

## 2026-09-30
完成：复核补丁后复测：不设 STM32_PROGRAMMER_CLI、PATH 里没有 SEGGER 的情况下 make flash 仍能找到 CLI（D:\APP\ST\STM32CubeCLT_1.22.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe）→ CLI 段 NVM size : 128 KBytes + Download verified successfully → J-Link 段 "Skipped. Contents already match" → 回读 81612 字节 firstDiff=none → 退出码 0。中途发现 tools.edit 丢 BOM 导致 .ps1 报错，已补回并复测通过。
下一步：继续别的目标；编辑 .ps1 后记得补 BOM。

## 2026-09-30
完成：用户装好 STM32CubeCLT 1.22.0（D:\APP\ST\，CLI 2.23.0）后复核整套链路，全部通过：make size（text 81384/data 220/bss 14264、bin 81612 B）→ make flash（CLI 段 NVM size : 128 KBytes + Download verified successfully；J-Link 段 "Skipped. Contents already match"；savebin 回读 81612 字节 firstDiff=none；退出码 0，日志 .work/flash-clt.log）→ stm32-dev.py rtt（77 行/4 s，横幅 BUILD: Sep 30 2026 12:33:16、CLK 170 MHz、host link init OK、FRAME avg≈674 us、HOST tx=1719 hz=50）→ stm32-dev.py doctor 退出 0。给 tools/flash.ps1 打了三处补丁（CLI 探测改先点已知布局、D:\APP 从递归扫描剔除、CLI 失败只告警继续走 J-Link）。另按用户要求清了 C: 盘：3.14 GB → 23.13 GB 剩余（约 +20 GB），用户随后叫停。
未完成：C: 清理停在「净 +20 GB」：pagefile 32 GB 固定未改；DISM /Online /Cleanup-Image /StartComponentCleanup /ResetBase 被拒 0x80070005（Access is denied，日志 C:\Windows\Logs\DISM\dism.log）未解。CLT 自带的 STMicroelectronics_CMSIS_SVD 未与现用 SVD 比对。
下一步：等用户下一个目标（当前工具链构建/烧录/观测/doctor 全绿；便捷路径 make flash 与 tools/flash.ps1 都能用）。

## 2026-09-30
完成：Makefile 的 shell 改成 Git for Windows 自带的 sh（SHELL = C:/Program Files/Git/bin/sh.exe，本机本来就有，无需安装 —— 之前"没有 sh"只是 PATH 里没有），clean/mkdir 恢复 POSIX 写法；踩到 GNU make 在 Windows 上对不含 shell 元字符的行会跳过 shell 直接 CreateProcess，所以 mkdir -p "$@" 的引号必须留着。全链实测通过：make clean（真删干净、无 "-p"）→ make -j4（bin 81612 B）→ make erase → erase 后第一次 make flash 一次成功、回读一致、退出码 0。tools/flash.ps1 的 CubeProgrammer 路径改为自动探测（-Cp > 环境变量 > PATH > C:/D: 常见位置），用户装完即自动生效。
未完成：CubeProgrammer 仍未安装（用户自己去装）。
下一步：用户装完 CubeProgrammer 后跑 make flash，确认前置段打印出找到的 CLI 路径。

## 2026-09-30
完成：本机烧录便捷路径全部修好并实测：Makefile 的 flash 目标改为调用 tools/flash.ps1（单一实现），新增 tools/verify_readback.ps1 逐字节回读比对（容忍 0x080001D8..0x080001DF），以回读为唯一结论、失败自动重试一次；DEVICE 改 STM32G474RE；clean 改 rmdir；mkdir 不再建 "-p"；cmd 的 echo 引号坑用 (echo a&echo b) 分组解决。实测：erase→flash 全量 81920 字节 + 回读一致退出码 0；内容已一致时 J-Link 报假错（Programming failed @ 0x08010534）但回读一致、仍退 0；make clean + make -j4 全量重编译通过（bin 81612 B）；烧完 RTT 抓到启动横幅证明板子在跑，DEMCR 已清。
未完成：STM32CubeProgrammer CLI 仍未安装（ST 官网下载要走下载/登录流程，抓不到直链；C: 只剩 3.40 GB）。
下一步：等用户决定是否装 CubeProgrammer；不装就维持现有 J-Link + 回读链路（flash.ps1 已留好分支）。

## 2026-09-30
完成：实测两件事：① 技能的 rtt 子命令跑完会在 DEMCR(0xE000EDFC) 留下 VC_CORERESET=0x01000000（清 0 → 跑一次 → 读回 0x01000000），所以观测完要补一次 J-Link 写 0 并 g；② NVM 页 0x0801F800 全 0xFF、SR=0xA0 是复位后的本来状态（firmware/App/Svc/nvram.c:173-177 的注释已记：PGAERR|PGSERR=0xA0 会让 HAL 直接返回错误，所以代码先 __HAL_FLASH_CLEAR_FLAG 再擦）。
未完成：make flash 与 tools/flash.ps1 两处便捷烧录路径未修；用户已明确要求把缺的依赖装上（STM32CubeProgrammer CLI）。
下一步：装 STM32CubeProgrammer/STM32CubeCLT 让 tools/flash.ps1 能跑，并修 Makefile 的 flash.jlink 生成（去引号 + DEVICE 改 STM32G474RE）

## 2026-09-30
完成：本机把 stm32-dev 链路跑通：Cube G4 补包（HAL + CMSIS 两个 submodule）+ SVD（STM32G4xx/Include/STM32G474.svd）+ plain make 就地重建 build-app（text 81384/data 220/bss 14264、bin 81612 B）+ -device STM32G474RE 烧 81920 字节并 savebin 回读（只差已知 8 字节空洞）+ stm32-dev rtt 抓到启动横幅与实时 FRAME/MOTION/HOST 行（BUILD Sep 30 2026 11:12:37、nvram loaded=0、enc l=0 r=0 yaw=-4）。中途「烧不进去」的真相是我生成的 .jlink 把 loadfile 行拆成了两行，板子与探针无损，已清 DEMCR 并放程序跑。
未完成：make flash 在本机会挂死（flash.jlink 每行带引号 + DEVICE=RB 静默假成功）；tools/flash.ps1 依赖的 STM32CubeProgrammer CLI 本机没装。两处都没改，等用户定。
下一步：把 Makefile 生成 flash.jlink 的行去掉引号、DEVICE 改 STM32G474RE（或让 make flash 直接调 stm32-dev flash）

## 2026-09-29
完成：在真板上把 INA226 的全部寄存器读出来核对（两次复位完全一致）：ID=5449/2260（真芯片在线）、reg00=4127、reg01=0x7FFF（分流满量程钳位、每个采样都恰好 0x7FFF）、reg02 换算 12.023V、reg04/03 换算 16.384A/197W、reg06 CVRF=1 OVF=0。用 CVRF 计数反推转换周期，实测确认配置位序：bit15 RST / bit14:12 只读保留(写任何值都读回 100) / bit11:9 AVG / bit8:6 VBUSCT / bit5:3 VSHCT / bit2:0 MODE，写 0x0000 掉电停转换。0x4127 = 1 次平均 + 1.1ms + 1.1ms + 连续，2.26ms 一次更新；CAL=0x0400=1024 = 0.00512/(1mA*5mΩ) 正确。电池网表 pcb/Netlist_Schematic1_2026-09-29.tel 核对：R2.2+U5.10=IN+ 在源侧、R2.1+U5.8(VBUS)+U5.9(IN-) 在负载侧，与设计一致（不是接线错）。结论：配置与读码换算都对，是分流模拟输入顶到满量程。据此改代码：board.c ina226_init 的错注释"16 次平均"换成实测位序，电流检测页在满量程时给电流/功率加 '!' 前缀（out/oled_power.png 实拍：!16.38A / 12.02V / !197.03W），TEMP 探针（board.c ina226_temp_probe/probe2/cvrf + main_app.c 调用）全部删除；make text 81288/data 220/bss 14264、flash.ps1 回读 firstDiff=none、RTT err=0 busy=0 st=0、表盘 dump 正常。
未完成：分流输入为何顶格仍未定：可能是 R2 焊点/实际阻值远大于 5mΩ（0.2A 下要 0.41Ω 才钳满）、或 U5 分流输入级被此前的 5V 灌 3V3 事故打坏、或 sense 走线开路把 IN+ 拉到近地。需用户在通电跑动时用万用表直流档量 R2 两个焊盘（0.2A 时约 1mV）并对照电源自身电流表。
下一步：用户给出 R2 两端电压后判定是 R2/焊点还是 U5，再决定换件或改字模（超量程 的中文需重生成 kk_font_body）

## 2026-09-29 · KK_UI 界面
完成：用户 m04708：翻页时表盘那圈圆角框必须自己当合缝（原来 32px 视差 + 合缝取进来那页左边缘，出去的表盘框右半边被裁掉、看着像被黑线切开）。改成 kk_ui_nav.c 的 KK_UI_DrawScene 里两页都整屏滑 127px、合缝 = 进来那页的左边缘，且进来那页的 x = (127 - slide)，保证 boundary-1 恰好是表盘框右边缘列（1+off+125 = off+126）。用 TEMP 冻结过渡在 35%（duration=60000 + progress=Q12*35/100）在真板上分别抓正向 home→menu（out/oled_tr_fwd.png：框右边缘竖线在第 82 列完整可见、右边紧贴菜单）与反向 menu→home（out/oled_tr_rev.png：表盘框从左边完整滑入，右边缘第 43 列）；TEMP 全部还原、root_page 回 PAGE_SPLASH、make -j4 text 81108/data 220/bss 14264、tools/flash.ps1 回读一致 81336 字节 firstDiff=none、RTT out/rtt_bezel3.log 启动 10 行齐 + FRAME avg=690us max=3410us tx_done=16 err=0 busy=0 st=0。顺手修掉 app_ui.c draw_splash_page 里指向已删除的 kk_ui_draw_card() 的过期注释。
未完成：kk_ui_draw_card() 已整段删除，KK_UI_PAGE_PARALLAX 宏变成没人用（留着没删）；用户还没回答四个电源灯闪烁的排查问题
下一步：等用户看实屏反馈：若过渡速度/观感要调，改 KK_UI_PAGE_MS 或过渡时长；若要菜单页也带框，就是另一套需求

## 2026-09-29 · KK_UI 界面
完成：按用户 m04657 的澄清重做外框：外框归'页'所有，翻页时每页各自的圆角框跟着自己滑，菜单页静止时不带框。(1) kk_ui_nav.c 新增 static void kk_ui_draw_card(int16_t x_offset, int16_t clip_x, uint16_t clip_width)：按该页裁剪窗口画 OLED_DrawRFrame(1+x_offset, 1, 126U, 62U, KK_UI_BEZEL_RADIUS)，KK_UI_DrawScene 的正向/反向过渡分支里在两个 KK_UI_DrawPage 之后各调一次；原来那段'贴屏边常驻外框'（OLED_DrawRFrame(0,0,128,64,6)，不带 s_dx）已删除。(2) app_ui.c 的 draw_splash_page() 恢复自画外框 OLED_DrawRFrame((1+s_dx),1,126U,62U,6U)，与 kk_ui_draw_card 完全同几何（所以表盘页不会双重描边）。效果：表盘静止有框、菜单静止无框、主页->任务过渡时合缝处是进来那页的带圆角竖线（不再是黑边直切）。真板实拍（TEMP 把过渡冻结在 35%：start_transition 里 duration=60000、DrawScene 里 progress 强制 35%、app_ui_init 末尾 NavigateTo(PAGE_HOME)）：out/oled_tr_real.png + out/oled_tr_real.bin；菜单静止 out/oled_menu_noframe.png；表盘静止 out/oled_splash_now.png。生产固件 text 81168/data 220/bss 14264、bin 81488 字节，tools/flash.ps1 回读一致 firstDiff=none，RTT err=0 busy=0 st=0。
未完成：过渡结束那一帧进来那页的框会消失（静止不带框的必然结果）；用户若只要'一条带圆角的竖线'、不要上下两条横边线，把进来那页的 clip 收窄到 8px 即可（还没做）。
下一步：等用户看 out/oled_tr_real.png 确认合缝样式；若要窄边线版本按上面的做法改 kk_ui_draw_card 的裁剪宽度。

## 2026-09-29 · KK_UI 界面
完成：把屏幕外框改成 KK_UI 常驻圆角白框：firmware/Middlewares/kk_ui/src/kk_ui_nav.c 的 KK_UI_DrawScene 在画完两页（含过渡位移）之后、对话框/Toast 之前，无条件 OLED_DrawRFrame(0,0,KK_UI_SCREEN_WIDTH,KK_UI_SCREEN_HEIGHT,KK_UI_BEZEL_RADIUS=6U)，常量加在 firmware/Middlewares/kk_ui/src/kk_ui_internal.h:33；同时删掉 firmware/App/Src/app_ui.c draw_splash_page() 里原来那句 OLED_DrawRFrame((int16_t)(1+s_dx),1,126U,62U,6U)（它带 s_dx，翻页时会跟着内容滑走、屏沿露出黑边，用户 m04517 就是看到这个）。构建 text 81132/data 220/bss 14264，tools/flash.ps1 回读 firstDiff=none，RTT 启动 10 行齐、FRAME avg=677us max=3425us tx_done=16 err=0 busy=0 st=0。实屏取证：J-Link savebin 0x20001230 2048 后 node out/render_oled.mjs 渲染成 PNG —— out/oled_final.png（表盘+贴边白框）、out/oled_menu2.png（图标菜单+同一个框）、out/sim_tr_035.png（用 KK_UI 过渡公式把两页真实缓冲合成的翻页中途帧，框定住不动）。
未完成：翻页中途只有合成图、没有真板实拍（没法注入按键）；外框现在所有页面都有，用户若只想让表盘有需回退。
下一步：等用户看板确认外框观感；若嫌图标菜单底部标签贴到框上可把 KK_UI_BEZEL_RADIUS 或 home 标签 y 微调。

> 更早的会话条目已归档到 252 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。
