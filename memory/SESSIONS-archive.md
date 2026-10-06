## 2026-09-30
完成：电机测试页（PAGE_CUSTOM_MOTOR）按用户 m01975「界面有点丑」重画成两行卡片式仪表盘，见 firmware/App/Src/app_ui.c 的 draw_motor_page()：标题 y=1 + OLED_DrawHLine(0,14,128U) 分隔线（与陀螺仪页同款）；下面两行卡片 y0 = 16 / 39、行高 23，每行 = 履带图标（dir!=0 用 OLED_DrawRBox 实心表示在转、停用 DrawRFrame 空心，x=5,y0+6,18x10,r=4）+ 左/右（x=28,y0+5）+ 停/正/反（x=42,y0+5）+ 速度条（框 OLED_DrawRFrame(58+s_dx,y0+7,42,9,2)，填充 OLED_DrawRBox(59+s_dx,y0+8,mag*40/100,7,2)）+ 读数右对齐 126-bw（y0+5）；选中行用 XOR 空心圆角框 OLED_DrawRFrame(1+s_dx,y0-1,126,23,5)。同时把 MOTOR_TEST_PWM 4000 改成 6000（实测 4000 只有 2083/1900 计数每秒 = 满速 42%，读数就是 40；6000 约 3650 计数每秒 → 读数约 73），并在 KK_UI_CustomOnInput() 的模块测试页与电机页两处 OK 分支前加了 `if (event.action == KK_UI_INPUT_OK && event.source == KK_UI_INPUT_REPEAT) return;`（否则按住中键会把方向档 停→正→反→停 一路刷过去）。验证：make -j8 过（text 86144 / data 228 / bss 14352，只剩已知 RWX 链接警告）；make flash 回读一致 86380 字节 firstDiff=none；J-Link 单次读 uwTick=13123（CPU 停在 HAL_Delay 里，板子在跑）、s_motor_sel=0 / s_motor_dir={0,0} / s_motor_show={0,0}；python tools/host/tests/run_all.py 101 项全过；布局自查用 PIL 按同一套坐标画 128x64 示意图（out/motor_page_mock.py → out/motor_page_mock.png，逐元素核对无压字、无出界）。
未完成：用户侧肉眼验收新版面与读数刻度。
下一步：让用户上电走一遍：进 设置→模块测试→电机，看两行卡片版式、左/右按中键切 停/正/反、读数是否约 73（新占空比）、按住中键不再连刷、退出后电机继续转。

> 更早的会话条目已归档到 273 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：修好「模块测试页按中键进不去电机测试页」：根因是 kk_ui 的导航策略在 firmware/Middlewares/kk_ui/src/kk_ui_nav.c:104-109 直接 `return KK_UI_NOT_ALLOWED` 拦掉「自定义页→自定义页」压栈，而 firmware/App/Src/app_ui.c 的中键处理写的是 `(void)KK_UI_NavigateTo(PAGE_CUSTOM_MOTOR, HAL_GetTick())` —— 返回值被丢掉，所以按键完全没有反应（屏不换、电机也不转），比报错更难查。改法：删掉那 5 行守卫并写明理由（应用确有两级自定义流程；框架已有 leave_custom 回调 + 类型无关弹栈）。验证：make -j8 过（text 86016 / data 228 / bss 14352，只剩已知 RWX 链接警告）、make flash 回读一致 86252 字节 firstDiff=none、J-Link 单次快照 uwTick=29107（在跑）且 kk_ui.transition.active=0 / blocked_keys=0 / keys[].stable=0 且 armed=1（无卡键）/ stack_count=0 / error.code=KK_UI_OK / current.page=10（PAGE_SPLASH，刚烧完重启）。
未完成：用户上电复测：设置→模块测试→选「电机」按中键应能进入电机测试页（并确认页内左/右 正/反/返回 与编码器读数）。
下一步：若在模块测试页按键仍无反应，用 J-Link 快照 kk_ui.keys[].pressed_at 与 kk_ui.current.page 区分「按键没进来」还是「页面状态卡住」——kk_ui 是全局符号（0x200010c8，size 0x1a4），可直接读。

> 更早的会话条目已归档到 272 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：重新生成 12px 字模修好「屏上汉字静默不画」（用户 m01831 报：菜单里前馈斜率/前馈摩擦是黑的）。做法：先 node -e 扫 firmware/{App/Src,App/Inc,App/Svc/trace_cal.c,App/Src/oled_diag.c,Middlewares/kk_ui/{src,include}} 里所有 "..." 字面量的汉字得到 267 字（存 out/_ui_chars.txt），再 `node tools/font/gen_font.mjs firmware/App/Fonts/kk_font_body.c wenquanyi_9pt 12 kk_font_body $ui` → 459 字形 / 11015 B（旧 346 / 7917）。验证：check_charset 对那 267 字**缺字=0**、对 '前馈斜率前馈摩擦' 缺字=0；`make -j8` text 86036 / data 228 / bss 14352（只剩已知 RWX 警告）；`make flash` → 回读一致 86272 字节 firstDiff=none；J-Link 快照 uwTick=14927（板子在跑）、k_app.route_count=16 / custom_page_count=8 / fonts.body_font=kk_font_body。同时把 tools/font/gen_font.mjs 改成自动保留旧文件的 #include 行。另：查清并写进 固件接口规格.md §3.1 —— ilim 3000 限的是积分累加器（单位「计数/5ms×拍」），经 ki=2 折算最多贡献 ±6000 PWM，既不是编码器量程也不是 7200 输出限幅。
未完成：make flash 是 mass erase，这次又把 NVRAM 里的菜单设置和黑白阈值标定清回默认了
下一步：让用户上电进「PID→速度环」看前馈斜率/前馈摩擦两项是否显示出来

> 更早的会话条目已归档到 271 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：模块测试页的「电机」项改成进独立页 `PAGE_CUSTOM_MOTOR`（`firmware/App/Inc/app_ui.h`：16 / `CUSTOM_MOTOR` 7 / `PAGE_COUNT` 16 / `CUSTOM_COUNT` 8；`firmware/App/Src/app_ui.c` 的 `k_routes`、`KK_UI_CustomOnEnter/OnLeave/OnInput/OnTick/OnDraw` 各有分支，新增 `draw_motor_page()`：车体+两条履带+选中侧 XOR 圆角框+右上角「返回」+底行左/右读数）。左/右各自用中键在 停→正→反→停 循环（`MOTOR_TEST_PWM 4000`），**离开页面不停电机**（删掉 `KK_UI_CustomOnLeave()` 里原来那句无条件 `Load(0, 0)`），方向存在 `s_motor_dir[2]` 跨页保留，进页面若速度环使能先 `SpeedControl_Stop()` 再 `motor_test_apply()`。读数口径按用户 m01700 改成「最近 20ms 窗口的计数之和」：新增 `int SpeedControl_WindowCounts(int wheel)`（`firmware/App/Svc/speed_control.c:312` 返回 `s_win_sum[wheel & 1]`；该求和在 `SpeedControl_Task()` 里位于 `if (!s_enabled) return;` **之前**，未使能也有值），满速 ≈100（`SPEED_FULL_COUNTS 25.0f × SPEED_AVG_TICKS 4` = 100），倒转为负；里程表 odometer 方案加过又全删。模块测试列表 `TEST_N 4→3` = 蜂鸣器/电机/返回，电机行尾显示 运行/停。验证：`make -j8` 过（text 82940 / data 228 / bss 14352，只剩已知 RWX 警告）；`make flash` 回读一致 83176 字节 firstDiff=none；J-Link 单次快照读到 `k_app.route_count=16`、`k_app.custom_page_count=8`、`k_app.int_binding_count=16`、`s_motor_sel=0`、`s_motor_dir={0,0}`、`s_motor_show={0,0}`，两次读 `uwTick` 12311→19388 证明板子仍在跑；`python tools/host/tests/run_all.py` 101 项全过；字模核对 `'左轮右轮电机测试返回停正反运行计数'` 只缺「轮」→ 屏上文案只用「左」「右」。`固件接口规格.md` 的测速口径小节已补这一条。
未完成：屏上布局（车/履带/选中框）只有用户眼睛能验收；副作用：进 设置→保存 擦 Flash 会停 CPU 几十毫秒而 PWM 照转，属已知可接受。
下一步：让用户上电走一遍：进 设置→模块测试→电机，左右各按 正/反，看轮子方向与屏上读数（4000 占空比约 ±40）；再退到电流页确认两轮还在转。

> 更早的会话条目已归档到 270 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：回答了「菜单里 PID 速度环还是旧值」：前馈常数以前只是 firmware/App/Svc/speed_control.c 里的 static 变量（菜单和参数表里从来没有它们），菜单显示的 kp/ki/kd/限幅 我从未改过，所以当然是旧值。已把前馈做成参数：① firmware/App/Svc/host_link.h 加 HP_SPEED_FF_SLOPE 0x0605（默认 0.662，0..2）+ HP_SPEED_FF_FRIC 0x0606（默认 2327，0..5000），默认值做成 #ifndef 宏以便编译期 A/B；host_link.c 的 SPECS[] 加这两项（顺序紧跟 0x0604，指纹从 n=27/0x661D 变成 n=29/0x3886）。② speed_control.c 删掉 s_ff_slope/s_ff_fric，speed_pi_step() 新增 ff_slope/ff_fric 形参、由 SpeedControl_Task() 每拍从参数表取，Set/GetFeedforward 改走 host_param_set_local/host_param_get。③ app_ui.c 的 k_pid_speed_items 加「前馈斜率」（int binding 下标 14，decimals=3，所以 662 显示成 0.662）/「前馈摩擦」（下标 15），k_int_bindings 从 14 项变 16 项，push_pid_params/state_pull 都带上这两项。④ nvram.c payload 末尾追加这两个字段、version 1→2、v1 记录按 length 前缀迁移。验证（都是真跑过的）：make -j8 过（text 82112/data 228/bss 14344）；out/_param_crc_check.py 复算固件 SPECS 与上位机 protocol.PARAMS 逐项相同、crc16 都是 0x3886；python tools/host/tests/run_all.py 101 项全过；make flash 回读一致 82348 字节 firstDiff=none；用 stm32-dev.py read 做单次现场快照读到 g_cfg.speed_ff_slope_x1000=662、g_cfg.speed_ff_fric=2327、s_param[10]=0.662、s_param[11]=2327、k_app.int_binding_count=16 —— 即菜单里那两项确实指向控制环真正在读的那份值。
未完成：两条新发现的坑：① make flash（tools/flash.ps1）是 CubeProgrammer **mass erase** + 编程，而 nvram 用的是 Flash 最后一页，所以**每次烧录后菜单设置和黑白标定都回到默认**（n 迁移只在非整片擦除的烧录路径下才真正被用到）——刚标定完别重烧。② firmware/App/Src/app_ui.c 的 k_app.int_binding_count / bool_binding_count 原来是字面量（14 / 4），加绑定忘了改就会让菜单项索引越界、编辑框打不开，已改成 APP_UI_ITEM_COUNT() 宏。
下一步：请用户上菜单「PID→速度环」肉眼确认多出「前馈斜率 0.662 / 前馈摩擦 2327」两项，并当场改一个数看电机反应（1.5s 防抖后会自动存 Flash 最后一页）。

> 更早的会话条目已归档到 269 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：速度环精度验收通过（口径：高速段 ≤2%，低速段不做要求）。生产默认前馈 `SPEED_FF_SLOPE 0.662` / `SPEED_FF_FRIC 2327` + kp 20 / ki 2.0 / ilim 3000，8s 长窗口 n=1400 拍、每点复跑一遍，实测最大偏差：1500 −1.7%；2000 0.9%；3000 0.8%；4000 0.3%；5000 0.1%；（低速）1250 +5.3%；1000 −8.4%。两遍复跑数字几乎逐位重现。依据 `out/bench_ff6.log`、`out/bench_ff9.log`，表格已写进 `固件接口规格.md` §3.1。正式固件已 `make flash` 烧回：`build-app/haixin-app.bin` 81868 B、回读一致。
未完成：低速段（<1500）仍是静摩擦粘滑极限环，−8%~+5%，靠调参消不掉，靠上层限速规避。
下一步：实车确认外环（角度环/循迹环）相位裕度；以及把 `SpeedControl_SetFeedforward()` 接到上位机参数表，方便换硬件后重标。

> 更早的会话条目已归档到 268 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：摩擦前馈的定标方法走通：台架 BENCH_MODE 0 先开环扫静态特性（PWM→计数/秒），再按闭环工作点拟合 `out ≈ 2327 + 0.662×目标`。关键验证——目标 5000 时前馈给 5637，闭环实际稳定输出就是 5637 左右，即 **PI 的稳态贡献≈0，前馈自己落在工作点上**。中途踩过一个大坑：窗口之间不复位（`SpeedControl_Stop()`）会把上一窗口前馈攒下的积分叠进来，输出顶到 7200 饱和，整窗数据全假（`out/bench_ff1.log` 作废）。
下一步：换电机/减速比/电池电压后必须重新标定这两个常数。

> 更早的会话条目已归档到 267 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：**验收「20ms 滑动平均测速」到底有没有改进（用户 m00692 要求实测）。** 做法：给台架 `firmware/App/Bsp/motor_bench.c` 加量化指标（每个闭环点 `STAT` 打 `mean/p2p/rev/stall/outp`，`outp` = PWM 指令峰峰抖动），再用命令行宏覆盖做**同一份固件只差一个宏**的对比：`make MOTOR_BENCH=1 -B -j8 C_DEFS="-DUSE_HAL_DRIVER -DSTM32G474xx -DMOTOR_BENCH -DSPEED_AVG_TICKS=1"`（关平滑）vs 去掉最后一项（默认 4 = 开平滑），然后 `make MOTOR_BENCH=1 C_DEFS="..." flash` + `python …\stm32-dev.py rtt --elf firmware/build-bench/haixin-bench.elf --device STM32G474RE --seconds 80 --out out\bench_<tag>.log`。**按 A-B-A-B-A 交替跑了 5 次**（日志 `out/bench_s1_avg1.log`、`s4_avg4.log`、`s2_avg1.log`、`s4b_avg4.log`、`s1c_avg1.log`，每组 n=400 拍）。结论：① **目标 ≥3000 时 PWM 输出峰峰抖动降到约 1/3–1/2**，且「3 次关平滑」与「2 次开平滑」两组**取值范围完全不相交**（组均值，左/右：5000 目标 269/290→88/112；4000 290/296→155/160；3000 322/364→196/289）——真效果，不是噪声。② **目标 ≤1250 测不出差别**，因为那里的摆动是粘滑极限环本身（关平滑三次复跑目标 1000 的均值 358/510/386、输出峰峰 1882/809/1222）。③ **稳态跟踪零代价**：两版均值在每个目标点上一致；≥2000 稳在 ±5% 内，1500 约 ±10%。④ 机理自洽：每拍计数的 ±几拍随机抖动经 kp=20 放大成 ±100 量级 PWM；4 拍平均压掉不相关部分≈2–3 倍，而真实机械脉动（齿轮/齿槽）相关时间 >20ms 平均不掉——所以**速度读数的峰峰（1200–2200）两版一样，只有 PWM 输出抖动下降**。⑤ 已把结论写进 `固件接口规格.md` §3.2（含群延迟代价与外环风险）。⑥ 验收后已 `make -B flash` 烧回正式固件（`build-app/haixin-app.bin` **81804 B**，`OK：回读一致 81804 字节，firstDiff=none`）。
未完成：外环（角度环/循迹环）在 20ms 反馈延迟下会不会振，台架测不了，要实车。
下一步：上实车看循迹/角度环在 20ms 平滑下的相位裕度；或者按用户示意回退平滑（删窗口那 4 行即可）。

> 更早的会话条目已归档到 266 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：**踩到的两个方法论坑（下次做台架 A/B 直接照做）。** ① `make` 的 `C_DEFS='...'` 是**完全替换** Makefile 里的默认定义，漏掉 `-DUSE_HAL_DRIVER -DSTM32G474xx -DMOTOR_BENCH` 就编不过；带上才是干净的 A/B。② **低速段的台架对比不可靠**：粘滑极限环是混沌的，同一份固件两次复跑目标 1000 的均值差 42%、输出峰峰差 2.3 倍——单次对比在那里没有任何分辨力，必须 A-B-A 交替多次、看两组取值范围是否相交再下结论。另：`make -B` 的输出极长（89 个文件），要 `*> file` 重定向，别直接回显；`stm32-dev.py rtt` 打印的 head/tail 常是主循环的 `FRAME:/MOTION:` 行，**台架行一定在 `--out` 文件里**，要用 `Select-String -Pattern '^STAT|^CLOSE|^STEP'` 去文件里捞。

> 更早的会话条目已归档到 265 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：按用户拍板的方案实现了 20ms 滑动平均测速并烧录。`firmware/App/Svc/speed_control.c`：新增 `#define SPEED_AVG_TICKS 4`、静态量 `s_win[2][SPEED_AVG_TICKS]`/`s_win_sum[2]`/`s_win_n`/`s_win_pos` 与 `static void win_reset(void)`（在 `SpeedControl_Init()`、`SpeedControl_Enable()` 的 0→1 边沿、`SpeedControl_Stop()` 三处调用）；`speed_pi_step()` 的反馈形参从 `int counts` 改成 `float counts`（不取整，保留亚计数分辨率）；`SpeedControl_Task()` 里做环形窗口更新（`s_win_sum[0] -= s_win[0][s_win_pos]; s_win[0][s_win_pos] = dl; s_win_sum[0] += dl;` 右轮同理）后 `avg_l = s_win_sum[0] / s_win_n`（s_win_n 至少 1，不会除零），PI 和 `s_mmps[]` 都改吃 avg，**控制节拍仍是 5ms**。`firmware/App/Svc/speed_control.h` 同步了头部注释与 `SpeedControl_Counts()` 的注释。验证：`make -j8`（workdir=firmware）编译通过，只有已知的 RWX 链接警告；`make flash` 回读一致 81804 字节、firstDiff=none、程序已 go。**按用户要求没做台架 A/B 实测** —— User said (m00613 附近): 「不用测试了就用你说的方案」。回退成本很低：删掉窗口那几行、把 `avg_l/avg_r` 换回 `(float)s_counts[0]/(float)s_counts[1]` 即可。
未完成：平滑后的低速表现没上板验证（用户明确说不测）；`SPEEDFF_GAIN`/kp/ki 仍是台架整定值，量纲没变所以未重调。
下一步：在菜单/上位机侧把速度目标限到 ≥1500，然后联调循迹（灰度）+ 舵机。

> 更早的会话条目已归档到 264 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：2026-09-30 用 J-Link 把电机+编码器标定通过。① 新建 `make MOTOR_BENCH=1` 台架固件（`firmware/App/Bsp/motor_bench.c`，`#define BENCH_MODE` 0=完整台架/1=转向目视+起步脉宽/2=手推标定）；② 确认两轮编码器都工作、互不串扰、正反转对称；③ 手推标定 + 目视确认解出极性模型；④ 修 `motor.c`（`Load()` 第二路取负）与 `encoder.c`（左路取负）；⑤ 复测：`Load(+,+)` 两轮都朝车头（用户目视确认），速度环目标 1500–5000 跟到 ±5%；⑥ 结论写进 `固件接口规格.md` §3.1/§3.2；⑦ 烧回正式固件 `build-app/haixin-app.bin`（81612 B）。验证：RTT 日志 `out/bench_fix2.log`（CLOSE 分段实测）、`out/bench_dir4.log`（起步脉宽 + ±6000 计数）、`out/bench_spin.log`（手推累计符号）、`out/bench_probe2.log`（修前对照）。
未完成：速度目标 <1500 的粘滑极限环只做了文档化，没做控制律补偿；`SPEED_MIN_ACTIVE 500` 与真实死区 2600 仍然不匹配。
下一步：循迹（灰度）+ 舵机联调；上层给速度目标加 <1500 下限；补测五向键左/中。

> 更早的会话条目已归档到 263 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：把 SVD 换成 CLT 自带版（G4 v3.0）并验证：stm32-dev.py doctor 退出 0；svd TIM8.TIM8_CNT --all 读到 0x00001a18 @0x40013424；随后 RTT 3 s 抓到 77 行、HOST tx 从 1719 涨到 32827（目标未被挂住）。寄存器名不一定带前缀（I2C1=CR1，TIM8=TIM8_CNT）；svd 子命令读寄存器会因 JLinkGDBServer 不支持 detach 而让退出码为 1，属已知无害噪声。
下一步：无。

> 更早的会话条目已归档到 262 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：复核补丁后复测：不设 STM32_PROGRAMMER_CLI、PATH 里没有 SEGGER 的情况下 make flash 仍能找到 CLI（D:\APP\ST\STM32CubeCLT_1.22.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe）→ CLI 段 NVM size : 128 KBytes + Download verified successfully → J-Link 段 "Skipped. Contents already match" → 回读 81612 字节 firstDiff=none → 退出码 0。中途发现 tools.edit 丢 BOM 导致 .ps1 报错，已补回并复测通过。
下一步：继续别的目标；编辑 .ps1 后记得补 BOM。

> 更早的会话条目已归档到 261 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：用户装好 STM32CubeCLT 1.22.0（D:\APP\ST\，CLI 2.23.0）后复核整套链路，全部通过：make size（text 81384/data 220/bss 14264、bin 81612 B）→ make flash（CLI 段 NVM size : 128 KBytes + Download verified successfully；J-Link 段 "Skipped. Contents already match"；savebin 回读 81612 字节 firstDiff=none；退出码 0，日志 .work/flash-clt.log）→ stm32-dev.py rtt（77 行/4 s，横幅 BUILD: Sep 30 2026 12:33:16、CLK 170 MHz、host link init OK、FRAME avg≈674 us、HOST tx=1719 hz=50）→ stm32-dev.py doctor 退出 0。给 tools/flash.ps1 打了三处补丁（CLI 探测改先点已知布局、D:\APP 从递归扫描剔除、CLI 失败只告警继续走 J-Link）。另按用户要求清了 C: 盘：3.14 GB → 23.13 GB 剩余（约 +20 GB），用户随后叫停。
未完成：C: 清理停在「净 +20 GB」：pagefile 32 GB 固定未改；DISM /Online /Cleanup-Image /StartComponentCleanup /ResetBase 被拒 0x80070005（Access is denied，日志 C:\Windows\Logs\DISM\dism.log）未解。CLT 自带的 STMicroelectronics_CMSIS_SVD 未与现用 SVD 比对。
下一步：等用户下一个目标（当前工具链构建/烧录/观测/doctor 全绿；便捷路径 make flash 与 tools/flash.ps1 都能用）。

> 更早的会话条目已归档到 260 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：Makefile 的 shell 改成 Git for Windows 自带的 sh（SHELL = C:/Program Files/Git/bin/sh.exe，本机本来就有，无需安装 —— 之前"没有 sh"只是 PATH 里没有），clean/mkdir 恢复 POSIX 写法；踩到 GNU make 在 Windows 上对不含 shell 元字符的行会跳过 shell 直接 CreateProcess，所以 mkdir -p "$@" 的引号必须留着。全链实测通过：make clean（真删干净、无 "-p"）→ make -j4（bin 81612 B）→ make erase → erase 后第一次 make flash 一次成功、回读一致、退出码 0。tools/flash.ps1 的 CubeProgrammer 路径改为自动探测（-Cp > 环境变量 > PATH > C:/D: 常见位置），用户装完即自动生效。
未完成：CubeProgrammer 仍未安装（用户自己去装）。
下一步：用户装完 CubeProgrammer 后跑 make flash，确认前置段打印出找到的 CLI 路径。

> 更早的会话条目已归档到 259 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：本机烧录便捷路径全部修好并实测：Makefile 的 flash 目标改为调用 tools/flash.ps1（单一实现），新增 tools/verify_readback.ps1 逐字节回读比对（容忍 0x080001D8..0x080001DF），以回读为唯一结论、失败自动重试一次；DEVICE 改 STM32G474RE；clean 改 rmdir；mkdir 不再建 "-p"；cmd 的 echo 引号坑用 (echo a&echo b) 分组解决。实测：erase→flash 全量 81920 字节 + 回读一致退出码 0；内容已一致时 J-Link 报假错（Programming failed @ 0x08010534）但回读一致、仍退 0；make clean + make -j4 全量重编译通过（bin 81612 B）；烧完 RTT 抓到启动横幅证明板子在跑，DEMCR 已清。
未完成：STM32CubeProgrammer CLI 仍未安装（ST 官网下载要走下载/登录流程，抓不到直链；C: 只剩 3.40 GB）。
下一步：等用户决定是否装 CubeProgrammer；不装就维持现有 J-Link + 回读链路（flash.ps1 已留好分支）。

> 更早的会话条目已归档到 258 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：实测两件事：① 技能的 rtt 子命令跑完会在 DEMCR(0xE000EDFC) 留下 VC_CORERESET=0x01000000（清 0 → 跑一次 → 读回 0x01000000），所以观测完要补一次 J-Link 写 0 并 g；② NVM 页 0x0801F800 全 0xFF、SR=0xA0 是复位后的本来状态（firmware/App/Svc/nvram.c:173-177 的注释已记：PGAERR|PGSERR=0xA0 会让 HAL 直接返回错误，所以代码先 __HAL_FLASH_CLEAR_FLAG 再擦）。
未完成：make flash 与 tools/flash.ps1 两处便捷烧录路径未修；用户已明确要求把缺的依赖装上（STM32CubeProgrammer CLI）。
下一步：装 STM32CubeProgrammer/STM32CubeCLT 让 tools/flash.ps1 能跑，并修 Makefile 的 flash.jlink 生成（去引号 + DEVICE 改 STM32G474RE）

> 更早的会话条目已归档到 257 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-30
完成：本机把 stm32-dev 链路跑通：Cube G4 补包（HAL + CMSIS 两个 submodule）+ SVD（STM32G4xx/Include/STM32G474.svd）+ plain make 就地重建 build-app（text 81384/data 220/bss 14264、bin 81612 B）+ -device STM32G474RE 烧 81920 字节并 savebin 回读（只差已知 8 字节空洞）+ stm32-dev rtt 抓到启动横幅与实时 FRAME/MOTION/HOST 行（BUILD Sep 30 2026 11:12:37、nvram loaded=0、enc l=0 r=0 yaw=-4）。中途「烧不进去」的真相是我生成的 .jlink 把 loadfile 行拆成了两行，板子与探针无损，已清 DEMCR 并放程序跑。
未完成：make flash 在本机会挂死（flash.jlink 每行带引号 + DEVICE=RB 静默假成功）；tools/flash.ps1 依赖的 STM32CubeProgrammer CLI 本机没装。两处都没改，等用户定。
下一步：把 Makefile 生成 flash.jlink 的行去掉引号、DEVICE 改 STM32G474RE（或让 make flash 直接调 stm32-dev flash）

> 更早的会话条目已归档到 256 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：在真板上把 INA226 的全部寄存器读出来核对（两次复位完全一致）：ID=5449/2260（真芯片在线）、reg00=4127、reg01=0x7FFF（分流满量程钳位、每个采样都恰好 0x7FFF）、reg02 换算 12.023V、reg04/03 换算 16.384A/197W、reg06 CVRF=1 OVF=0。用 CVRF 计数反推转换周期，实测确认配置位序：bit15 RST / bit14:12 只读保留(写任何值都读回 100) / bit11:9 AVG / bit8:6 VBUSCT / bit5:3 VSHCT / bit2:0 MODE，写 0x0000 掉电停转换。0x4127 = 1 次平均 + 1.1ms + 1.1ms + 连续，2.26ms 一次更新；CAL=0x0400=1024 = 0.00512/(1mA*5mΩ) 正确。电池网表 pcb/Netlist_Schematic1_2026-09-29.tel 核对：R2.2+U5.10=IN+ 在源侧、R2.1+U5.8(VBUS)+U5.9(IN-) 在负载侧，与设计一致（不是接线错）。结论：配置与读码换算都对，是分流模拟输入顶到满量程。据此改代码：board.c ina226_init 的错注释"16 次平均"换成实测位序，电流检测页在满量程时给电流/功率加 '!' 前缀（out/oled_power.png 实拍：!16.38A / 12.02V / !197.03W），TEMP 探针（board.c ina226_temp_probe/probe2/cvrf + main_app.c 调用）全部删除；make text 81288/data 220/bss 14264、flash.ps1 回读 firstDiff=none、RTT err=0 busy=0 st=0、表盘 dump 正常。
未完成：分流输入为何顶格仍未定：可能是 R2 焊点/实际阻值远大于 5mΩ（0.2A 下要 0.41Ω 才钳满）、或 U5 分流输入级被此前的 5V 灌 3V3 事故打坏、或 sense 走线开路把 IN+ 拉到近地。需用户在通电跑动时用万用表直流档量 R2 两个焊盘（0.2A 时约 1mV）并对照电源自身电流表。
下一步：用户给出 R2 两端电压后判定是 R2/焊点还是 U5，再决定换件或改字模（超量程 的中文需重生成 kk_font_body）

> 更早的会话条目已归档到 255 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：用户 m04708：翻页时表盘那圈圆角框必须自己当合缝（原来 32px 视差 + 合缝取进来那页左边缘，出去的表盘框右半边被裁掉、看着像被黑线切开）。改成 kk_ui_nav.c 的 KK_UI_DrawScene 里两页都整屏滑 127px、合缝 = 进来那页的左边缘，且进来那页的 x = (127 - slide)，保证 boundary-1 恰好是表盘框右边缘列（1+off+125 = off+126）。用 TEMP 冻结过渡在 35%（duration=60000 + progress=Q12*35/100）在真板上分别抓正向 home→menu（out/oled_tr_fwd.png：框右边缘竖线在第 82 列完整可见、右边紧贴菜单）与反向 menu→home（out/oled_tr_rev.png：表盘框从左边完整滑入，右边缘第 43 列）；TEMP 全部还原、root_page 回 PAGE_SPLASH、make -j4 text 81108/data 220/bss 14264、tools/flash.ps1 回读一致 81336 字节 firstDiff=none、RTT out/rtt_bezel3.log 启动 10 行齐 + FRAME avg=690us max=3410us tx_done=16 err=0 busy=0 st=0。顺手修掉 app_ui.c draw_splash_page 里指向已删除的 kk_ui_draw_card() 的过期注释。
未完成：kk_ui_draw_card() 已整段删除，KK_UI_PAGE_PARALLAX 宏变成没人用（留着没删）；用户还没回答四个电源灯闪烁的排查问题
下一步：等用户看实屏反馈：若过渡速度/观感要调，改 KK_UI_PAGE_MS 或过渡时长；若要菜单页也带框，就是另一套需求

> 更早的会话条目已归档到 254 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：按用户 m04657 的澄清重做外框：外框归'页'所有，翻页时每页各自的圆角框跟着自己滑，菜单页静止时不带框。(1) kk_ui_nav.c 新增 static void kk_ui_draw_card(int16_t x_offset, int16_t clip_x, uint16_t clip_width)：按该页裁剪窗口画 OLED_DrawRFrame(1+x_offset, 1, 126U, 62U, KK_UI_BEZEL_RADIUS)，KK_UI_DrawScene 的正向/反向过渡分支里在两个 KK_UI_DrawPage 之后各调一次；原来那段'贴屏边常驻外框'（OLED_DrawRFrame(0,0,128,64,6)，不带 s_dx）已删除。(2) app_ui.c 的 draw_splash_page() 恢复自画外框 OLED_DrawRFrame((1+s_dx),1,126U,62U,6U)，与 kk_ui_draw_card 完全同几何（所以表盘页不会双重描边）。效果：表盘静止有框、菜单静止无框、主页->任务过渡时合缝处是进来那页的带圆角竖线（不再是黑边直切）。真板实拍（TEMP 把过渡冻结在 35%：start_transition 里 duration=60000、DrawScene 里 progress 强制 35%、app_ui_init 末尾 NavigateTo(PAGE_HOME)）：out/oled_tr_real.png + out/oled_tr_real.bin；菜单静止 out/oled_menu_noframe.png；表盘静止 out/oled_splash_now.png。生产固件 text 81168/data 220/bss 14264、bin 81488 字节，tools/flash.ps1 回读一致 firstDiff=none，RTT err=0 busy=0 st=0。
未完成：过渡结束那一帧进来那页的框会消失（静止不带框的必然结果）；用户若只要'一条带圆角的竖线'、不要上下两条横边线，把进来那页的 clip 收窄到 8px 即可（还没做）。
下一步：等用户看 out/oled_tr_real.png 确认合缝样式；若要窄边线版本按上面的做法改 kk_ui_draw_card 的裁剪宽度。

> 更早的会话条目已归档到 253 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：把屏幕外框改成 KK_UI 常驻圆角白框：firmware/Middlewares/kk_ui/src/kk_ui_nav.c 的 KK_UI_DrawScene 在画完两页（含过渡位移）之后、对话框/Toast 之前，无条件 OLED_DrawRFrame(0,0,KK_UI_SCREEN_WIDTH,KK_UI_SCREEN_HEIGHT,KK_UI_BEZEL_RADIUS=6U)，常量加在 firmware/Middlewares/kk_ui/src/kk_ui_internal.h:33；同时删掉 firmware/App/Src/app_ui.c draw_splash_page() 里原来那句 OLED_DrawRFrame((int16_t)(1+s_dx),1,126U,62U,6U)（它带 s_dx，翻页时会跟着内容滑走、屏沿露出黑边，用户 m04517 就是看到这个）。构建 text 81132/data 220/bss 14264，tools/flash.ps1 回读 firstDiff=none，RTT 启动 10 行齐、FRAME avg=677us max=3425us tx_done=16 err=0 busy=0 st=0。实屏取证：J-Link savebin 0x20001230 2048 后 node out/render_oled.mjs 渲染成 PNG —— out/oled_final.png（表盘+贴边白框）、out/oled_menu2.png（图标菜单+同一个框）、out/sim_tr_035.png（用 KK_UI 过渡公式把两页真实缓冲合成的翻页中途帧，框定住不动）。
未完成：翻页中途只有合成图、没有真板实拍（没法注入按键）；外框现在所有页面都有，用户若只想让表盘有需回退。
下一步：等用户看板确认外框观感；若嫌图标菜单底部标签贴到框上可把 KK_UI_BEZEL_RADIUS 或 home 标签 y 微调。

> 更早的会话条目已归档到 252 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：用户 m01348 四项全部落地并上板：①主页=上电表盘(PAGE_SPLASH 根页)，没有房子图标，左右滑进出 7 项图标菜单(PAGE_HOME)，菜单两端缝隙再滑回表盘；②模块测试标题'偏一下'真因是 draw_text() 内部已加 s_dx、调用处又加了一次(双重位移)，已改成传 (128-w)/2；③模块测试返回行画的左尖角与原生菜单 kk_ui_draw_enter_arrow(back=true) 逐像素一致；④四个只读 agent 全量审计(菜单绑定/绘制几何/输入退出/字模与 API)，修掉:循迹页 0/1 数字双重 s_dx、模块测试页缺滚动条、fmt_milli 截断改四舍五入、任务页 '左键返回' 画在 y=64 屏外(改 y=52)、item_count 改用 APP_UI_ITEM_COUNT、循迹速度写回 HP_BASE_SPEED、5 个导航 API 从内部头提到 kk_ui.h、端点夹住不回绕、进模块测试复位 s_test_sel、存盘期电机被挡、'正在保存'每拍重试且只有提示真的上屏才锁键、OLED 忙等待加 2.1s 上限。验证:make text 81120/data 220/bss 14264 → bin 81348 → flash 回读 firstDiff=none → RTT 启动 10 行齐全、FRAME err=0 busy=0 st=0、HOST 50Hz。
下一步：把三套标题版式/各页返回提示口径不一致、过渡期快速连点丢键、两张死图标(256B)报给用户拍板；灰度模块 SDK 到位后做循迹页标定。

## 2026-09-29
完成：m03944 三项需求（①存盘时先显示"正在保存"+锁按键、擦完显示"保存成功"②灰度接口 AUTO 优先串口→I2C→数字口，循迹页显示当前通道"数字/串口/I2C"③标定不改，保留 burst 32 帧/点）已全部编码并在真板逐项验证。验证手法：TEMP 复现固件（root_page=循迹页、NOTICE/DONE 拉宽、nvram_init 末尾 g_cfg.trace_speed=3333 制造真保存、合成中键）＋J-Link savebin 抓 OLED 缓冲＋Node 渲染 ASCII/PNG＋RTT 打点。结论：uwTick=2992ms 屏上"正在保存"且合成中键被吃掉（锁生效）、RTT `NVRAM: write ok=1 tick=3504`、uwTick=4561ms 屏上"保存成功"、状态线 BUSY 2901→写 3504→DONE 3513→IDLE 4707（与常量吻合）；Flash 记录 magic=0x31425848 HXB1/version=1/length=144/crc=0x97df9ef2/payload trace_speed=3333。顺带定位并修掉一个真 bug：**每次复位后第一次存盘必失败、5s 后重试才成功** —— 复位时 FLASH->SR 残留 PGAERR|PGSERR（0xA0），HAL_FLASHEx_Erase 开头的 FLASH_WaitForLastOperation 把它当成"上一次擦写失败"，连擦除都不做就返回 HAL_ERROR（打点 `WR: erase FAIL page_err=0 bank=1 page=63 faer=000000A0 sr=00000000 cr=40000000`，SR 已被 HAL 清掉、CR 只剩 LOCK）；修法：write_record() 解锁前 `__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS)`，擦除失败再清一次重擦一遍，擦后按 SR 错误位判断，仍保留 memcmp 回读校验；另把 NVRAM_NOTICE_MS 250U→600U（250ms 看不清）。最终固件 text 80952/data 220/bss 14264、bin 81180 已烧录并 RTT 复检（err=0、无异常行）。
未完成：循迹模块 SDK 未到（g_live.gray[] 仍用数字口电平换算）、编码器 TIM3/TIM4 未配、renwu()/TIM6/TIM7 未启动、五向键左 PC15/中 PC3 未实测、模块测试量电机两端电压无变化、host_link 三处待改（poll 传的是 UI 耗时而非主循环周期 / ina226_init 塞在 host_link_init 里 / 耦合）
下一步：等用户下一步指令（硬件话题已按 m01174 关闭）

> 更早的会话条目已归档到 250 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：修掉「从任务/设置切回主页时主页圆角边框变直线」：定位到中间件过渡合缝竖线（x=boundary 一整列 64px），删除后重编烧录；复现实验（过渡时长临时拉到 20s + 合成按键）配合设备显存 dump 取证：修前同场景 x=31 有贯穿 64px 竖线，修后 x=39 全空、只剩主页外框 x=40（圆角 40..46 收进）；临时改动已全部还原后重编（text 80520 / data 220 / bss 14256，bin 80748 字节 firstDiff=none），RTT err=0 busy=0 st=0
未完成：INA226 分流顶格（显示 16.384A，实测 R2 两端 0.21mV、电源 0.04A）待用户量 U5.9/U5.10 到 R2 的连通性
下一步：等用户实机确认过渡中圆角边框好看；如需要再加「超量程」显示 + 抓 0x01 裸分流值与 0x06 溢出标志

> 更早的会话条目已归档到 249 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：①设置菜单加「灰度接口」（自动/I2C/串口/数字，走 KK_UI_IntBinding 新增的 names 枚举文案字段）；②新增 App/Svc/nvram.c/.h 把 g_cfg 全部字段 + gray_mode + 陀螺仪轴向档 + 灰度标定（valid/pol/bad/blk/wht/thr/jit）存到最后一页 0x0801F800；③标定改成 burst 连采 32 帧；④字模重生成（341 字形，body 7774B）。验证：临时块逼出一次真擦写 → J-Link 回读 160B，magic/version/length/CRC 全对（主机端重算 CRC32=0x46bdb6de 一致），字段=现场值；用 CubeProgrammer 植入 trace_speed=3333 的记录 → 复位 → RTT 打印 nvram loaded=1 且设置页显示「循迹速度 3333」「灰度接口 自动」；全片擦除后 loaded=0 且第 63 页保持 0xFF（开机不写）。构建 text 80552/data 220/bss 14256（bin 80780B≈63% 128K），只有既有的 ld RWX 警告。
未完成：灰度模块仍不在手上，标定/三通道切换只能等实物；Flash 写路径只在单 bank（FLASH_OPTR bit22=0）下验证过
下一步：等用户实机确认设置项与存档；顺着用户下一条需求走

> 更早的会话条目已归档到 248 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：灰度模块驱动升级并上板：trace_cal.h/.c 整文件重写（双通道 I2C/UART 自动选择、32 帧去极值平均标定、每路有效性、进度上报、trace_cal_src 通道标志）；board.h/board.c 新增 board_uart4_read_byte()；app_ui.c 新增 draw_progress()、循迹页标题按标定状态分四种提示、数据源指示 I2C/URT/DIG、无效通道画 '!'。构建 text 77876 / data 184 / bss 13792，bin 78068 字节 = 128KB 的 59.6%，flash.ps1 回读 firstDiff=none；RTT 7s：主循环仍约 235Hz、err=0、HOST 50Hz、无 HardFault。显存 dump（J-Link savebin oled_buffers 0x20001030 0x800 + Node 渲染 180 度翻转）确认：循迹页显示「循迹测试 DIG 中键标定」+8 柱+底行 0/1；强制采样态显示「全黑按中键 0/32」不压标题。
未完成：模块实物接上后要实测：I2C 与 UART 两条通道哪条通、黑白标定后的柱高与 '!' 是否合理。
下一步：等用户插上灰度模块后按中键标定一次，回报屏幕右上角显示 I2C 还是 URT。

> 更早的会话条目已归档到 247 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：固化两条调试手法：①显存 dump —— J-Link Commander（device/si SWD/speed 4000/connect/h/savebin out/vram.bin <oled_buffers> 800/w4 E000EDFC 0/go/qc，脚本 out/dump_vram.jlink），oled_buffers 地址每次构建后用 arm-none-eabi-nm 取；②Node 把 1024B/缓冲渲染成 128x64 PNG 并 180 度翻转（APP_UI_DISPLAY_ROT180）。踩坑：dump 时 h 会停 CPU，可能停在一帧画到一半的那个缓冲上（oled_buffers[2]），这时要改看另一个缓冲。
下一步：无需后续。

## 2026-09-29
完成：长按环"卡"的第二半修掉并上板：main_app.c:166 的 40ms 改成 20ms。make → text 76608/data 184/bss 13296，tools/flash.ps1 回读 76800 字节 firstDiff=none；RTT 静止态 n=239Hz、update avg≈675us、oled tx_done=16/s、err=0（静止时脏页差分几乎不占总线，说明 20ms 闸门本身不产生额外流量）。
未完成：等用户按住方向键实机确认流畅度；若还不够，可调 KK_UI_PAGE_MS_FAST(140ms)
下一步：用户实机试长按绕环

> 更早的会话条目已归档到 245 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：解释"263~283Hz"口径：那是主循环圈数/秒（DWT 计，FRAME 行的 n），不是屏幕帧率。每圈 ~3.5ms = ~1.5ms 干活 + HAL_Delay(2)；**HAL_Delay 是空转等 SysTick，不是真睡眠**（早前说"睡眠"不准确）。屏幕真实上限 = KK_UI_FRAME_INTERVAL_MS 20ms(50fps) + I2C 搬运（整屏 8 页 1024B@400kHz≈23ms，脏页差分只搬变化的页/列）。

## 2026-09-29
完成：新增电流检测页并上板验证：app_ui.h 加 PAGE_CUSTOM_POWER=15/CUSTOM_POWER=6（PAGE_COUNT 15、CUSTOM_COUNT 7）；app_ui.c 加路由、首页环第 6 项「电流」+ 闪电图标 kk_icon_power(32x32 XBM，kk_icons.c)、draw_power_page()（标签 x=4、值右对齐到 x=122、fmt_milli 千分位单位切换）、OnTick 200ms 采三个量；board.c/.h 加 ina226_read_power_mw()。烧录 76800 字节 firstDiff=none，RTT err=0/n=263Hz。
未完成：INA226 真值要等 12V 主线通电才能看（现在只有 3.3V：读出来 406mV/1mA/0mW）；顺带确认 P=U*I 数量级对不对
下一步：12V 上电后进「电流」页对着万用表比一下电压/电流

## 2026-09-29 · 固件-验证
完成：掌握并用『设备显存 dump』验证新页面：J-Link Commander `h` + `savebin out/vram.bin <oled_buffers地址> 800`（本 build 0x20000E40，地址随静态量变化，先 `arm-none-eabi-nm | grep oled_buffers`）+ 复位 + 180° 翻转渲染 PNG（out/power_page.png、out/home_page.png）。抓非根页的办法：临时把 `.root_page` 指过去；抓首页环里第 N 项则再临时把 kk_ui_nav.c:23 的 `state->selected = 0U` 改成 N（两项都要改回来）。

## 2026-09-29 · 固件-构建
完成：构建/烧录口径：Makefile 在 firmware/，必须在 `D:\Today\haixinbei\firmware` 里跑 make（根目录直接 make 会报 No targets specified and no makefile found）；烧录用仓库根的 `& .\tools\flash.ps1`（走 CubeProgrammer CLI + J-Link 回读逐字节比对，不用 loadbin）。Flash 占用 76608+184=76792 / bin 76800 ≈ 58.6%（字模扩容 +3.6KB 是主因）。

> 更早的会话条目已归档到 241 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：修复长按环"卡一下"并烧录验证（回读 72280 字节 firstDiff=none，复位 IPSR/CFSR 干净）；量了帧率与占用：主循环 263~283 Hz（含 HAL_Delay(2)），app_ui_tick 空载 66~115 us、动画中 avg 1.2~1.4 ms 峰值 12.6 ms，OLED 过渡时约 220 次页写/s（≈27 整屏/s，400kHz 全屏 1024B≈23ms 是真正的帧率上限），err=0/st=0；Flash text 72088 + data 184 = 72272，bin 72280 = 128KB 的 44.8%，RAM data+bss 13456 B / 96KB
下一步：等用户实机确认长按节奏是否连续加速；未验证项仍是左/中键实测、编码器手转、模块测试两端电压

## 2026-09-29 · KK_UI 界面
完成：长按环补上表盘 + 陀螺仪方框改实体渲染 + 默认档改 M3 + 去掉表盘停顿时长。verify: make text 71924/data 184/bss 13272；tools/flash.ps1 回读 72116 字节 firstDiff=none；Flash 后 IPSR=0/CFSR=HFSR=0（无假故障）。另外新学到的验证手段：J-Link savebin 读显存 0x20000E28（2×1024B，页式 bit0=页顶），Node 渲染成 PNG 就能在不上车的情况下看真屏内容——已据此确认主页『开始游戏』可见、陀螺仪页网点方框正常。
未完成：等用户实机确认：长按环的节奏是否恢复『越来越快』、网点填充的疏密是否合意。
下一步：用户反馈后：若要调网点疏密改 gy_fill_quad 的隔行条件；其余待办（循迹模块实物、编码器实测、renwu/TIM6/TIM7）不变。

## 2026-09-29 · 固件移植
完成：三层串级（编码器TIM3/TIM4 + speed_control速度环 + motion调度 + gyro_control角度环重写 + PID_tracing循迹环）编译通过并真正烧进板子：text 68992/data 180/bss 12956。RTT 实测 app 在跑：FRAME(OLED avg 300~1500us/max 7ms) / MOTION(mode=0 enc 0,0) / HOST(tx 50Hz) 每秒各一行，示波式确认 g_oled_tx_done 递增、g_oled_tx_err=0、Yaw≈-25(度)。J-Link 读寄存器确认编码器接口已配置正确：TIM3/TIM4 CR1=1(CEN)、SMCR=3(TI12 编码器模式)、CCMR1=0xA1A1(IC1F/IC2F=10 滤波,CC1S/CC2S=01)、CCER=0x11(双通道使能)、CNT=0(轮子没转)。
未完成：编码器 counts 要手转轮子才能验证（CNT 仍为 0）；三个界面改动（主页「开始游戏」/陀螺仪方块转向/返回箭头）待用户肉眼确认。
下一步：让用户手转左右轮各一次，读 out/rtt.log 的 MOTION 行确认 enc l/r 正负与大小。

> 更早的会话条目已归档到 238 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：PID 菜单按用户口径重构成三层串级：速度环/角度环/循迹环三个子页（firmware/App/Src/app_ui.c 的 k_pid_items/k_pid_speed_items/k_pid_angle_items/k_pid_trace_items，k_menu_pages 6 项，k_int_bindings 13 项，menu_page_count=6/int_binding_count=13）；中间件给 KK_UI_IntBinding 加 uint8_t decimals 并新增 KK_UI_FormatIntScaled()，菜单行与编辑框都改用它，屏幕显示真小数（3.75 而不是 375）；参数表新增 HP_SPEED_KP/KI/KD/ILIMIT 0x0601-0x0604。验证：firmware 下 make 成功（text 64124/data 164/bss 12428）；Node 复算参数表指纹 fw 与 host 双方 n=27 crc=0x661D 且顺序值逐项相同；python tools/host/tests/run_all.py 101 项全过；用 out/flash_app.jlink 烧录 Verify successful。
未完成：三层串级的控制代码还没写：pid.c 仍直接输出 PWM、gyro_control.c 仍是保持启动角；速度环需要的 TIM3/TIM4 编码器模式与双轮 PI 完全不存在（新板与旧板 encoder.c 都是整份注释）。
下一步：接用户新要求：写编码器+速度环+角度环/循迹环串级、主页两箭头间加「开始游戏」、修陀螺仪页方框转动方向。

## 2026-09-29 · KK_UI 界面
完成：「按键蜂鸣」原来只在任务 A/B 按中键一个分支里响，其它页面全不响（用户反馈"开了也不响"）。现在中间件 firmware/Middlewares/kk_ui/src/kk_ui.c:535 有通用钩子 `__attribute__((weak)) void KK_UI_OnKeyPress(uint8_t keys, uint32_t now)`，在 kk_ui_process_input 去抖后 :590 对每一次新的物理按下回调（长按重复不回调、过渡中按下的键也回调）；应用在 firmware/App/Src/app_ui.c:267 实现它→总使能+按键蜂鸣都开时 beep_ms(30)。beep_ms 同时从 HAL_Delay 改成非阻塞（app_ui.c:249-261 + app_ui_tick 顶部 :684 的 beep_poll()）。text 62,416 烧录 Verify successful。蜂鸣器是**有源 5V**、PD2 高有效、经 Q1(S8050) 低边驱动、挂 +5V 轨 → 只接调试口 3.3V 时 VCC5V≈2.7V 不会响；上板结果用户尚未回报。
下一步：等用户回报：模块测试→蜂鸣器 是否响 + VCC5V 实测值，分清固件/5V 轨。

## 2026-09-29 · KK_UI 界面
完成：陀螺仪页按用户要求重排：三维线框移到屏幕左半边（中心 GY_CX=27/GY_CY=39、GY_SC=9），右侧改成三行全名「横滚/俯仰/偏航」，数值用 fmt_x10_compact 右对齐到 x=126（GY_TX=56/GY_TRIGHT=126，行 y=18/32/46）；无信号时右侧居中显示「无信号」。线框尺寸不是拍的：正方体投影后的外接半径最坏 = 2.42*GY_SC 像素（对角顶点朝向观察者时），旧值 GY_SC=11/中心(64,33) → 26px，上顶标题栏(y=7)、下压角度文字(y=59)，这就是用户说的「方框太大超出去」；GY_SC=9 → 22px，最坏跨度 x 6..48 / y 18..60 刚好落在标题栏以下。用新写的查字工具确认「横滚俯仰偏航无信号」都在 kk_font_body 的 187 字形子集里，**本轮不需要重新生成字模**。text 62,348 / data 132 / bss 12,404，已烧录 Verify successful。
下一步：上板目视陀螺仪页：线框是否越界、三行标签与数值是否重叠。

> 更早的会话条目已归档到 235 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：表盘↔图标菜单做成 6 位环并修好动画方向：kk_ui_nav.c 重构出 kk_ui_navigate_push/pop(多一个 bool reverse)，新增 KK_UI_NavigateToDirected/KK_UI_NavigateBackDirected，并新增一次性入口选中项 KK_UI_SetEntrySelection(index)（在 push 里消费，只对 KK_UI_PAGE_HOME 且 index<item_count 生效）；kk_ui_home.c 的 kk_ui_home_delta 改纯差值、kk_ui_home_move 两端夹住；app_ui.c 的 PAGE_SPLASH 分支按左=SetEntrySelection(最后一项)+NavigateToDirected(reverse=true)，按右/中=SetEntrySelection(0)+NavigateTo。构建 text 61668 / data 132 / bss 12404，用 out/flash_app.jlink（内含 device STM32G474RB，缺了会 'Writing target memory failed'）烧录，Verify successful。
未完成：等用户上板确认'从表盘按左落点=陀螺仪'这一步；模块测试页的电机电压疑案（先量 PC6/PC7 占空比）；循迹 SDK 未到、编码器 TIM3/TIM4 未配、renwu()/TIM6/TIM7 未启动。
下一步：用户上板按左键进菜单，确认落点是'陀螺仪'且左边是'模块测试'、右边空。

> 更早的会话条目已归档到 234 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：①主页改成智能手表式：root_page=PAGE_SPLASH（表盘），表盘按左/右/中 → KK_UI_NavigateTo(PAGE_HOME)；图标菜单去掉『主页』那格回到 5 项，在最左/最右项继续往左/右由 app 侧 KK_UI_HomeOnMove 钩子调 KK_UI_NavigateBack 滑回表盘（前进/回退成对，导航栈恒 1 层）。这同时解掉了用户报的『卡到主页+亮一下白框』：旧代码每次回家都 NavigateTo 压栈不弹，8 层撞 KK_UI_NAV_DEPTH 后 kk_ui_nav.c:66-71 置 reject_flash=2。②自定义页补齐过渡水平位移 s_dx（原来是 (void)x_offset，只裁不平移 → 标题被裁剪边逐段露出，看着像『偏一下』）。③模块测试返回图标由朝上的 ︿ 改成与 kk_ui_menu.c:275-280 原生返回行逐像素相同的左尖角。④4 个只读 subagent 全量审计：路由/菜单/绑定/计数全部自洽；清掉孤儿陀螺仪信息页(PAGE_INFO_GYRO 无入口，连带 k_info_pages[0] 与每帧 snprintf 的 app_ui_refresh_gyro_rows)、死配置 k_confirms、死宏 EV_BUZZER_ON/OFF、死字段 g_live.speed_l/r、死参数 draw_task_page(task_id)，页面 ID 顺势重编号 1..10。⑤发现并修复真缺字：kk_font_body 原本 186 字形里没有『右』U+53F3，屏上『按左右键』只会显示『按左键』(KK_OLED 缺字静默跳过)；已按 kk-oled-font 技能重生成 wenquanyi_9pt/12px 187 字形/3698B。已烧录，text 61,460 / bss 12,404 / Verify successful。
未完成：电机测试量两端电压无变化（怀疑 AT8236 VM=0，需实测 PC6/PC7 占空比）；五向键左 PC15 仍未单独实测；编码器 TIM3/TIM4 未配置；renwu()/TIM6/TIM7 未启动(等 12V/7.4V)；循迹 SDK 未到；host_link 三处待改。
下一步：上板目视确认：上电停在表盘、左/右/中任一键滑进图标菜单、菜单头尾『缝』滑回表盘时整屏滑动且标题不蹦、模块测试返回行是 '<'。若电机测试仍量不到两边电压，先量 MCU 侧 PC6/PC7 的 PWM，判断是固件还是 AT8236 的 VM（7.4V）没上电。

> 更早的会话条目已归档到 233 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29 · KK_UI 界面
完成：按 m01247 改完并已烧录（text 61,624 / data 132 / bss 12,508）：
1) 陀螺仪页小车模型换回正方体（gy_box 140,140,140，删掉 4 个轮子和 wheel[]）。
2) 菜单「返回」标识：删掉应用自加的 11→10 / 3→2 两项返回，消除重复行与错误的 ">"。
3) 主页图标顺序：任务 提到第一位。
4) 新增 PAGE_SPLASH 上电主页 + root_page 改指它 + 按下键/中键进菜单 + 图标菜单按上回主页（在 kk_ui_home.c 加了 weak 钩子 KK_UI_HomeOnUp(selected)）。
5) 模块测试页电机项右侧显示实际占空比（s_motor_echo：按住显示 1500，松开 0）。
另修：out/flash_app.jlink 少了 device STM32G474RB，导致烧录报 "Writing target memory failed"。
未完成：电机测试量两端电压无变化（未定因，已加屏上占空比回读用于分离固件/硬件）；主页文案要中文需重生成字模。
下一步：让用户选「模块测试→电机」按住中键，看右侧是否从 0 变 1500；再确认万用表量的是 PC6/PC7（U2.38/39）还是 AT8236 输出端，以及电机电源 VM 是否已接。

## 2026-09-29
完成：**按用户 m01174 反馈改完并烧录（61,400B，逐字节校验 OK）。**

**1. 字模缺字根因 + 重新生成**：用户反馈"模块测试只能看见测试"——根因是 **KK_OLED 的 u8g2 静态字体子集里没有「模」「块」等字形，`OLED_DrawUTF8()` 遇到缺字直接什么都不画**（不报错、不画方框）。这不是排版问题。已按 **kk-oled-font 技能**流程（先读 https://ledfont.botelvdong.com/agent-api.md 作为唯一权威，再 `GET /api/v1/compressed-fonts` 查实时目录）重新生成两套字模：
- `kk_font_body`：`wenquanyi_9pt` size 12 → **186 字形 / 3669 字节**
- `kk_font_home`：`wenquanyi_11pt` size 15 → **108 字形 / 1850 字节**
- 字符集 = 全部可打印 ASCII(0x20–0x7E) + UI 实际用到的中文（设置/任务/循迹测试/模块测试/陀螺仪/关于/返回/取消/确定/提示/开关/速度/按键蜂鸣/过点蜂鸣/循迹反转/闭环/蜂鸣器/电机/编码器/响一下/按住中键执行/左偏航/横俯滚仰/状态/无信号/待机运行/在线离/主控频显示固件字体 等）。
- API 目录里的可用字体 id：wenquanyi_12pt(16)、wenquanyi_11pt(15)、wenquanyi_13px(14)、wenquanyi_10pt(13)、wenquanyi_9pt(12)、WenQuanDengKuanWeiMiHei(ttf 8-64)、Alibaba-PuHuiTi-Regular(ttf 8-64)、boutique_bitmap_7x7(8)、boutique_bitmap_9x9(10)、boutique_bitmap_9x9_bold(10)。**字模版权归生成服务，不随 KK_OLED 的 MIT 许可。**

**2. 陀螺仪页：正方体 → 小车模型**（`draw_gyro_page()`）。新增通用 `static void gy_box(m00,m01,m02,m10,m11,m12,cx,cy,cz,sx,sy,sz)` 画长方体线框（8 角点用同一个旋转矩阵投影，坐标单位 1/100 单位，**1 单位 = GY_SC 像素**）；车身 `gy_box(..., 0,0,0, 140,80,30)`，4 个轮子在 `(±85,±85,-35)` 各画一个 6×4 实心块。车头朝 +X。

**3. 角度正方向统一到一个函数**：新增 `static void gyro_angles(int32_t *roll, int32_t *pitch, int32_t *yaw)` —— `*roll=-g_live.pitch_x10; *pitch=-g_live.roll_x10; *yaw=g_live.yaw_x10`，符合用户约定「向左偏=横滚增大、向下低头=俯仰增大、向左转=偏航增大」。**轴序交换+取反只在这一个地方**，陀螺仪页/任务页/信息页三处显示都走它，以后要翻只改这里。

**4.** 顺手修掉 `snprintf(tail, sizeof(tail), "")` 的 `-Wformat-zero-length` 告警（改 `tail[0]=0`）。

**工程经验（值得记）**：KK_OLED 用的是**静态字符子集字模**，改文案时如果引入了子集外的汉字，屏幕会**静默地少画几个字**（不是乱码、不是方框）。以后改 UI 文案必须同步重新生成字模。
未完成：用户尚未反馈小车模型/横滚俯仰正负是否合意；循迹模块 SDK 未到（`g_live.gray[]` 仍是 0/1 电平换算）；编码器 TIM3/TIM4 模式未配置；`renwu()`/TIM6/TIM7 未启动；五向键左/中未实测。
下一步：等用户目视确认小车模型与角度方向；SDK 到手后接真灰度。

> 更早的会话条目已归档到 231 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：**两件事：① 用完整版网表查清了电源器件（含悬而未决的 D4）；② 按用户 m01126 的反馈重做 UI 并烧录。**

**① 完整网表（40744B，带立创编号/厂商/描述）核对结果** —— 之前只有 7859B 的简化版，查不到器件型号：
- **D4 = BZT52C5V1-TP（立创 C78728，MCC）＝ 5.1V 齐纳**，接在 VCC5V 与 GND 之间。
  ⚠️ **5.1V 偏低**：5V 轨额定 5.0V，齐纳击穿 ±5% = 4.85~5.36V，下限落点会在**正常工作时就开始导通**，把 5V 轨往下拉并发热；**应当用 5.6V**（与当初建议一致）。⚠️ 方向必须阴极接 5V、阳极接 GND，装反 = 正向导通把 5V 拉到 0.7V。
- 其余确认：U9 = **RT9013-33GB**(C47773，输入上限 5.5V)；D1 = BZT52C15(15V 齐纳，12V 钳位)；D2 = SMF15CA(双向 TVS 15V)；D3 = 1N4148W(蜂鸣器续流)；D5 = SS54(40V 5A 肖特基)；Q3 = PJQ4465AP(60V 15A P-MOS 防反接)；F1 = SMD2920-500-24(5A 自恢复保险丝)；R2 = 5mΩ 1W 1206 采样；R30 = 3296W-50kΩ 可调(7.4V 调压)；U7/U8 = SP1N28。

**② UI 按用户反馈全部改完并烧录（60,692B，约 46% Flash，逐字节校验 OK）**：
1. **修掉负余数格式化 bug**：`-123/10=-12`、`-123%%10=-3` → 直接拼小数得到 "-12.-3"（用户看到的 `-12.-4`）。新增 `fmt_x10()` 先取绝对值再拼符号，**横滚/俯仰/偏航现在全部带 1 位小数**。
2. **横滚/俯仰按用户反馈交换显示**（`draw_gyro_page()` 与 `app_ui_refresh_gyro_rows()` 两处都改）。
3. **陀螺仪图变大、字变小**：立方体 `GY_CX 36 / GY_CY 37 / GY_SC 15`（原 cx30/cy40/sc10，半宽 17→26px），右侧文字列 x=68 且改用 12px 字体（原 15px）。
4. **「总测试」改名「模块测试」**（首页 + 页标题）。
5. **修"选中框盖住文字"**：根因是先画实心 RBox 再写字、写字的 CLEAR 背景把框擦了。改为 **先清行 → 写字 → 最后画空心 Frame**。
6. **蜂鸣器改为"按一下响一下"**（`beep_ms(60)`），不再常响。
7. **电机改为按住中键转、松开停**：在 `KK_UI_CustomOnTick()` 里轮询 `keys_raw()` 的物理中键位(0x10)，并加 `s_motor_on` 边沿检测；`KK_UI_CustomOnLeave()` 强制 `Load(0,0)` 兜底。
8. **编码器项直接显示数值**（当前 0，TIM3/TIM4 编码器模式仍未配置）。
9. **退出方式统一**（用户明确要求"不要混着来"）：设置菜单 11 项、任务菜单 3 项，**末尾都有「返回」**；模块测试页 **4 行（蜂鸣器/电机/编码器/返回）**，上/下选、中键执行；**无选项框的页（循迹/任务A/B/陀螺仪）用左键返回**（左键在 `app_ui_read_input()` 里本就被归一化成 UP）。
10. 顺手修掉隐藏 bug：陀螺仪信息页里"横滚"和"偏航"**显示了同一个 yaw 值**，现在横滚/俯仰/偏航各自正确（`k_gyro_rows` 由 5 行缩为 4 行）。
11. 删掉已无人调用的 `draw_status_page()`（消除编译告警）。

**新增设置项（App/Inc/app_ui.h 的 app_settings_t）**：`bool beep_key`（按键时响）、`bool beep_marker`（任务每过一个点响一下）——用户要求"设置关于蜂鸣器有两个选项"。默认 `beep_key=false, beep_marker=true`。
未完成：⚠️ **D4（5.1V 齐纳）的容值和方向待用户确认**；用户尚未目视确认新陀螺仪页/温度计/模块测试页；循迹模块 SDK 仍未到（`g_live.gray[]` 仍是 0/1 电平换算）；编码器模式仍未配置；`renwu()`/TIM6/TIM7 未启动。
下一步：用户反馈新 UI 观感；确认 D4 是否需要换 5.6V 并核对方向；等 SDK 接真灰度。

> 更早的会话条目已归档到 230 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：**按用户新需求重做 UI**（用户原话要求见末段）。
1. **陀螺仪数据通了**：用户确认"设置里的陀螺仪有偏航角的数据" ✅ —— 根因是 `main_app.c` **从来没初始化 USART3**；已加 `board_uart_jy61_init()` + `board_jy61_start_rx()`，主循环把字节喂给旧上层 `jy61p_ReceiveData()`，并用 `gyro_bytes` 置 `g_live.gyro_ok`。
2. **修好"进陀螺仪页黑屏"**：根因是我只加了 `PAGE_CUSTOM_GYRO` 的路由，**没写绘制函数也没在分发里加分支**。现在 `draw_gyro_page()` 用 **Q1000 定点**（自带 91 点 sin 表，**故意不引 math.h/libm**，因为工程是 nano.specs）把单位立方体按 roll/pitch/yaw 旋转后正交投影，右边一列：偏航(小字标题)+**大号偏航值**(15px 字体)+「横x 俯x」一行。
3. **循迹测试页改成用户要的"温度计"**：8 条竖条，每条约 `TC_W=12/TC_STEP=15/TC_H=44`，**灰度 0..255 线性映射成 0..44 像素从底部往上填**（越白条越高）—— 数据取自新加的 `g_live.gray[8]`。
4. **新增总测试页**：蜂鸣器 / 电机 / 编码器 三项，**下键换选中、中键执行**（蜂鸣器切换 ON/OFF；电机 `Load(1500,1500)` 转 0.5s 再停；编码器显示"未接"）。
5. **清掉重复项**：设置菜单里的「陀螺仪」「总测试」删除，任务二级菜单里的「总测试」删除（只剩任务A/任务B）—— 它们首页已经有了。
6. **字体/图标**：重新生成 12px 字模（185 字形，补 总测试/电机/编码器/传感器/灰度/姿态 等）与 15px 大字体（59 字形，**补了 + - . :** 才能显示大号带符号的偏航值）；新画 2 个 32×32 图标 `kk_icon_gyro`/`kk_icon_test`。
7. **修掉 Makefile 缺头文件依赖跟踪的真 bug**：原来改了 `.h` 不触发重编，`make` 报 "Nothing to be done" 却链接旧 `.o`（这次改 `app_ui.h` 的页面宏就这么混过去，只因宏值恰好同数字）。已加 `-MMD -MP` 到 `CFLAGS` + 末尾 `-include $(wildcard $(BUILD)/*.d)`。
8. clean 重编 + 烧录 + 逐字节校验通过，Flash **60,496B / 128KB（约 46%）**；RTT 正常（`FRAME: update avg=752~913us`、`oled tx_done` 每秒增约 390）。
**遗留**：`draw_status_page()` 现在没人调用（编译器告警 unused），可删；用户还没确认新页面的观感。
未完成：用户找的**循迹模块 SDK 未到**（灰度数据目前仍是 `tracing_data[]` 的 8 位数字电平，不是真灰度）；总测试的编码器项未实现（PB4–PB7 的 TIM3/TIM4 编码器模式从来没配过，见 `docs/控制算法升级方案.md`）；`renwu()`/TIM6/TIM7 仍未启动。
下一步：用户看新页面观感并反馈；SDK 到手后把灰度接进 `g_live.gray[8]`；补编码器测速。

> 更早的会话条目已归档到 229 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：**新网表核对 + 屏幕翻转 + 修掉"自定义页出不来"的 bug。**
1. **网表 `Netlist_Schematic1_2026-09-29.tel`（两份都核过：7859B 与 40744B 版）**：所有与固件相关的网**逐条一致**（I2C1=U2.51/U2.62、电机 U2.38–41、舵机 U2.42–45、编码器 U2.57–60、五向键 U2.2/3/4/10/11、LPUART1=U2.8/U2.9、UART4=U2.52/53、USART3=U2.30/33、BUZZER=U2.55、huidu1–8=U2.12/13/14/17–21、SWD=U2.49/50、BOOT0=U2.61、VCC3V3 成员）→ **`board.h` 无需改动**。新增 **D4**（VCC5V↔GND 的 5V 保护，器件类型待用户确认）；OLED 外围改成 C41/C44=1µF（飞电容）、C45=4.7µF（VCC）、C31=4.7µF（VCOMH），值都对。
2. **审查了另一个会话新增的 `App/Svc/host_link.c/.h`**（上位机协议，LPUART1）：实现干净（SOF+CRC16-CCITT+滑窗重同步+长度校验+参数范围校验+SET 回读）。发现 3 处待改：`host_link_poll(cyc_last/170u)` 传的是 UI 刷新耗时而非主循环周期；`ina226_init()` 被塞进 `host_link_init()`；`renwu()`/TIM6/TIM7 仍未启动。
3. **屏幕翻转 + 按键跟转**：新增 `APP_UI_DISPLAY_ROT180`（app_ui.h），`app_ui_setup_display()` 紧跟 `OLED_Init()` 调 `OLED_SetRotation(OLED_ROTATION_180)`；`app_ui_read_input()` 里上↔下、左↔右互换。
4. **修 bug：自定义页进去出不来**（用户报告"系统状态"页卡死、按键只改屏上的二进制）。根因是我漏了 KK_UI 自定义页**没有内置返回**；已在 `KK_UI_CustomOnInput()` 里加 **按【上】= `KK_UI_CustomRequestClose()`**，并在标题栏右上角画 `<` 提示。
5. 每次改动都 **`make` + J-Link 烧录 + 逐字节校验**通过，实机 RTT 正常（`FRAME: update avg=118us`、`HOST: tx=..  hz=50`）。当前 Flash **58,280B / 128KB（约 44%）**。
未完成：D4 器件类型未确认；`renwu()`/TIM6/TIM7 未启动（有意）；五向键左(PC15)/中(PC3) 从未实测过；5V/12V 侧功能未测；屏幕翻转方向与按键方向待用户目视确认。
下一步：用户确认屏幕方向/按键方向；确认 D4；等电源稳定后启动周期任务与 renwu()，上车联调。

> 更早的会话条目已归档到 228 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-29
完成：**板子复活 + 新网表核对 + 主程序烧录成功。**

1. **J-Link 恢复正常**：VTref=3.29V、读到 DEVID 0x20036469 —— **3V3–GND 短路已修好、MCU 没被打死**。
2. **新网表 `pcb/Netlist_Schematic1_2026-09-29.tel`（7859B，198 行，另一种导出格式，无 LCSC 属性）已核对**：所有与固件相关的网**与 v6 完全一致**（I2C1=U2.51/U2.62、电机 U2.38–41、舵机 U2.42–45、编码器 U2.57–60、五向键 U2.2/3/4/10/11、LPUART1=U2.8/U2.9、UART4=U2.52/53、USART3=U2.30/33、BUZZER=U2.55(PD2)、huidu1–8=U2.12/13/14/17/18/19/20/21、SWD=U2.49/50、BOOT0=U2.61+R3 10k）→ **`board.h` 无需改动**。
   变化只在电源/OLED 外围：**C39/C40 改名为 C41/C44（1µF 飞电容）**、**C45=4.7µF 接 U16.28(VCC)**、**C31=4.7µF 接 U16.27(VCOMH)**，值都对；**新增 D4（SOD-123）接在 VCC5V 与 GND 之间**（用户加的 5V 保护，器件类型待确认）。
3. **发现工作区被另一个会话（"嵌入式固件工程师"）改过**：新增 `App/Svc/host_link.c/.h`（16.9KB，上位机协议），并改了 `app_ui.c`/`board.c`/`board.h`/`main_app.c`/`stm32g4xx_it.c`/`stm32g4xx_hal_conf.h`（09-24）。**审查结论：协议实现干净** —— SOF 同步 + CRC16-CCITT + 滑窗重同步 + 长度合法性检查 + 参数范围校验 + SET 后回读；`board_dl_init()` 把 LPUART1 配成 **PC1=TX/PC0=RX、AF8**，与网表的网名互换一致（代码按实际功能配）。
4. **主程序已烧录并逐字节校验通过**：`build-app/haixin-app.bin`，58,008B text（约 45% Flash）。实机 RTT：`CLK 170MHz` / `OLED init OK` / `KK_UI init OK` / `FRAME: update avg=118us` / `HOST: rx=0 tx=284 crc=0 resync=0 hz=50`。

**审查中发现的问题（已告知用户）**：① `host_link_poll(cyc_last/170u)` 传的是**UI 刷新耗时**而不是主循环周期，遥测里那个 loop_us 字段会偏小；② `ina226_init()` 被塞进 `host_link_init()`，耦合了通信模块与外设初始化；③ `renwu()` 与 TIM6/TIM7 周期任务**仍未启动**（有意，等 12V/7.4V）；④ D4 若是普通二极管则挡不住过冲（U9 仍会被打）。
未完成：D4 器件类型待确认；renwu/周期任务未启动；五向键左(PC15)/中(PC3) 仍未实测；5V/12V 侧功能未测。
下一步：确认 D4 类型；等电源稳定后启动 TIM6/TIM7 与 renwu()，上车联调。

## 2026-09-27
完成：对比用户给的两份 0.96" 30P OLED 引脚表：**不通用**，30 脚里 20 脚不同。图2（Pin Assignment 表）= 本板 U16 那款 **X096-2864KSWPG01-H30** 的定义，与 固件接口规格.md 3.7 + v6 网表逐脚吻合（2/3=C39、4/5=C40、11=BS1→3V3、12=BS2→GND、18=D0→SCL、19/20 短接→SDA、28 只接电容）；图1 是常见「通用 SSD1306／立创EDA 符号」定义（2–5 脚 C1/C2 顺序相反、10=BS0、29=VLSS、16/17/21–25 为 NC）。另按网表核实：板上 100Ω **只有两处** —— 电机输入串阻 R6/R9/R10/R11（配 100k 下拉）与编码器输入串阻 R29/R31/R32/R33（配 100nF 到地 + 10k 上拉 5V）；两处都属信号串阻，换 120Ω 无损（编码器那处 RC 截止 15.9k→13.3kHz，而编码器单相约 1.2~2.5kHz，仍有 5 倍以上裕量）。
未完成：**主板选型与设计.md:111 与网表不一致**：文档写舵机「信号串 100Ω」，但 v6 网表里 TIM1_CH1..CH4_SERVO1..4 各只有两个节点（U2.42/43/44/45 ↔ U11.1/4/7/10），**实际是直连、没有任何串阻**。待用户确认是有意省略还是漏画。
下一步：用户确认后：若有意省略就把 主板选型与设计.md:111 改成「信号直连」，若属漏画则列入下一版改板清单。

## 2026-09-24
完成：**做固件侧上位机链路（LPUART1），编译通过；硬件验证待接板**。

**先查手册确认复用编号**（不靠记忆）：DS12288 Rev6 第 75 页 Table 13 → **PC0 = AF8 = LPUART1_RX、PC1 = AF8 = LPUART1_TX** ✓（规格书只写了功能名没写 AF）。

**新增/改动**：
- `App/Svc/host_link.c/.h`（新建 506 行）：CRC16/CCITT-FALSE（与上位机同参数，校验值 0x29B1）、帧收发、**23 项参数表**（id/默认值/范围与 `protocol.py` 逐项对应）、HELLO/PARAM_GET/SET/SAVE、命令分发、遥测打包、收帧状态机（CRC 错只前移 1 字节重同步）。对外提供 `host_param_get(id, fallback)` 给控制代码取参数。
- `board.c`：`board_dl_init()`（LPUART1 115200 8N1）、512 字节环形缓冲、`board_dl_send()`；两个 UART 回调加 LPUART1 分支。
- `board.h` 四个声明；`stm32g4xx_it.c` 加 `LPUART1_IRQHandler`；`main_app.c` 加初始化 + 主循环 `host_link_poll(loop_us)`/`host_link_tick_1hz()`。
- Makefile 无需改（`wildcard App/Svc/*.c` 自动纳入）。

**遥测都是真实读数**：yaw/pitch/roll 来自 JY61P；`pwm_l/pwm_r` 读 **TIM8 CH1..4 的 CCR**；舵机读 TIM1 CCR；电压电流读 INA226（**只在 `OLED_IsBusy()==0` 时读**，否则会和异步刷屏抢 I2C1）；`line_mask` 取灰度原始电平；`state_id` 取 `black_num`。
**诚实边界**（未实现就明确回 `ST_NOT_READY`，不假装成功）：参数写 Flash、陀螺校准、车上记录；**遥测 v_l/v_r 恒 0**（编码器未做）——不拿 PWM 假装速度，否则调参台的波形会骗人；flags 的"陀螺有效/记录中"不发。

**验证**：`cd firmware && make` → **编译链接通过**，Flash 45%（text 58008+data 128 / 128KB）、RAM 10%。协议文档 §8.1 换成实际实现状态，新增 **§10 硬件验证步骤**（先看 RTT 的 `HOST: rx/tx` 递增 → 连上位机看握手/指纹 → 读参数 → 改参数看回读 → 最后才接状态机）。

**踩坑**：①**环形缓冲定义必须在中断回调之前**（C 要求先声明）——我第一次把整段 LPUART1 块连同回调一起挪，白改一轮；②**PowerShell 5.1 按 ANSI 读 .ps1**，UTF-8 中文会把引号拆坏（和 .bat 同一个坑，脚本必须纯 ASCII）；③TS 模板里嵌 markdown 反引号会截断字符串，改用 `chr(96)` 生成。

**未完成**：**用户没接板子，所以没烧录、没跑实测**（准备好的 flash+RTT 验证因此中断）。
未完成：① 硬件验证（接 U12 或 USB-TTL 后按协议文档 §10 走）；② **把 pid.c / gyro_control.c 的 #define 换成 host_param_get()** —— 在这之前改参数不影响控制律，这是当前最大的『看着能用其实没接线』处；③ 编码器（PB4..PB7）测速，否则波形里轮速恒 0；④ 参数写 Flash。
下一步：用户接上板子后：先只看 RTT 的 HOST 统计行确认 tx 在涨，再用上位机串口模式连接。

> 更早的会话条目已归档到 225 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-24
完成：**继续优化上位机（调参台）**：围绕"真实调参工作流"加了四件东西 + 一个会话记忆。

1. **改前改后对比**：波形页「存为基线」把当前窗口的数据冻结成**虚线**，改完参数再跑，实线（新）叠虚线（旧）—— 调 PID 最核心的动作。「清基线」清除。
2. **波形读数**：鼠标移到波形上出现**点线游标 + 各通道该时刻的值**（`t -3.42s 航向=+12.3° …`）。
3. **量化指标**：底部按可见通道显示 **峰峰 / 均值 / 标准差 / 过零次数** —— 调参别只靠眼睛数"晃了几下"。指标算法抽到纯函数模块 `metrics.py`（含 `settling_time`/`overshoot`/`zero_crossings`，带 deadband 忽略噪声），9 个单测覆盖。
4. **参数 diff 高亮**：参数表与参数总览里，**与默认值不同的项标黄**；总览页带 `*` 前缀，一眼看出这次调了什么。
5. **会话记忆**：模式/波特率/倍速/窗口/勾选通道存 `tools/host/config.json`，下次自动恢复；`MainWindow(load_config=False)` 供测试隔离。

**又抓到两个真 bug（都是测试/实跑发现的）**：
- `metrics.settling_time` 我写成了"稳定到结尾的时长"，正确约定是"从起点到稳定的时刻"（单测直接指出 2 != 3）。
- **改完参数"参数总览"不刷新**：`_refresh_param_row` 里只置了个标志位、没人消费它。已改为直接 `set_params()`；并把原来的测试从"手动调 `_fill_params`"改成走真实的"参数回读 → 实时刷新"路径，否则这个 bug 会被测试掩盖。

**验证**：`python tests/run_all.py` → **101 用例全过**（协议 40 / 指标 9 / 链路 25 / 启动脚本 4 / tk 界面 8 / Qt 界面 15）。另跑了完整调参工作流截图（存基线 → 改 line_kp 375→1500 → 再跑），`out/host/ui_wave_tuning.png` 能看到虚线基线与实线新曲线分离、底部四项指标。
未完成：固件侧 LPUART1 与 host_link 仍未实现（真正调参要等这个）；障碍位置与大型阶梯高度待现场复核。
下一步：① 按 docs/上位机通信协议.md 第 8 节做固件侧链路，打通后这套调参台就能对真车用；② 现场量场地校正规则文档。

## 2026-09-24
完成：**按用户要求砍掉地图与赛道仿真，把上位机收敛成调参台**（用户原话：地图太垃圾、仿真完全不对，都删了，只留波形+参数查看下发+参数的图形化显示）。

**删掉**：`field.py`（场地模型）、`MapView`（Qt 与 tk 两版）、`tests/test_field.py`、协议里的 **T_POSE 位姿/得分帧**及其收发、两版的"赛道"页与得分/障碍清单面板。规则与地图本身保留在 `docs/全地形机器人比赛规则与场地.md`（那是资料，不是仿真）。
**留下并加强**：波形（每通道独立量程）、参数表（双击改值即下发、预设）、实时图形（灰度条/表盘/轮速条/状态格）、**新增「参数总览」页** —— 每个参数一条，画出当前值在允许范围里的位置（绿=有余量/黄=接近边界/红=越界），一眼看出谁调到头了。
`simulator.py` 重写为 `DemoSource`：**只是信号发生器**（正弦偏差 + 反推灰度掩码 + 速度/电流/电压联动 + 注入堵转），不再有场地/障碍/计分；`line_kp` 调大偏差摆幅变小，保留"改参数看得出来"。界面模式名"模拟器"→"**演示数据**"。

**修掉两个真问题（都是我的错）**：
1. **`tools/host/main.py` 被覆盖成了 shot.py 的内容** —— `run_host.bat` 跑的其实是截图脚本，参数不认直接失败。已恢复；并扫过全部 21 个文件确认只有这一个被写坏。教训：脚本化写文件后要**回读校验**，别信"写成功"的返回值。
2. **"回放模式"弹窗一直弹** —— 根因是 `QComboBox.setCurrentText("模拟器")` 对**不存在的项静默无效**（模式名已改成"演示数据"），模式停在"回放"，点一次"连接"弹一次。已改为：连接流程**永不弹模态框**，模式不对/没串口就记日志并自动回落到演示数据；新增 `select_mode()` 带校验的切换方法。

**顺带**：重写 `ui_qt/widgets.py` 时漏了 `QPen` 导入（每次绘制都抛异常）—— **被 Qt 离屏冒烟测试抓到**，这就是那个测试的价值。

**验证**：`python tests/run_all.py` → **88 用例全过**（协议 40 / 链路 25 / 启动脚本 4 / tk 界面 8 / Qt 界面 11）；`python main.py --sim` 实测正常打开窗口、无报错。
未完成：固件侧 LPUART1 与 host_link 仍未实现；障碍编号位置与大型阶梯高度待现场复核；地图已不在上位机里。
下一步：① 按 docs/上位机通信协议.md 第 8 节做固件侧链路；② 现场量场地后校正规则文档里的障碍位置；③ 把实测轮径/轮距填进参数表。

## 2026-09-24
完成：**修好"双击 .bat 打不开"**（用户反馈"这个 bat 文件用不了"）。

**根因（实测复现，不是猜）**：`cmd /c run_host.bat` 输出 `'）' is not recognized as an internal or external command` / `'errorlevel' is not recognized...` —— 脚本被 cmd **按行拆错**了。两个原因叠加：
1. **换行符是 LF**，而 cmd.exe 要求 **CRLF**：LF-only 的 .bat 会被当成一行解析，命令全糊在一起；
2. 脚本里有 **UTF-8 中文**，而 cmd 用 OEM 代码页读脚本文件 → 乱码。

本仓库的写文件工具**默认给 LF**，所以我先前写的 `run_host.bat` / `selftest.bat` 必然踩这个坑。**与 Python、PySide6 无关**（同时单独验证了 `python main.py --offline` 正常）。

**修法**：两个 .bat 重写为**纯 ASCII 英文提示 + CRLF**，并加了健壮的 Python 查找（`where python` → `py -3` 兜底）、`cd /d "%~dp0"`、失败时 `pause` 留屏。
**实测**：`cmd /c run_host.bat` → 输出 `Starting host GUI with the built-in simulator...` 后 GUI 正常打开；`cmd /c "selftest.bat < nul"` → 跑完 104 用例、打印汇总表、停在 pause。两个都可用。

**顺带修的**：
- 新增 `tests/test_launchers.py`（4 项）**兜底防回归**：断言所有 `.bat` 是 CRLF、无 BOM、纯 ASCII、且 `cd /d "%~dp0"` —— 因为写文件的工具默认给 LF，这个坑很容易再犯。
- 测试里的**临时文件名写死了**（`%TEMP%/haixin_rec_test.csv` 等 6 处），两个测试进程并发时会 `WinError 32 另一个程序正在使用此文件`（我同时跑 selftest.bat 和 run_all.py 时暴露的）。已全部加上 `os.getpid()`。

**验证**：`python tests/run_all.py` → **104 用例全过**（协议 40 / 链路 24 / 场地 20 / 启动脚本 4 / tk 界面 8 / Qt 界面 8）。
未完成：固件侧 LPUART1/host_link、T_POSE 未实现；障碍编号位置待现场实测校正；大型阶梯立面高度待复核。
下一步：无阻塞项。下一步仍是固件侧链路（docs/上位机通信协议.md 第 8 节）与现场场地校正。

## 2026-09-24
完成：**按用户要求把上位机界面从 tkinter 换成 PySide6**（用户原话："界面太丑了，有那么多新的库你为啥不用"）。

**为什么换**：当初选 tkinter 是为了零安装，代价是观感差；但这是现场调参要盯着看的工具，不该省这一步。先确认 **PySide6 6.11.2 是 abi3 轮子、兼容 Python 3.14**（第一次装被网络中断，换清华镜像成功）。

**做法：逻辑层一行没动**，只换界面层 —— `protocol/transport/device/field/simulator/recorder` 全部保留，所以 100 个测试对两个界面都有效。
- 新增 `ui_qt/`：`theme.py`（深色 QSS）、`widgets.py`（QPainter 自绘：场地地图 / 8 路灰度条 / 航向表盘 / 速度条 / 状态格）、`window.py`（工具条 + 左侧车况 + 赛道|波形|参数|命令|记录|日志 六页）。
- 波形页做成**每通道独立量程的泳道**：共用一根 Y 轴时 PWM(0~7200) 会把航向(±180)和电流压成直线，没法调参。
- tkinter 版留作**零依赖备胎**（`--ui tk`），`main.py` 默认 Qt、缺 PySide6 自动退回并提示。
- **能在离屏模式渲染成 PNG 自己看并迭代**（tkinter 做不到）：`shot.py`。据此修掉了障碍标签压框/被截断的问题。

**验证**：`python tests/run_all.py` → **100 用例全过**（协议 40 / 链路 24 / 场地 20 / tk 界面 8 / **Qt 界面 8**）。截图 `out/host/ui_*.png` 肉眼确认：深色仪表盘、障碍通过变绿打勾、车+轨迹、状态栏链路统计都正常。

**坑**：①pip 装 168MB 的 PySide6 中途断流（IncompleteRead），换国内镜像解决；②**离屏平台字体数为 0**，截图里全是方块 —— 那是测试环境假象，真实 Windows 平台有 353 个字体，先在离屏下误判成"字体选错了"；③Qt 与 Tk 不能同进程，界面测试各自独立进程跑。
未完成：界面已换新，但**功能上仍缺**：固件侧 LPUART1/host_link、T_POSE 未发；障碍编号位置待现场实测校正；大型阶梯立面高度待复核。
下一步：① 现场量场地后改 field.py 的 OBSTACLES；② 做固件侧链路（含 T_POSE）；③ 按官方 11 个编号重排 renwu.c 状态机。

## 2026-09-24
完成：**改名 + 把比赛场地搬进上位机仿真。**

**1. 改名（用户明确）**：比赛名叫**海信杯**，「不是说我的车叫海信」。所以「海芯/海信」作为车的自称整个是错的 —— 已按**字节级替换**清掉全仓库 21 个文件里的「海芯」字样（含固件注释、RTT 字符串、`firmware/Makefile`、两份文档、上位机 4 个文件）；OLED 首页标题 `海芯 G474` 改成 `G474`（**字库是二进制子集，无法确认「全地形」在不在里面，所以用纯 ASCII，零风险**；要中文标题得用 kk-oled-font 重新生成字模）。目录 `Haixin/` 与 `haixin-app.bin` 是历史文件名，未动。

**2. 场地图数字化** → `tools/host/field.py`。场界/跑道/出发区/测试区用**黑色像素检测 + 45cm 网格标定**得到（±5cm）：大矩形环 + 中央十字 + 右侧凸出 + 顶部高台拱形、出发区 L 带、测试区 A/B。**11 个障碍的编号位置读不出来** —— PDF 里的地图是 465×522 像素位图，编号字只有 3~6 像素高（试过 500dpi 裁剪 + 连通域检测 + 拼图对照，只能看到蓝色像素团）。改为「蓝色标号连通域中心 + 障碍尺寸 + 必须压黑线」推断并**吸附到线上**（±20~30cm，`pos_conf` 标可信度，现场量完改表即可）。

**3. 上位机新增「赛道」页**：`widgets.MapView` 画场地/跑道/障碍(通过变绿)/车的位置朝向轨迹 + 得分/已完成清单/5 分钟倒计时。协议新增 **T_POSE（0x02，12 字节）**：x/y(0.1cm)、yaw、当前障碍、flags、得分、done_mask。

**4. 模拟器改成「真的在跑比赛」**：不再跑抽象正弦弯道，而是沿黑线的**密集路径纯追踪**（路点法实测会因跑道自交导致目标点前后翻转、原地打转上百秒，已弃用）；障碍按官方规则判定：1~10 号**前端进末端出**（穿越 ≥65% 占位）、11 号**登上即得分**（≥35%）、重复通过不重复计分；车高 25cm 会**卡在 20cm 隧道口**、车宽 30cm 过不了 25cm 窄桥。

**验证**：`python tests/run_all.py` → **92 用例全过**（协议 40 / 链路 24 / 场地 20 / 界面 8）。上位机实测：启动后得分 50、完成 4 个障碍、状态格显示 50/190 与剩余 2:30、地图 93 个图元、轨迹 58 点。场地模型另渲染成 `out/rules/field_model.png` 与官方俯视图逐项对照过。

**踩坑**：①地图位图分辨率太低，障碍编号物理不可读（见上）；②**转向符号反了** —— y 向下时 yaw 顺时针为正，目标在东边车却往西转；③障碍的「通过方向」不能取长边，要取**穿过它的那条黑线的走向**（柔软草地 40×60 摆在横线上，穿过的是 40 那一维）；④检测框要按循迹误差放宽（车沿黑线有 ~10cm 横向误差，用官方尺寸当判定框会擦边漏判）。
未完成：① 大型阶梯立面标的 3.0/5.0cm 比小型阶梯 10.8cm 还矮，待复核；② 障碍位置待现场实测校正；③ 固件侧 LPUART1 链路与 host_link 仍未实现（画地图用的 T_POSE 也还没发）；④ 车高/车宽未实测。
下一步：① 现场量场地后改 field.py 的 OBSTACLES；② 按 docs/上位机通信协议.md 第 8 节做固件侧链路（含 T_POSE）；③ 按官方 11 个编号重排 renwu.c 状态机，并加『进障碍前锁航向 + 卡住后退重试』。

> 更早的会话条目已归档到 220 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-24
完成：读了用户给的《第七届吉林省大学生人工智能创新大赛 竞赛规则》PDF，定位到赛项「**十、全地形机器人自主创新设计比赛**」（第 46–55 页），产出 `docs/全地形机器人比赛规则与场地.md`；PDF 存进 `docs/竞赛规则-第七届吉林省AI创新大赛.pdf`；新增可复用工具 `tools/pdf_pages.py`（--list/--dump/--render/--images）。

**关键规则事实**：11 障碍 = 外围 6×10 分（1栅格 2减速带 3小型阶梯 4石块地形 5方形隧道 6U形隧道）+ 内围 4×20 分（7防滑带 8柔软草地 9大型阶梯 10窄桥）+ 核心 1×45 分（11高台）；满分 190 = 185 障碍 + 5 计时；5 分钟从首次启动计时；**I 取单次运行最高分、不累积**，重跑次数不限；**1~10 号必须沿黑线前端进、末端出**，侧边进出/重复通过不得分；**11 号高台只需登上不必通过**；**不允许任何形式的远程控制干预**。

**硬约束**：隧道净高 **20cm**（整车总高必须<20cm）、窄桥桥面宽 **25cm**、窄桥坡 16.2cm、小型阶梯总高 10.8cm、高台约 20.5cm。**renwu.c 里的注释（2级台阶/木刺/栅栏/减速带/草/方形通道/UUUUU/超级大坡/十字路口）与这条赛道逐条对应**——旧固件任务 A 就是这个赛项。

**改名（第一轮）**：我发明的"**巡线小车**"是错的（它是全地形机器人）→ 先改为"海芯全地形机器人"。**用户随后澄清**：比赛名是「**海信杯**」，并明确"不是说我的车叫海信"——所以"海芯/海信"这个自我命名整个是错的，见下条。

**验证**：上位机 73 用例重跑全过；`tools/pdf_pages.py --list/--render` 实测可用。

**踩坑**：障碍尺寸图与障碍标题**不在同一页**（如"栅格"的文字在第 49 页、图在第 50 页顶部），按页码猜会张冠李戴——正确做法是按阅读顺序把每张图归给**前面最近的障碍标题**。第一次用页码对齐时把 4 张图标错了。
未完成：① 第 9 号「大型阶梯」立面标的是 3.0/5.0cm（比小型阶梯的 10.8cm 还矮），两次读图一致但与直觉相反，需对着原图再核；② 尚未按官方 11 个编号重排 renwu.c 状态机；③ 车高/车宽未实测，无法确认能否过 20cm 隧道、25cm 窄桥。
下一步：① 核实大型阶梯高度；② 实测车高/车宽对照 20cm 隧道与 25cm 窄桥；③ 按官方障碍编号重排状态机，并加『进障碍前用陀螺锁航向对准黑线延伸方向』+『卡住后退重试』；④ 把实测轮径/轮距填进上位机参数表（现为 65/150 待实测）。

> 更早的会话条目已归档到 219 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-24
完成：做完了**上位机**（`tools/host/`，Python3 + tkinter + pyserial，除 pyserial 外无第三方依赖）。
1. `protocol.py`：帧 = `A5 5A | type | seq | len | payload | CRC16-CCITT-FALSE`（开销 7 字节）；遥测**定长 32 字节**；参数表 23 项，默认值全部取自固件源码行号（循迹 375/1.5/50、陀螺 100/1.5/10、PWM 上限 7200 等）；HELLO 带 `param_crc` 指纹，两边参数表漂移会告警。
2. `transport.py`：真实串口 / 内置模拟器 / CSV回放 / 回环 四种传输 + 后台读线程 `Link`。CRC 失败只前移 1 字节重同步，所以上电乱码能自愈。
3. `simulator.py` **不是假动画**：灰度读数是横向偏差的函数，横向偏差又被参数表里的 line_kp/ki/kd 驱动回零，且控制器吃的是**量化后的掩码误差**（和真实固件一致）→ 改参数真的改变行为（Kp 375→5000 时横向偏差 26→6mm）。
4. 界面 `app.py`+`widgets.py`：24 项界面测试全过，关窗会取消残留 after 回调。
5. **验证**：`cd tools/host && python tests/run_all.py` → **73 用例全过**（协议 40 / 链路 25 / 界面冒烟 8），编译 0 失败；入口三种参数路径（--offline/--sim/--csv）实测。
6. 踩坑：①HELLO 帧 `<HBBH` 把 16 位 param_crc 当 8 位，被单测抓到；②模拟器最初横向动力学写错（转向指令既当角速度积分又当航向直接赋值），导致灰度掩码恒为同两路、line_error 峰值差 0，重写为"一阶转向延迟 + 正弦弯道 + 量化误差驱动 PID"后才正常；③**tkinter 界面测试必须独立进程**，同一进程反复建销 Tk 会 `Tcl_AsyncDelete` 直接崩（故用 tests/run_all.py 逐文件起进程）。
未完成：**固件侧链路未实现**：`board.c:11` 有 `hdl` 句柄、`:451` 有 `board_dl_handle()`，但 **LPUART1 从未初始化**（无 init 函数、main_app 未用）。所以上位机目前只能对模拟器/回放跑，接真车要等固件补完。参数改写只在 RAM，掉电不保存。
下一步：按 `docs/上位机通信协议.md` 第 8 节做固件侧，顺序：①只初始化 LPUART1 每秒发 TEXT 证明链路通 → ②发 TELEM 看波形与 CRC 计数 → ③PARAM_GET 回读且 param_crc 一致 → ④改参数用 RTT 对照 → ⑤最后接任务状态机。

> 更早的会话条目已归档到 218 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-24
完成：回答"固定动作的车还能加什么算法"，产出 docs/控制算法升级方案.md。核心结论：JY61P 是 6 轴无磁力计，yaw 本身就是积分量 → 不做全局坐标系，改**分段局部坐标系**（进入状态锁存 yaw0/cnt0，段内只用 Δψ/Δs，段间靠黑标/重新咬线/ZUPT 零速修正重锚）。

顺带读代码挖到 6 个此前没记录的事实（都会浪费后人时间）：
1. **HAL_TIM_PeriodElapsedCallback 在整个工程里没有定义** —— stm32g4xx_it.c:71-78 只调 HAL_TIM_IRQHandler，TIM6/TIM7 每 5ms 进中断后走 HAL 空弱回调 → motor_tracing_pid_0_task()/GyroControl_Task() 从来没被执行过。
2. **App/Bsp/encoder.c 整段是注释**，且 TIM3/TIM4 编码器模式在 Core/ 里也不存在（PB4–PB7 焊了没用）。
3. main_app.c 既不调 bsp_periodic_start() 也不调 renwu()（有意等电源，接回时要连上回调）。
4. **renwu.c 有 5 个分支永远进不去**：black_num==8(减速带,:422)/12(方形通道,:454)/14(:593)/20(:700)/22(:744) —— 这些值从未被赋值，在这几块上调参是白费。另有 210 处 HAL_Delay、212 行注释。
5. gyro_control.c:169 的 dt 硬编码 0.01，而 bsp_board.c:16-17 的 BSP_GYRO_PERIOD_MS=5 → I/D 项实际增益差 2 倍。
6. gyro_control.c:239-250 的 TurnLeftFixed/TurnRightFixed **名实相反**（Right 调 TurnLeft），且注释写"10度"实际 TURN_ANGLE_OFFSET=90。
未完成：方案未实施。P0（TIM3/TIM4 编码器 + 补 HAL_TIM_PeriodElapsedCallback + 非阻塞步骤框架）全未动；固有死分支与 dt 不符也未修。
下一步：做上位机（本轮开始）；之后按 docs/控制算法升级方案.md 的 P0→P3 落地。

> 更早的会话条目已归档到 217 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-23
完成：回答"为什么生产封装的引脚号越来越大"：用 v6 网表核实 —— 多出来的脚号是**散热焊盘**和**连接器固定脚**，不是错误：
- **U3/U4（AT8236，ESOP-8）**：物理 8 脚，网表用到 **9** 个，`U3.9/U4.9` = 底部**散热焊盘(EPAD)**，接 GND。
- **U12（SH1.0-6P）**：用到 **8** 个，7/8 = 固定脚（GND）；**U13（SH1.0-10P）**用到 **12** 个，11/12 = 固定脚；**U14/U15（SH1.0-4P）**各用到 **6** 个，5/6 = 固定脚。
- **U16（OLED FPC 30P）**用到 19 个、最大 28（29/30 是 NC）；U5（MSOP-10）最大 10、U2（LQFP64）最大 64 —— 这两个正常。
Gerber/贴片只看焊盘坐标，不看引脚编号，所以编号大小本身不影响生产。

**顺带给当前 1Ω 短路加了一条排查线索**：这些"多出来的固定脚"都接 GND，而 **U12（调试座 SH1.0-6P）的 1 脚是 3V3、7/8 脚是 GND 固定脚**，两者相邻 —— **如果那里连锡，就是 3V3–GND 短路**。另外 U1（JY61P 座，1/8 脚接 3V3）同理值得看一眼。
未完成：用户板子上的 3V3–GND 1Ω 短路仍未定位。
下一步：按上面线索：先看 U9 焊盘残锡、U12 的 1 脚 vs 7/8 固定脚、掀 OLED FPC 再量。

## 2026-09-23
完成：**电机输入电阻选型答复**（按网表核实位号）：AT8236 输入侧**串阻 = R6/R9/R10/R11（100Ω ±1%）**、**对地下拉 = R12/R13/R14/R19（100kΩ ±1%，接在 MCU 侧）**。下拉作用是复位/Hi-Z 时把两根输入钉成 0/0 → 电机滑行不转（安全）；且 **AT8236 输入端内部本来就有下拉**，外部这个是第二重保险，所以阻值不敏感。结论：**73kΩ 换上去更好**（拉低更硬、抗干扰更强，静态电流 33µA→45µA 可忽略）；**200kΩ 能用但不推荐**（拉低强度减半、抗干扰变差，只省 16µA）；**别低于 ~10kΩ**。时序上 200k×10pF≈2µs，远小于 23.6kHz 的 42µs 周期，无影响。已把 `pcb/主板选型与设计.md` 里写错的"R10–R13 一组"改成准确的串阻/下拉位号并补上结论。
未完成：是否要改（属下一版改板；现板已焊）。
下一步：无（等用户决定）。

## 2026-09-20
完成：**把旧固件上层并入新工程并通过编译+烧录运行**。
1. `App/Svc/`（renwu/tracing/PID_tracing/pid/gyro_control/jy61p，**上层逻辑不动**）+ `App/Bsp/`（motor/servo/encoder/key/buzzer/rgb，按新板重写）。
2. 新写 `App/Bsp/bsp_board.c`：TIM1 舵机 50Hz(PA8–PA11)、TIM8 电机 PWM 23.6kHz **保留旧板 0–7200 刻度**(PC6–PC9)、TIM6/TIM7 两个周期任务（替代旧 TIM2/TIM4，G4 上 TIM7 的中断向量叫 `TIM7_DAC_IRQn`）、循迹 PA0–PA7 读取。
3. 引脚重映射全部用脚本完成：循迹 8 路→`bsp_trace_bit(n)`、蜂鸣器 PD10→PD2、按键 PC1/2/3→五向键、舵机 CH4/CH3→CH1/CH2。`motor.c` 重写为 AT8236 双输入方案（正速度 IN1 出 PWM / 负速度 IN2 出 PWM）。`rgb.c` 空实现。
4. 旧的 `show_tracing_data()` / `show_tracing_data_pid()` 改成空实现（显示归 KK_UI，数据仍在 `tracing_data[]` 里供自定义页读），删掉 `OLED.h` 依赖。
5. **踩到编码坑**：旧工程源码编码混杂（部分 GBK、部分 UTF-8），统一按 GBK 转会吞换行把结构体成员注释掉；改成逐文件自动识别重拷后解决。
6. 主循环接上 `read_tracing_data()` → `g_live.trace[]`、`Yaw` → `g_live.yaw_x10`；`Motor_Init/Servo_Init/bsp_trace_init` 已调用（**TIM8 上电 PWM=0，电机不转**）；**TIM6/TIM7 周期任务暂不启动**（等电源到位再开，避免无电时跑控制）。
7. 实机验证：烧录校验通过，RTT 打印 `I2C1 @400kHz / OLED init OK / KK_UI init OK / FRAME: update avg=106us`，UI 正常运行。
（另外：主程序刷新方式经两轮人眼实验定为 **400kHz + IT**，用户确认有画面；400kHz+DMA 偶发黑屏已弃用。）
未完成：五向键 左/中 未实测；`renwu()` 任务状态机尚未接入 UI 事件（有意先不跑，避免无电时驱动电机/舵机）；TIM6/TIM7 周期任务未启动；5V/12V 侧全部未测。
下一步：电源件到齐后：调 R30→7.4V、开 TIM6/TIM7、把 `renwu_flag` 接到 KK_UI 的任务页、上车联调循迹与任务 A/B。

> 更早的会话条目已归档到 214 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：**黑屏根因排查（两轮人眼实验）**。
1. 先修掉一个真 bug：**STM32G4 的 DMAMUX1 时钟必须显式开**（`__HAL_RCC_DMAMUX1_CLK_ENABLE()`），HAL 不自动开；漏了会静默地永不作异步传输，但阻塞式的 `OLED_DriverInit()` 照常成功，极易误判。
2. 建了 `make DIAG=1` 诊断固件（`App/Src/oled_diag.c`）：利用 `OLED_Update()`(阻塞)/`OLED_UpdateIT()`/`OLED_UpdateDMA()` 是**运行时可选**的三个公共 API，一次烧录轮播多种组合，每相 5 秒画大字母+全屏图形。
3. 第一轮（v1）：A 100K阻塞✅ / B 100K+DMA✅ / C 400K阻塞✅ / **D 400K+DMA❌**。
4. 第二轮（v2，同样 4 相但换速率与方式）：A 400K+DMA✅ / **B 300K+DMA❌** / C 200K+DMA✅ / D 400K+IT✅。
   → **A 在 v1 黑、v2 好（配置完全相同）= 临界偶发**，不是速度阈值。结论：`400kHz + DMA` 不可靠，`400kHz + 中断` 正常。
5. 配套的软件量化手段（不用眼睛）：测 `OLED_IsBusy()` 持续时长 —— 8 页×128B≈1024B，100kHz≈92ms、400kHz≈23ms；实测 100K DMA=79ms、400K DMA=21ms、300K=28ms、200K=42ms，全部与理论吻合，**证明"数据确实发出去了"**，所以黑屏只能靠人眼在人眼层面区分。
6. 主程序已改为 **400kHz + IT** 并烧录验证（`oled tx_done=16 err=0 busy=0 st=0`），画面待用户确认。
未完成：400kHz+IT 的画面待用户确认；若仍偶发黑屏则退回 400kHz+阻塞（已验证）；旧上层(renwu/tracing/gyro/pid)尚未并入；五向键左/中未实测；5V/12V 侧未测。
下一步：用户确认 400kHz+IT 画面 → 继续把旧上层并入 App/Svc。

> 更早的会话条目已归档到 213 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：**排查"异步刷新黑屏"**。已确认的事实：
1. **根因之一（已修）**：STM32G4 上 **DMAMUX1 的时钟必须显式打开**（`__HAL_RCC_DMAMUX1_CLK_ENABLE()`），`HAL_DMA_Init()` 不会自动开（官方 `NUCLEO-G474RE/I2C_TwoBoards_ComDMA` 例程的 main.c 里有这一句）。漏了它的现象极其误导：**DMA 请求没被路由 → 异步传输永不发生**，但 `OLED_DriverInit()` 走阻塞调用所以照常打印 "OLED init OK"，看起来一切正常。加上后 `tx_done` 从 0 变 16。
2. **判定"异步到底有没有真搬数据"的办法**（纯软件、不用眼睛）：测 `OLED_IsBusy()` 持续时长。8 页 ×128B ≈ 1024B，**100kHz 应 ≈92ms、400kHz 应 ≈23ms**。实测 100kHz DMA **79ms**、400kHz DMA **21ms** —— 与理论吻合，证明整帧数据确实按正确速率发了出去；若只有几微秒就是"假完成"。
3. 建了 `make DIAG=1` 诊断固件（`App/Src/oled_diag.c`）：**四种组合自动轮播**（A 100K阻塞 / B 100K+DMA / C 400K阻塞 / D 400K+DMA），每相 5 秒画一个大字母 + 全屏图形（框/方块/圆/斜线）并打印 `oled_init/update/busy/last/tx_delta/err/busy_ms`。因为 `OLED_Update()`(阻塞) 与 `OLED_UpdateDMA()`(异步) 是**运行时可选**的公共 API（`KK_UI_REFRESH_MODE` 宏只决定 KK_UI 调哪个），所以一次烧录就能把"速率"和"DMA"两个变量分开。

**当前状态**：四组合的 API 层全部返回 OK、DMA 传输耗时也正确，但用户报告 400K+DMA 的主程序是黑屏；诊断为哪几个字母能显示**待用户肉眼确认**。教训：之前把 400kHz 和 DMA **两个变量一起改**且中间那版 400kHz 阻塞没让用户看过，导致无法二分。
未完成：黑屏根因未定 —— 需用户在四组合诊断固件上确认哪些字母有画面；旧上层(renwu/tracing/gyro/pid)尚未并入；五向键左(PC15)/中(PC3)未实测；5V/12V 侧未测。
下一步：用户报告 A/B/C/D 哪些能显示 → 据此定位是速率还是 DMA → 修复后回到主程序。

> 更早的会话条目已归档到 212 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：**按用户要求提升显示实时性**，两步都用实测数据验证：
1. **I2C 100kHz -> 400kHz**：把 `board.c` 的 `i2c_timing_100k()` 泛化成 `i2c_timing(pclk, scl_hz)`（t_LOW=0.52/f、t_HIGH=0.48/f，向上取整保证不违反最小时间），新增 `board_i2c_init_hz()`；自检固件仍用 100kHz，主程序用 400kHz。整屏首帧 119ms -> **29.7ms**。
2. **阻塞 -> 异步(IT+DMA)**：按官方 `NUCLEO-G474RE/I2C_TwoBoards_ComDMA` 例程接线 —— **I2C1_TX -> DMA1_Channel1 / DMA_REQUEST_I2C1_TX**，开 `I2C1_EV_IRQn`/`I2C1_ER_IRQn`/`DMA1_Channel1_IRQn`；`board_i2c_enable_dma()` + `board_i2c_dma_tx()`；中断向量在 `stm32g4xx_it.c`；新增 `App/Src/oled_glue.c` 把 `HAL_I2C_MemTxCpltCallback`/`HAL_I2C_ErrorCallback` 只转发给本显示实例（kk-oled-port 契约要求）；并把驱动 `OLED_DriverHandleMemTxComplete()` 的数据阶段改成**按 mode 选 IT 或 DMA**（原来硬编码 DMA）。
3. 主循环加了 **DWT CYCCNT 打点**，每秒 RTT 打印 `FRAME: update avg/max`。实测：阻塞 400kHz 时 avg=105us/max=4716us；改异步后 **avg=max=30us**，每秒 334 次循环 —— 刷新彻底移到后台。

用户已确认 400kHz 阻塞版的画面"图像都对了"；异步版的画面待用户再看一眼确认。
未完成：异步(DMA)刷新后的画面待人眼确认；旧上层(renwu/tracing/gyro/pid)尚未并入；五向键左(PC15)/中(PC3)仍未实测；5V/12V 侧全部未测。
下一步：用户确认异步刷新画面 -> 把旧上层并入 App/Svc 并接入自定义页实时数据。

> 更早的会话条目已归档到 211 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：**启动"旧固件 -> 新板 + KK_UI"移植**。做完的事：
1. 读完旧固件（`Haixin/`，F103+Keil）：上层= `renwu.c`(任务A/B状态机)/`tracing.c`/`PID_tracing.c`/`pid.c`/`gyro_control.c`；底层= `OLED.c`+`OLED_Data.c`(江协科技软件I2C 48KB)/`motor.c`/`servo.c`/`key.c`/`buzzer.c`/`rgb.c`/`jy61p.c`/`encoder.c`(整段注释掉了)。第 3 节列了**旧->新 引脚映射表**（循迹 PA4/PA5/PC4/PC5/PB0/PB1/PE7/PE8 -> PA0–PA7；电机 TIM8 PC6/PC7+PD6/PD7 -> PC6–PC9 双端输入；舵机 TIM1 PE13/PE14 -> PA8–PA11；按键 PC1/2/3 -> 五向键；蜂鸣器 PD10 -> PD2；OLED 软件I2C PB8/PB9 -> 硬件 I2C1 PA15/PB9 + SSD1315）。
2. **装载运行库**：`kk_oled` + `kk_ui` 复制进 `firmware/Middlewares/`（不从 Skill 资产目录构建）；6 个供应商 Skill 装到 `.agents/skills/`（已被 harness 识别）。
3. **按 kk-oled-port 契约重写驱动**：`kk_oled/driver/kk_oled_driver.c` 由 CH1116 改成 **SSD1315**（地址 0x3C、列偏移 0、初始化序列取屏厂手册含 `8D 14` 内部电荷泵与 `AD 30` 内部 IREF）。
4. **字模**：按 `kk-oled-font` 技能走 LEDFont 在线 API 生成 `kk_font_body`(wenquanyi_9pt 12px,182字形,3721B) 与 `kk_font_home`(wenquanyi_11pt 15px,63字形,1372B)；首页 4 个 32×32 XBM 图标程序化生成。
5. **写应用层**：`App/Src/app_ui.c` 定义页面拓扑（首页4入口 / 设置菜单10项 / 2个信息页 / 4个自定义页）、int/bool 变量绑定、`KK_UI_Texts`，并实现 KK_UI 要求的 5 个自定义页适配符号；输入归一化把五向键映射成 KK_UI 的 上/下/确定（上&左=UP、下&右=DOWN、中=OK）。
6. **Makefile 改双目标**：`make`=主程序 `build-app/haixin-app.bin`（45KB text / 128KB Flash），`make SELFTEST=1`=自检。补了 `Core/Src/syscalls.c`（newlib `_sbrk` 等桩，缺了链接不过）。第一版用 **blocking 刷新**（`-DKK_UI_REFRESH_MODE=KK_UI_REFRESH_BLOCKING`）。
7. **实机验证**：烧录后 RTT 打印 `CLK HSE=1 PLL=1 SYSCLK=170000000` / `OLED init OK` / `KK_UI init OK` —— **SSD1315 驱动初始化与 KK_UI 页面配置校验全部通过**，屏幕是否显示首页待人眼确认。

（另一个待办：五向键的**左(PC15)/中(PC3)** 仍未实测；上次 240 秒补测窗口内没人按键，日志显示修好的计数 `cnt=0/0/0/0/0` 正确。）
未完成：KK_UI 首页是否真显示待人眼确认；I2C 仍是 100kHz 阻塞刷新（动效会偏慢，待提到 400kHz 或改 IT/DMA）；旧上层(renwu/tracing/gyro/pid)尚未并入工程；五向键左/中未测；5V/12V 侧全部未测。
下一步：用户确认首页画面 -> 提高 I2C 速率改善动效 -> 把旧上层并入 App/Svc 并接入自定义页实时数据。

## 2026-09-20
完成：**USART3 自环验证通过（PB10/PB11 焊接 + AF 复用 + 收发链路全对）**，OLED 经用户肉眼确认**已点亮**。自环做法：`make LOOPBACK=1` 后每 2 秒在 USART3 发一帧合成 JY61P 角度帧（roll=10°/pitch=−20°/yaw=90°），短接 **U1 的 2 脚(PB10/TX) 与 3 脚(PB11/RX)** 自收，实测 `bytes` 每帧 +11、`frames` 每帧 +1、`bad=0`、`resync=0`，解出 `roll=99 pitch=-200 yaw=900`（0.1°）—— 字段偏移与校验和也一并验证。

**踩到并修掉一个自检固件 bug**：USART3 原来在主循环里 100ms 轮询 RDR，而 11 字节 1ms 内到齐，**只收到第 1 个字节、其余全溢出丢失**（`bytes=1`）。已改成 **USART3 中断接收 + 256 字节环形缓冲**（`board_jy61_start_rx()` / `board_jy61_get_byte()` / `HAL_UART_RxCpltCallback` / `USART3_IRQHandler`），JY61P 100Hz 连续输出才不会丢。

**另一处自检逻辑 bug**：按键计数把 `last_keys` 初始化成 `0xFF`，循环第一次读到空闲 `0x00` 被算成"五键同时刚按下"，导致 `cnt` 每键虚高 1。因此上一轮记录的"五向键全通"**作废** —— 实际只有 **上(PC13)/下(PC14)/右(PC2)** 被按到，**左(PC15)、中(PC3) 未验证**。已改成进循环前先读一次，并重开录制补测。
未完成：五向键的左(PC15)/中(PC3) 待补测；5V/12V 侧（蜂鸣器、编码器、循迹、AT8236 电机）因电源件未到全部未测。
下一步：等补测日志；电源件到齐做 5V/7.4V/12V 相关测试。

> 更早的会话条目已归档到 209 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：**五向键实测通过**：给 `firmware/` 加了**按键锁存 + 每键计数**（不用赶录制窗口），用户短接 PC13/PC14/PC15/PC2/PC3 到 GND，RTT 读到 `seen=0x1F`、`cnt=2/2/1/2/1` —— **五个方向全部识别**（日志见 `out/rtt_loop.log`）。同时新增 `make LOOPBACK=1` 变体：每 2 秒在 USART3 发一帧**合成的 JY61P 角度帧（yaw=90.0°）**，把 **U1 的 2 脚(PB10/TX) 与 3 脚(PB11/RX) 短接**即可自收 —— 这是**不依赖 JY61 模块就能验证 USART3 收发 + AF 复用 + PB10/PB11 焊接**的办法（用户没有该模块）。本次录制里 `bytes=0`，说明用户未做该短接，USART3 仍未验证。
未完成：OLED 是否真点亮仍待用户肉眼确认；USART3 自环未做；5V/12V 侧（蜂鸣器、编码器、循迹、AT8236 电机）因电源件未到全部未测。
下一步：确认 OLED 画面；需要时重开录制让用户短接 U1 的 2/3 脚跑 UART 自环。

## 2026-09-20
完成：**主板第一次上电自检，焊接体检通过**。搭好 `firmware/` 自检工程（Makefile + 自写 RB 版链接脚本 + STM32CubeG4 HAL + SEGGER RTT），J-Link 烧录（loadbin/verifybin）后用 RTT 直连观测，实测得到：**HSE 25MHz 起振 OK、PLL 170MHz OK（SYSCLK/HCLK/PCLK1/PCLK2 全 170MHz、FLASH_LAT=4）、I2C1 扫到 0x3C(OLED)+0x40(INA226)、INA226 MFG_ID=0x5449("TI")/DIE_ID=0x2260** —— 即**晶振、I2C 总线与上拉、OLED 控制器、INA226 焊接全部正确**。OLED 驱动（SSD1315，按屏厂序列，含 8D 14 内部电荷泵 + AD 30 内部 IREF）37 条命令全通、8 页显存写入无错，屏幕图案待人眼确认。五向键空闲读到 KEYS=0x00（正确），按下待用户实测；JY61P 解析器就绪但模块未插（bytes=0）。

**排查掉的假故障**：烧入带 I2C 的固件后 J-Link 一度完全连不上（`Failed to initialize DAP`，4 种速度 + JTAG/cJTAG 全试过）。**未能复现**：原样重烧同一固件、60 次连续探测全 OK；另外专门做了 PA15 判决实验（`make PA15TEST=2` 静态 I2C SCL、`=3` 开漏 GPIO 翻转、`=4` 完整 I2C 扫描后释放），SWD 全程都在。结论：**那次是调试会话偶发（stm32-dev 技能的 `rtt` 先起 GDB server，失败后留下的坏状态），不是板子/固件问题，也不是 PA15 与 SWCLK 连锡**。恢复手段：按住复位键 SW1 + J-Link 连上后烧 `make RESCUE=1` 的救援固件（已验证有效）。
未完成：OLED 是否真点亮待用户肉眼确认；五向键待按；JY61P 模块待插；5V/12V 侧（蜂鸣器、编码器、循迹、AT8236 电机）因电源件未到全部未测。
下一步：用户看屏/按键/插 JY61P 后读 RTT 日志确认；电源件到齐再做 5V/7.4V/12V 相关测试。

> 更早的会话条目已归档到 207 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：为 STM32G474RBT6 主板搭好**自检固件工程** `firmware/`（按 stm32-dev 技能流程）。构建链：arm-none-eabi-gcc 14.3 + GNU Make + 本地 STM32Cube_FW_G4_V1.6.3（只引用不复制）+ SEGGER J-Link V974（烧录用 JLink.exe loadbin/verifybin）。观测走 **SEGGER RTT**（J-Link 一条 SWD 就够；RTT 源码取自 github.com/SEGGERMicro/RTT 的 RTT/ 目录，缺的 SEGGER_RTT_Conf.h 自写）；链接脚本按 **RB 版 128K Flash/96K RAM/32K CCM** 自写（Cube 模板是 RET6，不能直接用）。

**里程碑1 通过（已验证）**：RTT 收到 `CLK: HSE=OK(25MHz) PLL=OK(170MHz) SYSCLK=170000000 HCLK=170000000 PCLK1/2=170000000 FLASH_LAT=4` + 1 秒心跳 —— **工具链/烧录/校验/25MHz 晶振/170MHz PLL/RTT 全通**。

**里程碑2 把调试口搞挂了（关键故障，未解决）**：烧入带 `board_i2c_init()`（PA15=I2C1_SCL / PB9=SDA）的固件后 **J-Link 再也连不上**：`Failed to initialize DAP`，4000k/1000k/200k/100k、SWD/JTAG/cJTAG 全试过均失败；VTref=3.289V 正常（板子有电）。唯一变量就是 PA15。**PA15 = LQFP64 第51脚，与 SWCLK(PA14=第50脚) 相邻** → 高度怀疑这两脚连锡短路（里程碑1 时 PA15 是复位默认 JTDI 高阻+上拉，不干扰 SWCLK；配成 I2C SCL 后一拉低就把 SWCLK 拽下去）。已建 `make RESCUE=1` 救援固件（只跑时钟+RTT、不碰外设）和 45 次自动重连后台脚本；用户当时未按复位键，未恢复。**待用户：断电量 MCU 第50/51脚是否导通 + 按住 SW1 让后台循环连上并烧救援固件。**
未完成：调试口未恢复；I2C/INA226/OLED/按键/JY61P 全部未测；5V/12V 侧外设因电源件未到暂缓。
下一步：用户按住 SW1 → 后台循环烧 build-rescue/haixin-rescue.bin → 连上后依次做 I2C 扫描、INA226、OLED、按键、JY61P；并查清 PA15/SWCLK 是否短路。

> 更早的会话条目已归档到 206 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-20
完成：回答「只上 3.3V 会不会出问题」。结论：**不会烧任何东西**（倒灌只有 0.2mA 量级），但 4 条使用注意：5V 侧外设要拔掉（欠压会让模块跑飞、搅乱 I2C）、别输出电机 PWM（AT8236 未上电却被驱动）、别在此状态标定 INA226/电源、注意调试器 3.3V 的带载能力（板子 3.3V 侧约 60–100mA，带不动会掉压复位）。已写进 `固件接口规格.md` 坑清单第 12 条。

**顺带查出一个文档错误（重要）**：解析 v6 网表发现 **U3.4 / U4.4 接的是 VCC3V3（不是 GND）**，U3.7/U3.9 接 GND。查 AT8236 手册确认脚位：GND=1、IN2=2、IN1=3、**VREF=4**、VM=5、OUT1=6、**ISEN=7**、OUT2=8、PPAD=9。即 **ISEN 直接接地（=不启用限流，手册原话「若不需要限流，直接接地」）、VREF 接 3.3V**（推荐范围 0.5–4V 内，因 ISEN 接地实际不起作用）。此前两份文档都写「ISEN/VREF 都接 GND」，**已改正**。同时发现关键采购约束：**AT8236 与 AT8236N 第 4 脚定义不同**（VREF vs nFAULT 开漏输出），本板 4 脚硬接 3.3V，**换成 AT8236N 会把 3.3V 对地短路** → 已写进 DECISIONS 与两份文档。另确认 U5=INA226AIDGSR(C49851)、U9=RT9013-33GB(C47773)、U7/U8=SP1N28STER。
未完成：未实测只上 3.3V 时 U9 的 1 脚电压；未评估是否给 ISEN 加检流电阻（要割线，暂不做）。
下一步：用户实测 U9.1 ≈2.6–2.8V 确认倒灌路径；后续 PCB 布局按附录 D 做 AT8236 EPAD 散热。

## 2026-09-20
完成：排查「只从调试口接 3.3V，5V 指示灯也亮」。**用 v6 网表严格证明路径**：VCC3V3 与 VCC5V 两条网络上**唯一共有的元件是 U9（RT9013 LDO）**（其余元件都只挂在一条轨上）；LED6+R24 在 VCC5V、LED7+R26 在 VCC3V3、D5.2=VCC5V / D5.1=buck 输出节点。结论：**PMOS LDO 的体二极管从 OUT 倒灌到 IN**，把 5V 轨顶到约 2.6–2.8V → LED6 只分到约 0.2mA（正常约 1mA）→ 微亮。**不是故障、不用改板**；12V/7.4V 灯不亮（D5 与关断的 buck 挡住）。佐证：下载并解析 Richtek 官方 RT9013 手册（`docs/datasheets/RT9013.pdf`，10 页），**全文搜 "reverse" 零命中** —— 既无反向电流规格也无反灌保护。已写进 `固件接口规格.md` 坑清单第 11 条 + `pcb/主板选型与设计.md` §2。
未完成：用户未实测 U9 的 1 脚电压（预期 2.6–2.8V）确认；未定是否把 LED6 挪到 D5 之前。
下一步：等用户量 U9.1 对地电压确认；若不想被误导，可把 LED6 从 VCC5V 挪到 D5 前（U8 输出侧）。

## 2026-09-18
完成：**把 `pcb/主板选型与设计.md` 从 v0.8 重写成 v1.0 并与 `固件接口规格.md` 同步**。做法：直接解析 v6 网表（`pcb/Netlist_Schematic1_2026-09-16.tel`，UTF-8，124 元件，`$PACKAGES` + `$NETS`）取**真实位号与真实网络**，不再靠记忆。查出并修掉的旧文档错误：电源芯片还是 SY8205/MP2315/AP2112K（实为 **U7/U8 SP1N28STER + U9 RT9013**）、还有 RGB/WS2812、INA226 ALERT 接 PB12、BOOT0 跳线、OLED 是 4 针模组、无线下载器挂 UART4（实为 **LPUART1 接 U12**）、连接器位号 J1–J12 全是虚构的（实为 **CN1/U1/U11/U12/U13/U14/U15/U16/U33/U34/H1/H2/H3**）。同步后 `固件接口规格.md` 改 5 处：附录 A 编码器不在 7.4V 轨（走 5V）、附录 B 电机座 **U33/U34 的 1 脚与 6 脚名字写反了**（实为 1=motorA2 / 6=motorA1）、OLED1 4P 行删掉换成 U16 裸屏、补 U1 JY61P 座脚位、补「I2C 座 U14/H3 第 1 脚给的是 5V」提醒。

**网表核对结论（v6 与规格书一致的部分，已逐条核对）**：MOTA_IN1/2 = U2.38/39 = PC6/PC7，MOTB_IN1/2 = U2.40/41 = PC8/PC9；舵机 U2.42–45 = PA8–PA11；编码器 U2.57–60 = PB4–PB7（TIM3=电机A、TIM4=电机B）；huidu1–8 = U2.12/13/14/17/18/19/20/21 = PA0–PA7；I2C 上拉 R5→U2.51(PA15)、R4→U2.62(PB9)；INA226 U5 高侧 U5.10=IN+、U5.9=IN−、U5.8=VBUS 全在 12V_PROT_OUT，**U5.3(ALERT) 确认悬空**；R2 5mΩ 在保护之后、U5 取样之前。**两处网名与实际相反（功能无影响，代码按实际写）**：`I2C2_SCL/SDA` 实为 I2C1；`LPUART1_TX`=PC0、`LPUART1_RX`=PC1（G4 上 PC0=RX、PC1=TX）。
未完成：未实测灰度模块 I2C 空闲电压；RES# 的 100nF 未补；7.4V 轨的 R30 未上台调。
下一步：进 PCB 布局；需要时出分区草图 / 2oz 走线宽度 / AT8236 散热过孔估算。

> 更早的会话条目已归档到 203 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-18
完成：**工作区整理**（用户要求：清缓存 + 以后新文件不许丢根目录）。新建 `docs/datasheets/`（4 本手册移入：stm32g474re / SSD1306 / SSD1315 / GME12864-41）、`tools/`（5 个脚本重写并移入，新增 `_paths.py` 统一用仓库根定位路径、`tools/README.md`）、`out/`（渲染产物目录，`render_gme.py` 现在写到这里）。删除 7 张一次性渲染图（gme_p3x/iic_*/cp_zoom）。**删除 `Haixin_FreeRtos (2).zip`（32MB）**——先对 1115 个文件逐个比 MD5，与 `Haixin/` 全部一致（checked=1115, mismatch=0）才删。**已验证**：`python tools/i2c_bus.py` / `render_gme.py 31` / `extract_pins.py` 均能跑通（输出数字正确，控制台中文乱码是环境问题）。根目录现在只剩 AGENTS.md、固件接口规格.md 和 6 个目录。规则写进 `AGENTS.md` 的「文件放置」节。
未完成：`pcb/主板选型与设计.md` 已被 `固件接口规格.md` 大部分取代，去留未定；`pcb/111111111.zip`（嘉立创 Gerber 包）保留未动。
下一步：把 `固件接口规格.md` 里 3.7 节 OLED 裸屏内容与实际屏幕再对一遍；然后进 PCB 布局。

## 2026-09-18
完成：把 **OLED 裸屏接法写进 `固件接口规格.md` 3.7 节**（此前该节还写着「4 针模组 SSD1306」）：改名「OLED：裸屏 U16（I2C1）」，补 30 脚逐脚接法表（C39/C40 飞电容、VBAT/VDD=3V3、BS1=3V3+BS2=GND→I2C、CS/A0/WR/RD 接地、D1+D2 短接=SDA、**IREF 脚26 悬空用内部 IREF**、**VCC 脚28 只接 4.7µF 绝不能接电源**）、I2C 控制字节（0x00 命令 / 0x40 数据）、屏厂初始化序列（AE…8D 14…AD 30…AF）、上下电顺序（VDD→VBAT→RES#→8Dh→AFh，约 100ms 点亮）、显存/刷新速率，以及「**做 FPC 封装必须按厂家脚位，立创EDA 通用 SSD1306 符号 2–5/10/29 脚定义不同**」的警告。另修正 3.4 节 I2C 实测判据：**4.7k 时若模块 10k 接 5V，空闲电压是 3.84V（不是此前写的 3.61V）**。
未完成：仍未实测灰度模块 I2C 空闲电压；RES# 的 100nF 未补。
下一步：上电实测 I2C 空闲电平；确认后可定是否拆模块上拉。

> 更早的会话条目已归档到 201 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：诊断「Tailscale 为何走 DERP 中继而非直连」。本机 `tailscale netcheck`：**UDP: true、MappingVariesByDestIP: false（本机是易穿透 NAT）、IPv4 61.149.135.62、UPnP/PMP/PCP 全为 false** → 路由器（网关 192.168.77.1，**360 路由**，HTTP 可开）**不做任何自动端口映射**，只能靠打洞，对端 `cai` 是硬 NAT 时失败，回落 `DERP(tok)`，RTT 300~430ms。**本机无需改设置**：`Get-NetUDPEndpoint` 显示 tailscaled 已固定监听 **UDP 41641**（0.0.0.0 与 ::），所以不需要 `tailscale set --port`。修复路径：路由器开 **UPnP**，或手动**端口映射 UDP 41641 → 192.168.77.125:41641**；只需一侧可达即可 P2P，故 `cai` 不用改。⚠️ 前提是路由器 WAN 拿到的是公网 IP 而非 CGNAT(100.64.x/10.x)。若双端 CGNAT 则无解，退路=找联通要公网 IP 或自建 derper。
未完成：用户尚未改路由器端口映射/UPnP；直连未验证
下一步：用户改完路由器后：`tailscale netcheck` 应显示 UPnP: true，`tailscale ping 100.71.106.46` 的 via 应从 DERP(tok) 变为直连 IP

## 2026-09-16
完成：**避坑**：本机系统代理开着（`HKCU Internet Settings` → `ProxyEnable=1, ProxyServer=127.0.0.1:7897`，Clash/Mihomo 类）。因此用 HTTPS 查出口 IP（api.ipify）得到 **112.120.48.227（香港）**，而 `tailscale netcheck` 的 STUN 得到 **61.149.135.62（北京联通）**。两者不一致是正常的：Tailscale 走 UDP、不经 HTTP 代理。已确认机器上**无 Clash TUN 虚拟网卡**在运行（网卡仅 WLAN/Tailscale/VMware VMnet1&8/蓝牙/以太网169.254），故代理未劫持 Tailscale。**若将来开启 Clash TUN 模式会吞掉 Tailscale UDP、直连必挂**，需加绕过规则。
未完成：无
下一步：若用户报 Tailscale 直连失效，先确认是否开了 Clash TUN

> 更早的会话条目已归档到 199 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：复测 `100.71.106.46`（Tailscale 节点 `cai`）：**已上线**（`tailscale status` 无 offline 标记，`tailscale ping` 有 pong，但经 **DERP(tok) 中继**，RTT 约 320–430ms，`direct connection not established`）。**但 SSH 仍不通**，且根因已从「机器离线」变为「对端没有 SSH 服务」：ICMP 通、**TCP 22 超时（超时=被防火墙丢弃，不是 refused）**；端口扫描 22/2222/3389/5985/5986/80/443/8080 全不通，**仅 445(SMB) OPEN**。ssh -v 显示 `Connecting to 100.71.106.46 port 22 ... Connection timed out`。结论：对端是 Windows，未装/未启动 OpenSSH Server（或防火墙未放行 22），与本机 SSH 客户端、用户名、密钥都无关。
未完成：需要在 `cai` 那台机上启动 OpenSSH 服务并放行 22（或改用其他通道）；用户名与认证方式仍未知
下一步：等用户在 `cai` 上开 sshd + 防火墙放行 22 后重试；若要走 445 则需另定方案（当前无该授权）

> 更早的会话条目已归档到 198 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：用户决定 **I2C 上拉保持 4.7k**（撤回 2.2k 改动）。核算：4.7k ∥ 模块 10k = **3.197kΩ**；低电平灌电流 0.91mA（限 3mA）；tr=0.847·Rp·Cb，Cb=100pF→271ns；**400kHz 可容忍 Cb≈111pF（带模块）/75pF（不带），100kHz 可容忍≈370pF**。已把 `固件接口规格.md` 的 I2C 上拉改回 4.7k 并写明并联关系、各速率可容忍电容、以及「空闲电平 ≥3.6V 说明模块 10k 接 5V，需拆或改 2.2k」的实测判据。
未完成：仍待实测灰度模块 I2C 空闲电压以确认其 10k 上拉到 3.3V 还是 5V
下一步：把 OLED 裸屏接法+厂家初始化代码写进 固件接口规格.md 的 OLED 节

## 2026-09-16
完成：用户告知**循迹（灰度）模块自带 10k 上拉**。与板上 2.2k 并联 = **1.803kΩ**：低电平灌电流 1.61mA（限 3mA，安全）；上升时间 tr=0.847·Rp·Cb，Cb=100pF→153ns、400kHz 可容忍 Cb≈196pF（比只有板上 2.2k 时的 161pF 更宽松）。**待确认关键点：模块那 10k 上拉到哪个电压**——若拉到模块自己的 3.3V 则完全没问题；**若拉到 5V，总线空闲电压会被分压抬到约 3.61V**，可能顶到 OLED(SSD1315) 与 INA226 的 3.3V IO 上限，需拆掉模块上的 10k 或改到 3.3V。验证方法：插上模块、总线空闲时量 SDA 对地电压，≈3.3V 正常、≥3.6V 要处理。
未完成：循迹模块 10k 上拉的目标电压待实测确认
下一步：确认后可定是否需要拆模块上拉；继续把 OLED 裸屏接法写进 固件接口规格.md

> 更早的会话条目已归档到 196 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：解释了 I2C 的 Rp（上拉电阻）与 Cb（总线电容）并用 RC 模型（tr≈0.847·Rp·Cb）说明速率上限；用户决定 **把 R4/R5 从 4.7k 改成 2.2kΩ**（支持 400kHz）。已把 `固件接口规格.md` 的 I2C 上拉值改为 2.2k 并补了说明（tr 公式、2.2k 可容忍约 161pF、模块自带上拉并联后仍高于 Rp_min≈967Ω）。
未完成：RES# 的 100nF 是否补；OLED 模组/裸屏对应文档更新
下一步：把 OLED 裸屏接法+厂家初始化代码写进 固件接口规格.md 的 OLED 节

## 2026-09-16
完成：复核第六版 netlist（sha 20652129，38004B，124 元件）：**OLED 从 4 针模组（OLED1 删除）改成裸屏 U16 = X096-2864KSWPG01-H30**，新增 C39/C40（1µF 飞电容）、C42/C44（4.7µF，VCC/VCOMH）、C41（100nF 退耦）、R39（10k，RES# 上拉）。逐脚核对 **U16 全部正确**：C1P/C1N→C39、C2P/C2N→C40、VBAT(6)→3V3、VDD(9)→3V3、BS1(11)→3V3、BS2(12)→GND、CS(13)/A0(15)/WR(16)/RD(17)→GND、RES(14)→R39→3V3、D0(18)→SCL、D1(19)+D2(20) 合并→SDA、D3–D7 悬空、**IREF(26) 悬空（内部 IREF）**、VCOMH(27)→C44、VCC(28)→C42、NC 全悬空；无单节点/空网络。**小建议**：RES# 只有 10k 上拉、缺 100nF，建议补（或用 MCU GPIO 控 RES）；D3–D7 可接 GND 更干净（可选）。

**I2C 总线仿真（Python RC 模型，`i2c_bus.py`）**：总线 = MCU + INA226(0x40) + OLED SSD1315(0x3C) + H3/U14 两个扩展座；板上上拉 R4/R5=4.7k→3.3V。上升时间 tr=0.847·Rp·Cb。结论：**100kHz 下 4.7k 可撑 Cb≈251pF（稳）；400kHz 下 4.7k 只能撑 ≈75pF（有风险，实际 Cb 估 60~130pF）**；Rp_min=(3.3−0.4)/3mA=967Ω。**建议把 R4/R5 换成 2.2kΩ（400kHz 可撑 ≈161pF）或保持 4.7k 只用 100kHz。** 吞吐：100kHz 满屏 1024 字节 = 92ms(≈11fps)，400kHz = 23ms(≈43fps) → 建议局部刷新。另注意灰度模块自带的上拉会与板上并联、别低于 ~1kΩ，且 I2C 地址不要撞 0x3C/0x40。
未完成：R4/R5 是否改成 2.2k、以及用 100k 还是 400k 待用户定；RES# 的 100nF 待补
下一步：定后更新 BOM/上拉表；把 OLED 裸屏接法+初始化代码写进 固件接口规格.md

> 更早的会话条目已归档到 194 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：用户提供了这块 OLED 的**厂家手册 `GME12864-41.pdf`**（金晨阳，0.96"/128×64、驱动 **SSD1315Z**、COG、连接方式=SOLDER、接口 4-SPI/I2C/8080、1/64 duty）。据此**定案 IREF(脚26)：厂家初始化代码用命令 `ADh 30h` 开内部 IREF，手册写明「用内部 IREF 时该脚保持 NC」→ 26 脚直接悬空、不接电阻**（此前讨论的 150kΩ/390kΩ 只用于外部 IREF 方案）。同时确认 30 脚定义与用户贴的表一致；I2C 接法：BS1→VDD、BS2→GND（BS[2:0]=010）、CS/WR/RD→GND、A0→GND(地址 0x3C)、D0=SCL、**D1 与 D2 短接=SDA**、D3–D7 悬空、VDD=3.3V(+4.7µF)、**VBAT=3.3V**、**VCC(28) 只接 4.7µF 不接电源**、VCOMH=4.7µF(厂家建议钽电容)、C1P/C1N 与 C2P/C2N 各 1µF 飞电容。**重要坑：厂家参考电路标注的是「External VCC」版（VDD/VCC 都外供、电荷泵关闭）；本板只有 3.3V，必须用内部电荷泵 → 软件要发 `8D 14` 开泵，且 VCC 不接电源。** 手册还给了完整初始化代码（见文档/会话），写固件可直接抄。保存了 `GME12864-41.pdf` 与参考电路渲染图（`gme_p31.png`/`iic_zoom.png` 等）。
未完成：OLED 该用这块裸屏还是 4 针模组仍未最终定；固件接口规格.md 的 OLED 节还没写
下一步：定屏后把该屏的接法+初始化代码写进 固件接口规格.md，并给出 I2C 写命令/写数据的封装

> 更早的会话条目已归档到 193 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：**更正**：上一轮按 SSD1306 手册算的 IREF 电阻（400kΩ）对 **SSD1315 不适用**。查 SSD1315 手册（Rev 1.1，`SSD1315.pdf`）确认：① `ISEG = (Contrast/32) × IREF`，对比度用命令 **81h**；手册**推荐 IREF = 30µA**（对比度 FFh 时 ISEG=240µA）；② **IREF 脚电压 = VCC − 3V**（不是 SSD1306 的 −2.5V），故 `R = (VCC − 3V)/IREF`；内部泵 VCC=7.5V → **R ≈ 150kΩ**（手册 I2C 应用图用 19µA → 240kΩ）；③ **也可用内部 IREF**：命令 **ADh** 选内部、ISEG 150/240µA，此时 **IREF 脚悬空**（连电阻都省）。另有：电荷泵 VCC 三档 **7.5/8.5/9V** 由命令 **8Dh** 选；上电顺序 **先 VDD → 再 VBAT → RES# 拉低≥3µs → 8Dh 开泵 → AFh 开显示 → 约 100ms 点亮**；关电 AEh→8Dh 10h→关 VBAT→关 VDD；**VBAT 绝不能拉到地**。SSD1315 显存 128×64（不是 SH1106 的 132），命令兼容 SSD1306。
未完成：IREF 具体阻值仍以该屏厂家参考电路为准（150kΩ 或 240kΩ）；OLED 模组/裸屏未定
下一步：可出 I2C 初始化命令序列；定屏后写进 固件接口规格.md

> 更早的会话条目已归档到 192 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-16
完成：补充 OLED 裸屏两点（查 SSD1315 手册 Table 5-1/Table 5-2 确认）：① **IREF(脚26) 必须接一个电阻到 GND**，芯片把该脚偏置在 (VCC−2.5V)，`IREF=(VCC−2.5)/R`；内部电荷泵 VCC≈7.5V、手册标称 IREF=12.5µA → **R≈400kΩ**（屏厂参考值优先；悬空则无段电流、屏不亮；越小越亮越耗电）。② **VBAT(脚6)**：手册规定开启电荷泵时 VBAT 接外部 VBAT 源，**要求 VDD < VBAT（典型 3.3~4.2V）**；本板只有 3.3V，实际做法是把 **VBAT 也接 3.3V**（多数模组如此，可用）；要更规范可 VDD 走 2.8V、VBAT 走 3.3V。
未完成：OLED 用 4 针模组还是裸屏仍未定
下一步：定后写进 固件接口规格.md

> 更早的会话条目已归档到 191 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：发现并核实**OLED 符号与实物不符**（重要，做封装前必须知道）：用户上传的 U16 是**立创EDA 通用 SSD1306 30 脚符号**，而屏 X096-2864KSWPG01-H30 厂家（SSD1315）的物理 FPC 引脚表不同——**2–5 脚顺序相反**（厂家 C1N/C1P/C2P/C2N vs 符号 C2P/C2N/C1P/C1N）、**10 脚厂家是 NC 而符号是 BS0**、**29 脚厂家 NC 而符号是 VLSS**、15 脚 A0=D/C#、16 WR=R/W#、17 RD=E(RD#) 只是命名差异。**做 FPC 封装必须按厂家表**。另下载 SSD1315 手册确认：**Table 5-2 BS[2:0]=010 为 I2C**（000=4线SPI、001=3线SPI、110=8080、100=6800），本屏未引出 BS0（内部已接 VSS）→ 只要 **BS1 接 VDD、BS2 接 VSS** 即为 I2C；C1P/C1N、C2P/C2N 是电荷泵飞电容脚（各接一个电容）。
未完成：OLED 用 4 针模组还是裸屏未定
下一步：若用裸屏：按厂家脚号做 FPC 封装 + 外围件，并写进 固件接口规格.md 的 OLED 节

> 更早的会话条目已归档到 190 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：回答 OLED 接法（用户拿的是**裸屏** X096-2864KSWPG01-H30，立创 C18723026：**主控 SSD1315（SSD1306 兼容）**、0.96/128×64、I2C+SPI、逻辑 1.65–3.3V、30 脚 FPC）。下载 SSD1306 手册核对后给出 I2C 模式接法：**BS0=0(GND)、BS1=1(VDD)、BS2=0(GND)**（Table 7-1）；**D/C#=SA0 → 接 GND 得地址 0x3C**；**E(RD#) 与 R/W#(WR#) 在 I2C 模式必须接 VSS**；**D0=SCL、D1 与 D2 短接 = SDA**（D2=SDAOUT 必须并上）；**VBAT 接 VDD**；**VCC(28) 是电荷泵输出≈7.5V，只能接电容、绝不能接电源**；VCOMH 电容 2.2~4.7µF；IREF 电阻约 400kΩ（=12.5µA，手册算法 (VCC−2.5)/12.5µA）；C1P/C1N、C2P/C2N 各 1µF 飞电容（以屏厂参考电路为准）。另说明 4 针模组无此外围、更省事；**用户尚未决定用模组还是裸屏**。
未完成：OLED 用 4 针模组还是这块裸屏未定；若用裸屏需加 30 脚 FPC 座 + 约 8 个外围件
下一步：用户定后把 OLED 那节写进 固件接口规格.md

> 更早的会话条目已归档到 189 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：复核第五版 netlist（sha f2574a39，36037B，118 元件）：**新增 12V 电源指示灯 LED1 + R38=10k**，接在 `12V_PROT_OUT → R38 → LED1 → GND`（≈1mA，位置在保险丝+反接保护+分流之后，正确）；M3 螺丝符号由 U16–U19 改为 SCREW1–4（无功能变化）；其余一处未动、无悬空网络。据此确认**原理图已无功能性遗留，可进 PCB 布局**；并同步更新 `固件接口规格.md`（附录 A 电源指示灯说明 + BOM 改为 4 个 LED）。
未完成：PCB 布局尚未开始
下一步：按附录 D 出详细布局/走线清单（AT8236 散热过孔、2oz 铜宽、Kelvin、地分割）

> 更早的会话条目已归档到 188 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：应要求把硬件规格**重写成面向固件/AI 的接口规格书**：新建根目录 `固件接口规格.md`（按 v4 netlist + DS12288 核对）。含：主控/时钟(HSE 25MHz, PLL /5×68/2, HSE_VALUE=25000000)、64 脚权威引脚总表（含 PC0/PC1 与 I2C1 命名纠正）、每个外设的协议与参数（AT8236 真值表、编码器 1980 计数/圈、舵机 50Hz、JY61P 11 字节协议、INA226 地址 0x40/CAL=1024/5mΩ、OLED 0x3C、五向键、蜂鸣器）、定时器与通信分配（TIM1/3/4/8/6/7、USART3/UART4/LPUART1/I2C1）、旧 F103 对照表、10 条坑清单、连接器/BOM/布局附录。
未完成：用户可能需要把旧的 pcb/主板选型与设计.md 合并或删除
下一步：进入 PCB 布局（附录 D 已有要点）

> 更早的会话条目已归档到 187 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：用户澄清循迹模块的 I2C 信号是 3.3V（5V 仅供电）→ **循迹电平问题闭环**：模块走 I2C 接 H3/U14（PA15/PB9，均 FT，上拉 R4/R5→3V3），huidu1–8 数字线留备用，PA0–PA7 的 TT(3.6V) 限制不再影响使用。至此整板**无功能性遗留问题**。
未完成：仅剩可选：VDDA/VREF+ 磁珠+1µF；I2C 地址与 OLED 0x3C/INA226 0x40 不撞
下一步：可进入 PCB 布局（AT8236 散热/电机大电流/晶振与 ADC 隔离/地分割）

> 更早的会话条目已归档到 186 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：复核第四版 netlist（sha dd7a0261，35579B，116 元件）。**去耦已补**：VCC3V3 上加了 C32/C33/C34/C36/C37/C38 共 6×100nF，现为 1×10µF + 7×100nF。**仍未改（唯一重要遗留）：循迹/灰度座 H1.1、U13.1 仍是 VCC5V**，且 huidu1–8 直连 U2.12/13/14/17/18/19/20/21（PA0–PA7）无串阻无分压 → 而 **PA0/1/3/4/5/6/7 是 TT_a(3.6V)**，若模块 5V 输出会超额定。其余核过：I2C 上拉 R4/R5→3V3、F1=5A、编码器 FT 直连 5V、无悬空网络。命名仍是 I2C2(实为 I2C1)、LPUART1_TX/RX 与 PC0/PC1 对调。
未完成：循迹供电 5V→3.3V（或加分压/电平转换）待用户改；VDDA/VREF+ 的 1µF+磁珠可选补
下一步：循迹电平问题确认后可进 PCB 布局

> 更早的会话条目已归档到 185 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：**用 ST 官方手册核实了引脚 5V 容忍性**（下载 DS12288 Rev 6 stm32g474re.pdf，用 PyMuPDF 提取 Table 12 'I/O structure' 列；图例 FT=5V 容忍、TT=3.6V 容忍）。结论：**编码器 PB4=FT_c / PB5=FT_f / PB6=FT_c / PB7=FT_f → 全 5V 容忍，5V 编码器直连 OK**。**但循迹 PA0–PA7 里只有 PA2=FT_a，其余 7 个（PA0/1/3/4/5/6/7）=TT_a（3.6V）→ 若循迹模块 5V 供电输出 5V 会超额定、有风险**；建议循迹模块改 3.3V 供电（或加分压/电平转换）。其余：I2C PA15/PB9=FT_f、UART4 PC10=FT/PC11=FT_f、电机 PC6–PC9=FT_f、舵机 PA8–PA11 全 FT、JY61P PB10/PB11=TT_a 但 3V3 供电 OK、LPUART1 PC1=TT_a 但 U12 供 3V3。手册已存 D:\Today\haixinbei\stm32g474re.pdf，提取脚本 D:\Today\haixinbei\extract_pins.py。
未完成：循迹模块供电（5V→3.3V）待用户改；原理图其余小项（去耦、D5、BOOT0 跳线）未改
下一步：用户改完循迹供电后可进 PCB 布局

> 更早的会话条目已归档到 184 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：复核第三版 netlist（sha a5b6ba01，34146B，110 元件）。**关键修复全部到位**：① I2C 上拉 R4/R5=4.7k→VCC3V3；② 保险丝 F1 改为 **SMD2920-500-24 = 5A**（原 2A）；③ 驱动输入 100Ω(R6/R9/R10/R11) + 100k 下拉(R12/R13/R14/R19) 到 GND；④ 编码器 100Ω(R29/R31/R32/R33)+100nF 对地(C24-C26/C31)+10k 上拉(R34-R37)；⑤ 蜂鸣器续流 D3=1N4148W（阳极 Q1 集电极、阴极 +5V）；⑥ 复位按键 SW1=TS665WS 接 NRST-GND；⑦ 单节点网络归零。**仍待处理**：编码器上拉/输出是 **5V**（编码器 VCC5V 供电）→ 必须确认 **PB4-PB7 是 FT(5V 容忍)**；MCU 去耦仍只有 C8(10µF)+C18/C23(100nF)，需每个 VDD 加 100nF + VDDA/VREF+ 磁珠；D5 SS54 仍串在 5V 输出；BOOT0 无跳线；I2C2/LPUART1 命名待正；ALERT(U5.3)/PB12/PC12 按决定留 NC。
未完成：确认 PB4-PB7 的 5V 容忍性；补 MCU 去耦
下一步：用户确认 FT 并补去耦后可进入 PCB 布局

> 更早的会话条目已归档到 183 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：回答用户 6 个问题：I2C 上拉只在 MCU 端加一组 4.7k→3.3V；RGB 暂不做；ALERT 非必须（不接不影响测量，用 I2C 轮询即可，要中断再接 PB12+10k）；驱动输入加 100Ω 串阻 + 100k 下拉（防上电/复位时电机误转）；编码器加 100Ω+100nF（+开漏输出需 10k 上拉）；蜂鸣器并 1N4148 续流管（阳极接 Q1 集电极、阴极接 +5V）；复位按键：NRST 加轻触开关到 GND（已有 10k↑+100nF），建议 BOOT0 加跳线。
未完成：原理图待按此修改
下一步：等第三版 netlist 复核

> 更早的会话条目已归档到 182 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：复核用户第二版 netlist（sha 4912201f，27329B，86 元件）。**上次的悬空全部修好**：I2C 已接上 OLED1.3/4 + INA226 SCL/SDA + 扩展座 U14(SH1.0-4P)/H3(2.54-4P)；循迹 huidu1–8 接到 H1(2.54 1x10) + U13(SH1.0-10P)；UART4 接到 H2(2.54 1x4) + U15(SH1.0-4P)；单节点网络为 0。新增 4×M3 螺丝孔(U16–U19)。串口分配与文档不同：**无线下载器 U12 走 LPUART1，灰度串口走 UART4**（功能等价）。**仍未改**：① I2C 无上拉电阻（4.7k 缺失，总线可能拉不起来）；② RGB/WS2812 完全删了（PC12 悬空）；③ INA226 ALERT(U5.3)/PB12 悬空；④ 保险丝 F2 仍 2A；⑤ 驱动输入/编码器无 100Ω/100k/RC；⑥ MCU 去耦仅 2×100nF+10µF；⑦ D5 仍串在 5V 输出；⑧ 无复位键/BOOT0 跳线/蜂鸣器续流管；⑨ I2C 插座供 VCC5V（MCU 3.3V 逻辑，需确认电平/上拉电压）。
未完成：上拉、RGB、ALERT、保险丝等未改
下一步：用户改完再复核；或按此清单出逐条修改表

> 更早的会话条目已归档到 181 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：用户问电机扭矩。查了 JGB37-520（12V）系列资料：同系列空载 7–960 rpm，全系列扭矩约 **0.07–16 kg·cm**，最大功率 ~10W；已知数据点 107rpm≈4.8 kg·cm、1:131(45rpm) 堵转≈7 kg·cm。**用户这颗 45:1/150rpm 的准确扭矩没有查到官方表**，按同系列推算约 **额定 0.2–0.4 N·m（2–4 kg·cm）/ 堵转 0.5–0.8 N·m（5–8 kg·cm）**，已建议以电机标签/规格书为准（或给额定电流/堵转电流/Kt 我来精算）。
未完成：电机准确扭矩未确认
下一步：用户给规格书或实测堵转电流后精算；可用于选轮径/传动比校核越障能力

> 更早的会话条目已归档到 180 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：复核用户提供的立创EDA 网表 `Netlist_Schematic1_2026-09-15.tel`（316 行，逐 net 解析 + MCU 64 脚对照）。结论：**原理图未画完**——I2C1（PA15/PB9）只有 MCU、无上拉、INA226 的 SCL/SDA/ALERT 与 OLED 的 SCL/SDA 全悬空；循迹 huidu1–8（PA0–PA7）无连接器；UART4（PC10/PC11）悬空而下载器走了 LPUART1；RGB（PC12）、INA226_ALERT（PB12）悬空。另发现 **F2 保险丝只有 2A**（电机堵转总电流 5–7A，会误跳），MCU 3.3V 去耦仅 2×100nF+10µF。已核对正确项：25MHz+2×15pF、NRST 10k+100nF、INA226 高侧接法(IN+ 源侧/IN− 负载侧/VBUS 负载侧/A0A1=GND=0x40)、两路 SP1N28 的 RT/COMP/FB/BOOT、7.4V 用 50k 电位器可调(3.4–8.3V)、反接 P-MOS+TVS、电机/编码器/舵机/按键/USART3 引脚与最终方案一致。用户器件有变：五向键用 **GT-TC006D-H060-L1**(C3019724)、3.3V LDO 用 **RT9013-33GB**(C47773) 而非 AP2112K。
未完成：原理图待补：I2C 总线+上拉+器件、循迹排针、RGB、ALERT、UART4、保险丝改大、去耦补齐
下一步：等用户补齐原理图后再复核；或出一份逐项 checklist 给用户改图

> 更早的会话条目已归档到 179 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：晶振改为 **25MHz + 2×15pF**（CL=12pF），文档改 4 处（引脚表/5.2 表/BOM/6.3 说明），并注明 **固件 HSE_VALUE 要改成 25000000**、PLL = /5×68/2 = 170MHz。
未完成：电源设计待复核；CubeMX 两项核对
下一步：可按需出布局分区草图

## 2026-09-15
完成：驱动最终拍板 **2× AT8236**，并全面替换文档里的 DRV8412（共 29 处：标题/约束表/框图/电源表/第3节/BOM/U2 网络/上拉表/布局建议/CubeMX 待办）。文档升到 **v0.8**。要点：每电机一颗单路 H 桥，PC6/PC7 与 PC8/PC9 做 IN1/IN2，PWM 直接打输入；ISEN/VREF→GND；EPAD 铺铜散热；**省下 PB0/PB1/PB2**。
未完成：电源设计待用户完成复核；CubeMX 两项核对（循迹 ADC 通道、编码器脚 FT）
下一步：可出布局分区草图 / 走线宽度 / AT8236 散热过孔估算

> 更早的会话条目已归档到 177 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：用 ST 官方 STM32_open_pin_data 的 STM32G474R(B-C-E)Tx.xml（LQFP64）逐项核对了全部引脚分配。**发现并修正**：① PC5 无 I2C 功能→PC4/PC5 不能做 I2C2（I2C2_SDA 仅 PF0/PA8）；② LQFP64 不存在 PE0/PE1；③ 确认 BOOT0 = PB8，且 G4 的 I2C1 与 F1/F4 不同（PB6 不是 I2C1_SCL）。据此把 I2C1 改到 PA15/PB9、编码器2 改到 PB4/PB5(TIM3)、ALERT→PB12、状态灯→PA12，并更新文档 21+ 处。其余（TIM4/TIM3/TIM8/TIM1/USART3/UART4/LPUART1/SPI2/ADC/五向键）全部核对通过。
未完成：驱动选型未定；灰度总线未定
下一步：驱动定案后更新第3节/BOM/接口；可出布局分区草图

## 2026-09-15
完成：用户要求 SSH 连 100.71.106.46，实测**连不上**，根因不在 SSH：该地址是 **Tailscale 节点 `cai`（MagicDNS：cai.taila1d22f.ts.net）**，`tailscale status` 显示 `offline, last seen 14h ago`，因此 ICMP 全超时、TCP 22 不通、`ssh cai@...` 报 Connection timed out。本机 Tailscale 是好的（本机 100.85.86.10 desktop-9772765，在线）。**踩点：`tailscale` CLI 不在 PATH**，直接跑 `tailscale status` 会报 CommandNotFound、容易误判成「没装 Tailscale」；实际在 `C:\Program Files\Tailscale\tailscale.exe`。目标机登录用户名与 SSH 认证方式（密码/密钥）均未知，`~/.ssh/config` 里没有它的条目。
未完成：目标机 `cai` 离线；无法验证 SSH 登录
下一步：等 `cai` 上线后，先 `& "C:\Program Files\Tailscale\tailscale.exe" status` 确认 online，再按用户给的用户名/密钥连（必要时加进 ~/.ssh/config）

> 更早的会话条目已归档到 175 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：讲了 INA226 接线，并**纠正两处会画错板的错误**（已同步改进文档）：① 封装 —— 立创页把 INA226AIDGSR 标成 VSSOP-8 是错的，TI 的 DGS 实际是 **10 脚 VSSOP**；正确引脚：1=A1、2=A0、3=ALERT(开漏)、4=SDA、5=SCL、6=VS(2.7–5.5V)、7=GND、8=VBUS(0–36V)、9=IN−、10=IN+。② 分流阻值 —— 满量程 ±81.92mV，`Rs ≤ 0.08192/Imax`；整机峰值按 ~10A 估，**由 10mΩ 改为 5mΩ**。接法：高侧，12V+ →[IN+]、Rs、[IN−]→负载，VBUS 接负载侧(IN−)，VS=3V3，SCL/SDA=PB8/PB9(4.7k↑)，ALERT=PB5(10k↑)，A0/A1→GND=地址 0x40；**IN± 必须四线 Kelvin 单独取样**，不能与大电流铜皮共用。
未完成：驱动最终选型仍待拍板
下一步：用户继续画图时可帮着核 INA226 校准寄存器/分流取值，或出布局分区草图

## 2026-09-15
完成：讲了 AT8236 的接法与 ISEN（未拍板驱动的科普 + 本项目约定）：引脚 1 GND / 2 IN1 / 3 IN2 / 4 VREF / 5 VM / 6 OUT1 / 7 ISEN / 8 OUT2，EPAD 接 GND 铺铜散热；IN1/IN2 两线控制（1/0 正转、0/1 反转、1/1 刹车、0/0 滑行），PWM 直接打输入；两颗做双路，本项目映射 PC6/PC7=电机A、PC8/PC9=电机B。**ISEN 是低端电流采样脚**：接 R_ISEN 到 GND 得 V=I×R_ISEN，既可用于测电流，也配合 VREF 做堵转限流（I_limit≈VREF/(R_ISEN×G)）。**本项目约定：既然电流检测只做 12V 主电源，每路电机 ISEN 直接接 GND（不设限流）**；VREF 不是供电（网上有文章写错），不用限流时接 GND/固定电平，别悬空。另发现**规格分歧**：中科微原厂 AT8236 标 5.5–30V、6A 峰值/4A 连续，而 MSKSEMI 的 AT8236-MS 立创页写 2A 连续/3.6A 峰值（更像 AT8870）—— 选型/限流时要以实际拿到的型号 datasheet 为准。
未完成：驱动最终选型仍待拍板（TB6612 / 2×AT8236 / 2×RZ7899 / DRV8412）
下一步：用户定驱动后改文档第3节+BOM+接口，并在接口表里写明 ISEN/VREF 处理

> 更早的会话条目已归档到 173 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-15
完成：**纠正**：上一轮引用立创商品页把 RZ7899-MS 写成「单路 6A 持续」是错的。核实后：RZ7899-MS 为无锡睿智微电子单通道双向驱动，**实际≈3A 等级**、3.0–25V、SOP-8 **无散热焊盘**，6A 只能是峰值/宣传值。结论修正为：两颗做双路时 **AT8236-MS（ESOP-8 带散热焊盘，2A/3.6A）比 RZ7899 更可信**；TB6612FNG 仍是最省事的单芯片双路。
未完成：驱动最终选型仍待用户拍板
下一步：用户选定后改文档第 3 节/BOM/接口

## 2026-09-15
完成：因用户嫌 DRV8412 贵，调研便宜双路驱动替代并给出候选（未拍板）：TB6612FNG（双路 1.2A/3.2A，~¥4）、RZ7899-MS ×2（单路 6A，~¥1–2/颗）、AT8236-MS ×2（单路 2A/3.6A，~¥1–2/颗）；已排除 SS8833T(1.0A 太小)、L298N(发热)。确认换驱动不改变现有引脚表。
未完成：驱动最终选型待用户拍板，文档第 3/BOM/接口仍是 DRV8412 版本
下一步：用户选好后把文档 DRV8412 段 + BOM + 接口定义一并改掉

> 更早的会话条目已归档到 171 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：复核用户的 12V→7.4V/5V 电源原理图。识别芯片为 **SP1N28STER（芯洲 SCT）**：Vin 3.8–36V、**5A 连续**、同步降压(45mΩ/20mΩ)、**Vfb=0.8V**、ESOP-8、频率可编程。核对：U8 5V 分压(51K+1.5K)/10K = 5.0V ✅；**U7 7.4V 分压 27K/8.2K 只有 3.4V ❌**，建议固定 68K/8.2K(=7.43V) 或 82K/10K。另指出：RT/CLK 频率电阻需确认、D3 SS54 对同步 buck 多余、C23 标 47pF 疑为 47µF 笔误、ESOP-8 散热焊盘要铺铜+过孔。
未完成：用户电源图未改（U7 分压仍为 3.4V）；电源文件不在仓库
下一步：等用户改完电源图再核；或出布局分区草图

> 更早的会话条目已归档到 170 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：按用户要求把陀螺仪从板载 ICM-42688-P 改回**外接 JY61P**：串口重排（USART3→JY61P J10；UART4→无线下载器 J8；LPUART1→备用串口 J9 + J6 灰度串口 15/16）；删除板载 IMU 的 SPI2/INT 接线，释放 PB12–PB15、PE0/PE1，富余脚增至 7 个（+PA12）；更新 BOM（去掉 ICM-42688-P，陀螺仪改为外购 JY61P 模块）、框图、关键电路（JY61P 布线/减震）、CubeMX 核对项（去掉 PE0/PE1）。文档共 20+ 处编辑。
未完成：电源设计待用户完成；CubeMX 两项待核对
下一步：等电源设计复核，或出布局分区草图/走线宽度/DRV 散热估算

> 更早的会话条目已归档到 169 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：把**所有接口定稿**并写入文档 v0.7：第 5 节引脚表改为定稿版（含网络名、每个信号的复用/上拉/默认电平、PA10/PA11 预留舵机、PB5=INA226_ALERT、PA12 为空余）；第 7 节新增 J1–J12 接插件逐 pin 网络定义、板上器件网络 U1–U7、上拉/默认电平/保护表（关键：DRV_nRESET 10k 下拉，上电默认关断电机输出）；第 10 节改为下一步。
未完成：CubeMX 三项待用户核对；电源设计待用户完成
下一步：等用户电源设计来复核，或出布局分区草图/走线宽度/DRV 散热估算

> 更早的会话条目已归档到 168 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：用户拍板主控用 **STM32G474RBT6**。已把文档改为该型号：第 0 节约束表、系统框图、BOM(含 C529413)、5.1 节重写为「主控已定 G474RBT6」并列出够用性与约 3 个富余脚。
未完成：电源设计待用户完成；CubeMX 三项待确认
下一步：引脚表按 G474RBT6 定稿；等用户拿电源设计来复核

> 更早的会话条目已归档到 167 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：补主控选型细节：查证 **STM32G474RBT6 = 128KB Flash + 128KB RAM（96KB SRAM + 32KB CCM）、LQFP64、52 I/O**（立创 C529413 参数字段为 128KB/128KB），与 G431RBT6 同封装同引脚但 RAM 是 4 倍 → 是「LQFP64 + 大 RAM」的甜点，避免了非升 LQFP100 不可的结论。注意立创该型号多为无现货可订货。已把该对比补进文档 5.1。
未完成：主控型号未最终确认（G474RBT6 / G474RET6 / G431RBT6 / LQFP100 选项）；电源设计待用户完成
下一步：用户确认 G474RBT6 后重排引脚表并标出富余脚（52 I/O 对约 49 用量）

## 2026-09-14
完成：回答主控够用性并补文档 5.1：G431 定时器(含 TIM4)与通信够；引脚 LQFP64 约 51 GPIO、本设计用 47~49，几乎无余量；32KB RAM 比引脚更紧。查证 **G431VBT6 = 128KB Flash / 32KB RAM（LQFP100，内存与 G431RBT6 完全相同，只多引脚）；G431 全系最大 128KB Flash**；要更多内存须 G474(512K/128K)。已在文档 5.1 加 LQFP100 选项对比行。
未完成：主控封装未定（LQFP64 vs LQFP100）；PE0/PE1 是否在 LQFP64 引出待 CubeMX 确认；电源设计待用户完成
下一步：用户定封装后重排引脚表并标出富余脚

## 2026-09-14
完成：核对 STM32G431RBT6 是否够用：定时器确认够（G431 共 10 个：TIM1/8 高级、TIM2 32位、TIM3/4、TIM15/16/17、TIM6/7，含 TIM4）；通信（I2C1/2、SPI2、USART3、UART4、LPUART1）够。**引脚是瓶颈**：LQFP64 约 51 GPIO，本设计用掉约 47~49，几乎无余量。关键事实：**G431RB 与 G474RE 同为 LQFP64，引脚数相同**，换 G474 不多引脚；要余量须 LQFP100。**32KB RAM 是比引脚更大的风险**。已把该结论写入文档 5.1 节。
未完成：PE0/PE1 是否在 LQFP64 引出未最终确认（待 CubeMX）
下一步：用户定 LQFP64(G431RBT6) 还是 LQFP100；确认后再定引脚并复核

> 更早的会话条目已归档到 164 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：选型文档推进到 v0.6（全板载 DRV8412、TM-4175-B-B 五向键 C465996、灰度主用 I2C2、电流检测仅 12V 主电源 INA226）。用户决定**电源部分（12V→5V 等）自己设计**，完成后交由我复核，不再让我做电源选型。
未完成：用户自行设计电源，待复核；选型文档 v0.6 的第 10 节小问题未回
下一步：等用户拿出电源设计（原理图/选型/参数）后按其复核：输入范围、负载预算、芯片外围、保护、噪声、布局

> 更早的会话条目已归档到 163 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：更新到 v0.6：确认全部板载（DRV8412 做板上，电机 6P 直接接 DRV 输出）；按键定为 TM-4175-B-B（立创 C465996，SMD-8P，库存 3761，前两款候选库存不足）；灰度主用 I2C2；电流检测仅 12V 主电源 INA226；给出 DRV 功率焊盘散热/大电流走线要点；建议 4 层板。
未完成：电机 6P 与 DRV 之间是否加熔断、舵机接口位置、是否接 OTW
下一步：继续辅助：引脚冲突核对、走线宽度/散热过孔估算、布局分区草图、BOM 核对、驱动移植要点（不生成原理图）

> 更早的会话条目已归档到 162 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：更新到 v0.5：驱动选 DRV8412（查到 TI 参数：2×3A/2×6A峰值、≤52V、HTSSOP-44，DRV8432 为 2×7A 版）；按键换五向方向键，找到立创现货 WS-4175(C53207707) 与 K1-5203UA-06(C5370424) 并给出 5 路 GPIO 接法；灰度 I2C2+UART 双接口保留；引脚表重排（PWM 改 PC6-PC9 给 DRV，PB0/PB1/PB2 给 RESET/FAULT，五向键占 PC2/PC3/PC13/14/15）。
未完成：DRV 板载/外接未定、五向键型号未定、灰度主用总线未定
下一步：等用户确认后继续辅助：核引脚冲突、走线宽度/散热、布局分区、BOM、驱动移植要点（不生成原理图）

> 更早的会话条目已归档到 161 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：更新文档到 v0.4：电机走主板 6P（新增驱动输出 KF128-4P 与布局大电流要求、XH2.54 3A 边界提示）；编码器 5V 电平处理（直连/分压三焊盘跳线）；舵机 2 路 7.4V；灰度预留 I2C2(PC4/PC5)+UART 通用循迹接口；引脚表加入 I2C2。
未完成：第 10 节：驱动型号、灰度总线、板尺寸、保留外设
下一步：确认后出完整原理图连接表（网络表）

> 更早的会话条目已归档到 160 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：确认电机 45:1/150RPM@12V、舵机 LDX-218，更新选型文档到 v0.3：舵机改为独立可调电源轨（SY8205FCC，默认按 6.0/7.4V 标定，5V 版可跳线并用 5V）；5V 逻辑改 MP2315 3A；补充编码器计数 11PPR×4×45=1980/圈、PID 节拍改用 TIM6/TIM7。尝试查 LDX-218 官方 datasheet 未成功（robotshop/PDF 被拦截），已在文档与 DECISIONS 标注电压待用户确认。
未完成：第 10 节待拍板点；原理图网络表未做
下一步：用户确认 LDX-218 电压/舵机数量/接法后出原理图连接表

> 更早的会话条目已归档到 159 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：根据用户回复更新选型文档到 v0.2：无线不板载、改为外接无线串口下载器并预留 3 路串口（USART3 主用 + UART4 + LPUART1）；IMU 定为板载 ICM-42688-P(SPI2)；电机定为 JGB37-3865-520（12V、11PPR 霍尔 AB 相、XH2.54-6P、堵转约 2.5~3.5A）。新增每电机 6P 接法二选一（过板/只取编码器）与编码器 3.3V/5V 电平处理说明。
未完成：第 10 节剩余待拍板点；原理图网络表/立创EDA 工程未做
下一步：确认接法/循迹路数/驱动型号/编码器供电/舵机数量/板尺寸后出原理图连接表

> 更早的会话条目已归档到 158 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：产出新主板选型文档 `pcb/主板选型与设计.md`：系统框图、电源架构（12V 单输入，TPS5450 转 5V/5A、AP2112K 转 3.3V，含反接/TVS/保险丝）、主控选型（G474RET6 首选/G431RBT6 降本，LQFP64）、功能块选型（ICM-42688-P IMU、INA226 电源监测、8 路循迹、2 路 AB 编码器、外接电机驱动接口）、完整引脚分配表（含旧板→新板对应关系）、接口定义、初版 BOM、布局布线建议。
未完成：第 10 节 8 个待拍板点未确认；原理图网络表/立创EDA 工程未做
下一步：等用户确认无线方案/IMU 板载与否/循迹路数/驱动型号/编码器电平/舵机数量/板尺寸层数/保留外设，再出原理图连接表

> 更早的会话条目已归档到 157 条（`SESSIONS-archive.md`，不自动注入；需要时用 `memory_read` 读取）。

## 2026-09-14
完成：调研了旧参考固件 `D:\Today\haixinbei\Haixin`（STM32F103VET6 + HAL + FreeRTOS），从 `Haixin_FreeRtos.ioc` 与 `Core/Src/*.c`、`Hardware/*.c` 还原出完整硬件接口表：8 路循迹 PA4/PA5/PC4/PC5/PB0/PB1/PE7/PE8；电机 PWM TIM8_CH1/CH2=PC6/PC7、方向 PD6/PD7+PB4/PB5；舵机 TIM1_CH3/CH4=PE13/PE14；OLED I2C1 remap PB8/PB9；JY61P USART3+DMA PB10/PB11@115200；按键 PC1/2/3；蜂鸣器 PD10；RGB PD0/PD1/PD3/PD8/PD9/PD11；TIM2(1ms)/TIM4(10ms) 做 PID 周期中断；HSE 8MHz@72MHz。
发现用户给的参考路径 `D:\Today\新建文件夹\Haixin` 不存在，旧代码实际在工作区内。
未完成：新板选型 BOM、引脚分配、原理图尚未产出
下一步：按已确认约束（G474/G431、12V、外部驱动、立创EDA）产出选型方案与引脚表，再画原理图

## 2026-09-20 · 台架测试
完成：用户反馈'现在加速度很快, 你没改回来吧' —— **追下去发现是我测错了**。
(1) 读电机实际参数: **KP_APR=400、ACC=100**(都是我测试时写进电机 RAM 的; 重烧板子/复位不影响电机参数)。所以'全恢复'其实只恢复了固件, **电机侧还在用 400**, 用户感觉到的快就是这个。
(2) **重新用正确方法测**: 先把夹爪移到起点并等到【真的停住】(连续 8 次采样位置不变), 再发 0->1000, 采样间隔压到最小, 记录到 50%/90% 的时间。结果 **KP_APR=400 时全程 220ms**(两次 220/220), 50%=156ms、90%=220ms, 轨迹近乎线性。**推翻了上一条'KP_APR/加速度都没用'的结论。**
(3) 上条结论错在测量方法: 判据用'位置进入目标±2/1000'被指数拖尾主导, 且采样函数自带 60ms sleep + read(256) 等满 20ms 超时, 每次采样 ~80ms, 分辨不出差异。**正确判据是'行进过程中的分位时间(50%/90%)', 不是'到位时间'。**
(4) 另外: 我保留了 ACK 过滤(它确有价值, 且独立验证过), 撤掉了 0x010B/Damiao_SetKpApr/使能重发 —— **但这些现在应该恢复**, 否则电机一断电 KP_APR 回 54、夹爪重新变慢。
(5) 尝试用 gdb 直接写电机 0x1B 做 54 vs 400 的 A/B, 但 stm32-dev 技能没有 call 命令、write 又要求 --device, 没做成。
未完成：**关键风险**: KP_APR=400 只在电机 RAM 里, 电机一断电就回出厂 54 -> 夹爪变慢。必须把 0x010B 接口 + 使能前重发恢复回去。另外 400 是否偏高(会不会啸叫/过冲)还没带载验证。
下一步：恢复固件的 0x010B(KP_APR 20..400, 默认 216 或 400) + Damiao_SetKpApr + 使能前重发, 网页上给'响应'滑块; 恢复后用【分位时间】法复测 54/216/400 三档, 并带载看有无啸叫。

## 2026-09-20 · 台架测试
完成：按用户'把它全恢复'的要求, 把 KP_APR/加速度这套'响应快慢'功能整个撤掉, 只保留其中真正有价值的 ACK 过滤。
**保留**: damiao.c 对【电机寄存器写 ACK】的过滤(WriteReg 置 s_reg_w_pending, 收包处理在反馈帧解析前把 data[2]==0x55 且刚发过写命令的帧丢掉)。**独立验证**: 写 0x0001=4(切控制模式, 会写电机 0x0A)之后, 角度/力矩/温度保持 0, 不再被污染 —— 这条路径在加功能之前就存在, 所以这是修掉一个既有缺陷。
**删除**: 0x010B 寄存器(读/写/白名单)、Damiao_SetKpApr、使能前的重发、g_acc_level/g_acc_rad_s2/Damiao_SetAccel/gripper_acc_to_rad_s2; 后端 R_ACC 与 acc 参数、status 的 acc 字段。固件零残留、重新编译烧录后二进制回到 40292 字节(与加功能前一致)。
**必须记录的一次纠错**: 我早前承诺'KP_APR 216 能让响应快 3 倍(54->505ms / 216->169ms)', **那组数据是 ACK 污染造成的假象** —— 当时写电机寄存器把 pos_abs 覆盖了, 0x0202 报的'位置'是假的, 于是看起来'到位更快'。ACK 过滤修好后数据干净, 实测 KP_APR 54/108/216/400 走 +50 步进是 780/781/778/785ms, **没有区别**; 加速度档 20 vs 200 是 1101/1080ms, 也没有区别。所以'快 3 倍'不成立, 已撤回。
**关于'为什么实际速度只有指令的 1/100'**: 上位机给 10 rad/s, 走 0.08rad 本该 ~8ms, 实测 ~780ms; 电机侧 ACC=100 / MAX_SPD=300 / VMAX=50 都不是瓶颈。**下一步该查电机反馈帧里报的实际速度(0x0011)**: 若也只有 ~0.1rad/s, 说明是电机自己没按 v_des 跑(很可能得换控制模式, 如模式 1 MIT 直接在帧里给速度和 kp/kd); 若报 10rad/s, 那就是位置回读的问题。这一步没查之前不要再给提速承诺。
未完成：速度慢的真因未定(见下一步)。
下一步：查 0x0011(电机反馈的实际速度)判断是电机没按 v_des 跑、还是回读有问题。

## 2026-09-20 · 台架测试
完成：处理用户两条反馈。
(1) **实时状态慢**: 根因是角度/力矩/温度属于 2s 的慢变量(当年为躲控制柜 56ms/条而下放)。改走串口后一条命令只要 ~2ms, 已把它们提回快变量: /api/status 现在 3 条命令 6.8ms(原来 1 条 2.4ms), 100ms 轮询占空比 6%, 年龄 0.0s。
(2) **加速度没作用**: 实测坐实 —— 档 20(2rad/s²) 与档 200(20rad/s²) 走 0->1000 是 **1101ms / 1080ms**, 没区别。结合早前的 KP_APR 实测(54->505ms / 108->336ms / 216->169ms), 结论是**模式 4 下电机不用 ACC/DEC**, 真正决定快慢的是位置环增益。另外还发现**写电机寄存器会把 g_motor 搞坏**(ACK 被当成反馈帧): 写一次 0x010B, 角度就变 12.59rad、温度 -63。因此**撤销加速度功能**: 固件里 0x010B 只记录不下发(写后实测 0x0010/0x0013/0x0014 保持 0, 不再污染), 网页上的加速度滑块与相关 JS 全部移除(确认页面里已无 c-acc)。
注: 之前那个'每次使能前重发 ACC'的改动也是同一个 ACK 污染问题的来源, 本轮一并撤掉了。
未完成：要做真正可用的'响应快慢'控制, 得先让 damiao.c 的收包处理能区分电机对寄存器写的 ACK 帧(当前它会被当成反馈帧), 然后暴露 KP_APR(0x1B)。KP_APR 已证实有效(505->169ms)。
下一步：等用户确认实时状态是否够快; 若要响应速度控制, 先修 ACK 过滤再暴露 KP_APR。

## 2026-09-20 · 台架测试
完成：按用户要求删掉实时状态卡底部那行连接说明(原来显示「已连接 USB-485 COM21（115200 8N1，从站 1）」, 断开时显示错误原因)。同样【元素和两处 JS 赋值一起删】—— 只删 div 会让 $('s-info') 变 null、render() 每轮抛错(和之前删 h-sub 是同一个坑)。已确认零残留引用, 截图确认实时状态现在结束于「数据年龄 / 上次操作耗时」。注: 断开/无应答的状态提示仍由右上角红标 + 连接面板的提示承担, 信息没有丢失。
未完成：无
下一步：无

## 2026-09-20 · 台架测试
完成：用户要求: 删掉控制卡里那三行说明; 夹持力/速度/加速度滑块拉长、后面的数据缩短。都做了(CSS 里 label 66->54、val 56->44、map 158->104, 力档文本 '1.20 / 1.50' -> '1.20/1.50'; 说明块整段删除), 截图确认。
**但截图同时暴露了一个我自己引入的严重 bug**: 实时状态里角度/温度持续变成 --、力矩显示 -5.000。追查: 485 直读板子确实返回垃圾(0x0010=1259、0x0012~15=[-500,-63,-96,0]), 而**同一时刻 RTT 显示板子内部是正常的**(en=1 st=3 toq=-79) —— 两边矛盾。垃圾值恰好是 PMAX(12.566) 和 TMAX(5.0), 说明是【电机参数值混进了 g_motor】。
**根因**: 我在 Damiao_Enable() 里加了 Damiao_SetAccel()(每次使能前重发 ACC/DEC 两帧)。电机对寄存器写的 **ACK 帧被收包处理当成反馈帧**, 把 g_motor 的 pos_abs/torque/t_rotor/t_mos 覆盖成垃圾; 因为放在使能热路径上, 每次使能都污染一次。
**修复**: 把 Damiao_SetAccel 从 Damiao_Enable() 移除, 只在 0x010B 被写时调用。重烧后立即恢复: 0x0010=0、0x0012~15=[0,0,0,0]; 使能后 角度 0.65/力矩 0.02/温 34/39/cal 0/1625/v_des 10.0(速度修复保持)/acc 改 400->400; **护栏告警 0 条**。
保留的合理性护栏(角度/力矩/温度/端点越界当没读到)在这次排查里立了功 —— 它把 65473℃ / 12.59rad 挡在了界面外, 也正是它让我注意到异常持续存在。
未完成：加速度只在改档位时写进电机; 电机若断过电, ACC 会退回出厂 2.0, 要重新拨一下 0x010B。根因(ACK 被当反馈帧)只在热路径上致命, 但收包处理本身没有过滤 ACK —— 属于已知隐患。
下一步：若要让 ACC 在电机掉电后自动恢复, 得先让收包处理正确区分 ACK 帧(需要达妙手册里的 ACK 帧格式), 不能靠重发绕。

## 2026-09-20
完成：SDK 文档收口 + 工具口实现按台架脚本复核。① 文档结构定为：`SDK/README.md`（唯一主文档，简洁版：安装/三种接法/安全/API 表/异常/测试/说明）+ `SDK/docs/485命令表.md`（`docs/485命令全表.md` 的随件副本，正文逐行核对一致）+ `SDK/host/`（上位机占位，注明还在 tools/web_gui 迭代）；原 `SDK/docs/GenceGripper-SDK.md` 已删（两份文档会漂移）。② 对照 `tools/gripper_test/test_gripper.py`、`tools/web_gui/gripper_web.py`、`tools/gripper_test/mb.py` 补了工具口 4 处：读失败重试 2 次（写不重试，依据 test_gripper 的 rd_retry）、`arm_enable` 开关（默认不发 EnableRobot，test_gripper 默认发、gripper_web 默认不发）、`safe_enable` 第一次 165 没生效时隔 1s 再写一次（上层 165→等3s→165 的自愈）、`ping()/wait_link()`（依据 mb.open_bus 的探测）。**验证**：`python tests/test_mock.py` 27 PASS / 0 FAIL；假控制柜联调 7 PASS / 0 FAIL；py_compile 全过。
未完成：① **485 命令表有两份**：仓库根 `docs/485命令全表.md`（权威）与 `SDK/docs/485命令表.md`（随件），改一处必须同步另一处，否则会像 ERR_TEXT 那样出现两套不一致的表。② 上位机还在 `tools/web_gui/gripper_web.py` 迭代，没迁进 `SDK/host/`。③ SDK 仍未上真板/真控制柜。
下一步：上位机稳定后迁到 `SDK/host/`；迁的时候顺手把 485 表副本的同步做成一条检查（比对两份正文）。

## 2026-09-20 · 台架测试
完成：按用户要求做了两件事并验证。
(1) **显示真实映射值**: 滑块旁按固件同一套公式显示(力 30 -> 1.20/1.50 N·m、速度 500 -> 10.00 rad/s、加速度 200 -> 20.0 rad/s²), 实时状态里另外显示设备回读的 0x0003(电机速度给定)/0x0004(电机力矩给定)。实测: 力 30 -> 1.2 N·m、速度 500 -> 10.0 rad/s, 与公式一致。
(2) **新增加速度**: 固件新增 0x010B(档 20..1000 -> 2..100 rad/s², 默认 200), 写入即把 ACC/DEC 写进电机 0x04/0x05; Damiao_Enable 每次使能前重发(电机参数在它自己 RAM 里, 掉电丢失)。端到端实测: 档 200/400/800/1000 -> 电机 ACC 20/40/80/100; 档 <20 钳到 20(写 0 时电机不接受 ACC=0 会退回出厂 2.0, 钳位后回读与电机一致); 写 1001 被拒。
(3) **显示功能当场抓到一个真 bug**: safe_enable() 里在写完速度后又写 0x0107=2, 而它与 0x0003/0x0104 是同一个 v_des, 把用户设的速度盖成 2 rad/s —— 已删除该写入, 实测修复后滑块 300/500/1000 -> 电机 6.0/10.0/20.0 rad/s。
(4) 偶发读数离谱(角度 12.59/温度 -63)加了合理性护栏, 并修掉 temp_rot/temp_mos 漏做 s16 导致显示 65473℃ 的问题; 护栏实测抓到过 3 条并正确显示 --。
固件编译 0 告警、烧录校验一致(40452 字节); 后端 mock/真机回归正常。
未完成：偶发读数离谱只在【后端刚启动的第一次慢变量刷新】出现过, 之后再没复现(485 直接读始终干净); 已用护栏挡住不让它上屏, 但根因未彻底定死(倾向'串口刚打开时首几个事务受扰')。
下一步：等用户刷新页面看四个滑块(位置/力/速度/加速度)与映射值显示。

## 2026-09-20 · 台架测试
完成：按用户要求把左上角标题后面的连接说明(USB-485 直连 · COM21 · 115200)去掉, 标题只留「夹爪上位机」。⚠️ 必须【元素和 JS 一起删】: 只删 span 的话 $('h-sub') 返回 null, render() 会每轮抛错。已确认 h-sub 无残留引用, 未连接分支仍完整(右上角红标 + 信息行照常显示未连接)。注: 后端 /api/status 的 conn 字段保留(现已无人显示, 留着不碍事且 API 层有用)。截图确认标题干净、页面正常(link 正常 / lat_avg 2.3ms)。
未完成：无
下一步：无

## 2026-09-20 · 台架测试
完成：按用户要求改掉「切到越疆方式时预填控制柜地址」。改动: (1) loadConn 里只有 c.bus==='dobot'(即后端当前真的在用控制柜)时才回填 k-ip/k-dash, 否则一律清空; (2) doConnect 选 dobot 时若 IP 为空则拒绝连接并提示「左臂 192.168.5.4 / 右臂 192.168.5.5」, 端口留空按 29999; (3) k-ip 的 placeholder 改成「控制柜 IP（必填）」, 方式说明里补上左右臂地址与端口默认值。
验证(服务端返回的 HTML 实测): k-ip 只有 placeholder、**没有 value= 属性**; loadConn 的预填分支只在 c.bus==='dobot' 时走, 否则清空; 当前后端是 serial 模式, 所以切到越疆时地址一定是空的。页面截图确认连接条仍是 方式/串口/刷新/连接/断开, 布局未坏, lat_avg 2.3ms。
未完成：无
下一步：等用户刷新页面切到越疆方式确认地址是空的。

## 2026-09-20 · 台架测试
完成：修用户报的串口重连问题: 连接别的波特率后控制不了、再切回 115200 也连不上(因为旧连接不会自动断开)。
先复现: 走 API 把波特率改 9600 再改回 115200, 我这边能恢复, 说明根因不是「没关旧连接」那么简单。真正的机理是 **Windows 上 Serial.close() 返回后端口句柄要过一下才真正释放**, 紧接着重开同一个口会拒绝访问 —— 换参数/快速重连时正好踩中。
改动: (1) SerialBus._open() 失败时重试 5 次、每次隔 0.3s; (2) apply_conn 关掉一个本来开着的总线后先 sleep 0.3s 再重开; (3) close() 顺手复位 _dead_t; (4) **波特率固定 115200**: 页面删掉波特率下拉, apply_conn 忽略传上来的 serial_baud(API 仍可传但会被忽略), 提示文案改成「波特率固定 115200、Modbus RTU 从站 1, 无需设置」。
实测: **快速连续重连 10 次 -> 10/10 成功**; 故意传 serial_baud=9600 -> 连接信息仍回 115200(已忽略); 断开后立刻重连 OK; 页面截图确认波特率下拉已消失、连接条为 方式/串口/刷新/连接/断开。
未完成：无
下一步：等用户刷新页面重试切换连接。

## 2026-09-20 · 台架测试
完成：按用户要求加了「断开连接」。后端 POST /api/disconnect: 关总线、置 bus=None、清状态缓存与拖动轨迹。页面在连接横条上加了「断开」按钮, 并让页面进入明确的未连接状态(左上角标题、右上角红标都显示未连接, 状态项全 --, 提示先选端口再点连接)。
顺手处理了三处只有断开才会暴露的副作用: (1) /api/status 的异常分支原来无条件 traceback.print_exc(), 断开后每轮轮询都会往 stderr 刷堆栈 —— 改成 bus 为 None(主动断开)时不打, 只在真异常时打; (2) 拖动轨迹泵在断线时会每 10ms 刷一条轨迹回放失败日志 —— 改成断线时静默清空轨迹; (3) 断线后前端轮询放慢到 1s, 避免一直打。
实测: 连接 -> disconnect -> link=False/connected=False/error=未连接（在上面选好端口后点「连接」）-> 断开状态下连取两次状态都干净 -> connect 重新连上(位置 450, lat_avg 2.2ms); 后端输出里 **Traceback 次数 = 0**。断开后的界面已截图确认(tmp/shot_off.png): 标题未连接、红标未连接、状态全 --、断开按钮在。
未完成：无
下一步：等用户刷新页面看观感与手感。

## 2026-09-20 · 台架测试
完成：按用户反馈(排布完全不对、不用模拟、你自己打开看看来验证)修完并**用截图验证**。做法: 用 Edge 无头截图(msedge --headless=new --screenshot --window-size=1400,1000 --virtual-time-budget=6000), 再读图看。看到的问题: 连接卡在 DOM 里排第一、于是占了左栏最显眼的位置, 实时状态被挤到右栏, 右栏下面一大片空白、页面下半截全空。改法: 连接压成**顶部一条紧凑横条**(横跨两栏), 实时状态(读)放左、控制(写)放右, 日志全宽在底 —— 用 CSS order + grid-column 实现, 不动 DOM 顺序。另外: 从页面的方式下拉里去掉「模拟」(用户说不用; 命令行 --bus mock 仍保留给开发用)。顺手修掉页面上两处旧文本: 控制卡说明还写着 0x0040=0x51(固件早已统一成 0x50, 正是那个照文档改就复发的坑), 以及说明里没被渲染的 Markdown 星号改成 <b>。验证: 宽屏(1400)与窄屏(760)各截一次 —— 宽屏连接条一行、状态/控制两栏等高、无空档; 窄屏自动降级单列、连接条折行不溢出。截图落在 tmp/shot1..3.png 与 tmp/shot_narrow.png。
未完成：改前端前一定要先截图看 —— 上一轮就是盲改 HTML 才出的排布问题。
下一步：等用户刷新页面确认观感。

## 2026-09-20 · 台架测试
完成：按用户反馈(左上角还显示 29999、要能选串口、这个上位机要所有人接上线都能用)改完并验证。根因: index.html 的 h1 里写死了副标题控制柜 dashboard 29999, 既过时又只有懂这套的人才看得懂。改动: (1) 新增连接面板(页面最上面第一张卡): 方式下拉(USB-485 直连/越疆控制柜/模拟)、串口从本机实际枚举的列表里选(带描述, 可刷新)、波特率、控制柜 IP 与端口按方式显隐、连接按钮与结果提示、底部随方式变化的说明; (2) 左上角标题改成动态, 由 /api/status 的 conn 字段驱动(如 USB-485 直连 · COM21 · 115200); (3) 后端新增 GET /api/ports(pyserial list_ports 枚举, 当前设置里那个即使没被列出也补进下拉框) 与 POST /api/connect(关旧总线, 改 args, 重建, 清状态缓存, 即时生效); (4) 打不开串口的报错改成人话(提示检查适配器/端口号/是否被占用)。实测(COM21): /api/ports 正确列出 COM21(USB 串行设备)/COM1/COM19; 切 mock 成功; 切回 serial 成功; 故意选 COM99 返回明确失败; 恢复后 lat_avg 2.2ms。页面元素检查 k-bus/k-port/k-refresh/k-connect/h-sub 全部存在, 写死的 29999 已消失。
未完成：页面现在跑在 PC 上, 地址是 http://192.168.77.125:8080/ ; 拖动跟手度最终仍需用户目视确认。
下一步：等用户在浏览器里刷新(拿新版页面)后试连接面板与实际手感。

## 2026-09-20
完成：在 `D:\Today\interface-adapter\SDK\` 落地了 GenceGripper SDK（产品 = 转接板 + 自研夹爪，仓库 gence-gripper.git）。交付物：`gence_gripper/{__init__,gripper,registers,enums,errors}.py` + `transport/{base,serial485,toolport,mock}.py` + `examples/01~03` + `tests/test_mock.py`、`tests/test_toolport_offline.py` + `docs/GenceGripper-SDK.md` + `pyproject.toml`。三种接法 `on_serial/on_toolport/on_mock`。**验证（都是离线）**：① `python tests/test_mock.py` → **23 PASS / 0 FAIL**：CRC-16/MODBUS 标准向量（01 03 00 00 00 0A → CDC5）、与 docs/485命令全表.md 的 4 条示例帧逐字节一致、读区间/写白名单/危险地址/连续写校验、安全使能保持原位（不冲向 0）、力档 80→30、抓取/松开、失能后位置命令无效、闭合端 wait_until_stopped 不误判、一键标定、直读电机、LED、诊断；② 工具口联调 `mock_dobot_server.py --fake --listen-port 19999` + `python tests/test_toolport_offline.py 19999` → **7 PASS / 0 FAIL**（握手、分块读写、位置到位、未建模地址如实报错）。**未验**：串口路径没上过真板、工具口没连过真控制柜。
未完成：① 工具口帧终止符不一致：本仓库假 dashboard 按 ')' 切帧、不认分号 → SDK 加了 terminator 参数，默认 ';'（真控制柜实测带不带都收），离线联调传 ''。② 假 dashboard（tools/gripper_test/mock_dobot_server.py --fake）只模拟兼容页少数寄存器，标准区多数地址回无应答、也没有直读电机模型 —— 功能验证只能靠 tests/test_mock.py 的 Mock，别拿它当整机验证。③ docs/ 与 PROJECT.md 里那批「别名区 / AG95 / PGE」叫法未改（4 个文档 15 处），SDK 内部已统一叫「兼容页」。
下一步：真机上按 board_state → motor_online → endpoints → safe_enable → 小幅 set_position → grip/release 的顺序过一遍；对外发文档前先把 docs/ 与 PROJECT.md 里的厂商/型号名统一改成「兼容页」。

## 2026-09-20 · 台架测试
完成：按用户要求把网页上位机改成「USB-485 直连为主、越疆控制柜为可选小功能」。
**新增 SerialBus 类**(Modbus RTU, FC03/FC06/FC10, 128 寄存器上限分块, CRC16, 断链重开+冷却), 与 DobotBus/MockBus 同签名, 所以上层(Session/snapshot/_slow_pump/_pos_pump/所有 API)一行没改。
**默认值改动**: `--bus` 默认由 dobot 改为 **serial**; 新增 `--serial-port`(默认 COM21)/`--serial-baud`/`--serial-timeout`。
**踩到并修掉一个坑**: 第一版 `_recv_frame` 写 `read(256)`, 而应答只有 7 字节 —— pyserial 的 read(n) 会阻塞到收满 n 或超时, 于是**每条命令白等满 20ms**, lat_avg 实测 30ms。改成 `read(self.ser.in_waiting or 1)` 后掉到 **2.1ms**。这正是 PROJECT 里记过的同名坑。
**实测(本机 PC + COM21 + 真实板子)**: 每条命令往返 **2.1ms**; 各 API 15 次取中位 —— status **3.5ms**、move(只位置) **3.2ms**、move(位置+速度+力) **5.7ms**、param 3.6ms、pos(异步) **1.2ms**(对照走控制柜: 57 / 72 / 116ms)。偶然最大 25~28ms 是撞上后台慢变量刷新, 可接受。
**功能与安全回归(串口模式)**: 使能/失能/移动/拖动轨迹都正常(移到 700 停在 699、拖到 500 停在 500、12 点轨迹回放终值 304≈300); 一键自动标定跑通(cal_state=2, 行程 1625, 端点 0/1625); 上一轮的所有修复在串口模式下同样成立 —— 危险寄存器 0x0100/0x0034 返回 400、安全地址 0x0038 返回 200、畸形 JSON 与数组体都返回 400、/api/read 正常。
**前端**: 节拍改为由后端下发(poll_ms/drag_ms), serial=100/40、dobot=200/80。
**文档**: gripper_web.py 模块说明与 README 全部改成「USB-485 主用 + 控制柜备用」, 参数表更新, 「停 leap-os 开发栈」标注为只对 dobot 生效。
未完成：后端现在**跑在 PC 上**(COM21 在这台机器), 不在 77.11 —— 所以网页地址从 192.168.77.11:8080 变成 **http://192.168.77.125:8080/**。这一条要同步给用户; 以后如果要把 USB-485 挪到 77.11, 后端的启动命令与 README 也要跟着挪。另外: 拖动的手感最终还得用户在浏览器里目视确认。
下一步：等用户刷新页面( http://192.168.77.125:8080/ )实测拖动跟手度; 若还想要更快, 可以再压 poll_ms/drag_ms 的串口档位。板子侧与后端侧都已全绿。

## 2026-09-20 · 台架测试
完成：电机接上后(需**复位一次**才会重新扫描 —— 开机扫描只在复位那一刻跑, 电机后上电不会被发现; 复位后 0x0018=1、直读 0x0040=0x0A 立刻应答)。用 COM21 直连板子把剩余项全部验完。
**全量套件**: tmp/test_all.py COM21 -> **85 PASS / 0 FAIL**。⚠️ 一开始有 4 个 FAIL 全是**脚本过时**, 不是固件错: ① 三条断言还在按旧的「力档 20..100」判(固件早已按决定改成 0..30 截断, 写 40/60 回读 30 是**正确行为**); ② 一条期望端点 ~1595, 而 p_m 坐标统一后实测是 **1624/1625/1625(±1, 三次重复)** —— 差的正是 0x50(p_m) 与 0x51(xout) 之间的传动柔性 ~0.03rad。两处都已按新契约更新并注明原因。
**M1 广播(6 PASS)**: 广播帧后紧跟单播帧时**只回 1 个包**、广播**确实被执行**(0x0065 计数 +1、目标寄存器真被写); 回归面通过: 广播单独发=0 包、广播连发3帧=0 包、单播x2=2 包、四帧混排=2 包。
**M3 标定中断(6 PASS)**: 标定进行中写 0x0100=0 与 0x0000=0 **都被拒**(0x86/0x02), 标定照常跑完(0x0035=2, 5.3s)并正确落盘(0/1624)。
**S2 过温(用 gdb 造残留高温)**: 在**失能状态**下把 g_motor.t_rotor 塞成 95℃(失能时电机不回帧, 不会被覆盖), 再走安全使能 —— 结果 0x0201 **持续保持 1**(5 秒无反弹)、0x0015=1(不是 0x13)、0x0013 由 95 变回真实的 32。**修复前这里会在 1ms 内被残留值立刻失能并重起 10s 冷却。** 另验了冷却标志清理: 使能状态下塞入一个已过期的 g_motor_ovt_until, 300ms 内被清掉(0x0015 恢复 1)—— 旧代码清理写在 else(失能)分支里, 使能时会永远卡在 0x13。
**M2 位置累积基准**: 把 s_pos_prev 故意塞成 3.0(=1 真实位置 1.62 明显不同), 走安全使能(直读 0x50 -> 写 0x0002 -> 使能)。结果 **0x0202 = 997(真实位置, 正确)**; 若基准没同步, 第一帧会算出 1.6167-3.0 的假增量、回读掉到 ~146。gdb 回读确认 **s_pos_prev = 1.62005234**, 与 g_motor.pos_abs 逐位一致, 证明已同步。
**灯效**(上一轮已验): 待机 = 纯蓝 rgb=0,0,193 / ccr=0,0,756。
未完成：只剩**上位机(网页 HMI)那批修复没有真机验证** —— 控制柜 192.168.5.4 仍不可达, 后端起不来, 目前只有 --bus mock 的离线验证。
下一步：控制柜恢复后: 重启后端并复验 /api/status、/api/move、拖动、以及危险寄存器封禁与畸形 JSON 返回 400。板子侧已全绿、可收工。

## 2026-09-20 · 台架测试
完成：用户接了 USB-485(枚举为 **COM21**, 与 PROJECT 里记的本地台架口一致), 于是用本机当 Modbus RTU 主站直连板子, 把「需要 485 主站才能验」的那批固件改动全部测掉。
**新增两个可复用脚本**: tmp/regress_485.py(24 项) 与 tmp/m1_regress.py(6 项), 合计 **30 PASS / 0 FAIL**。
**M1 广播判定(本轮修复的核心, 之前完全没法测)**: 造出「广播帧后面紧跟单播帧」这个正是修复前会误回包的场景 —— 实测**只回 1 个包(单播那个)**, 且广播**确实被执行**(0x0065 广播计数 0 变 1、0x0038 真的被写新值)。回归面 6/6: 广播单独发=0 包、**广播连发 3 帧=0 包**(这是原来就通过的用例, 改动没弄坏)、广播+单播=1、单播+广播=1、单播x2=2、四帧混排=2。
**M10**: 写 0x0016=65535 得到异常 0x86/0x02 且变量保持 100; 写 100 正常且回读 100。
**白名单**: 写只读 0x0202 -> 0x86/0x02; 写不存在 0x1234 -> 0x86/0x02; 写合法 0x0037 -> 正常。
**M9**: 0x0105 / 0x0109 均可写。**新寄存器**: 0x001F 可读、0x0089=354us、0x010A 可写。**灯效配置**: 0x0037~0x003F 全部可读, 0x0039=0xAB64(色相 171=240度=蓝)。
**链路质量**: 0x0063 UART溢出=0、0x0064 CRC错=0(94 帧)、0x0018 开机扫描=0、0x0015=0x0F(CAN 发不出去)。
**一个测试脚本坑(易重犯)**: 用 FC06 写寄存器时, 异常回复的功能码是 **0x86**(即 0x06|0x80), 不是 FC03 的 0x83。第一版断言按 0x83 判, 把 3 个完全正确的响应误报成 FAIL。
未完成：**电机不在线**(用 0x0040 直读电机 0x0A, 8 轮无应答 —— 这是判电机在不在的唯一正解), 所以 S2 过温冷却、M2 位置累积器、M3 自动标定 这三项仍无法验证; test_all.py 的 D/E/F 组同样跑不了。上位机那批修复仍只有 mock 验证。
下一步：接上电机后(或控制柜恢复后): 1) 跑 tmp/test_all.py COM21 全量(61/61 基准); 2) 重点复验 S2 —— 制造一次过温, 确认冷却后能正常再使能(不再白等 10s 或锁死); 3) M2 用「手动标定拖爪后使能, 读 0x0202」复现; 4) M3 用「标定跑到一半写 0x0100=0」确认被拒且不写 NVM。

## 2026-09-20 · 台架测试
完成：用户要求把手头这块转接板当验证对象(J-Link 已插回**用户的 PC**,不是 77.11; 控制柜 192.168.5.4 仍不可达)。**已在板上完成验证**。
**烧录**: 本机 J-Link 用 stm32-dev 技能脚本 `flash --elf build/interface-adapter.elf --device STM32G431CB` —— 成功且逐字节校验 `VERIFY: 板子固件与 ELF 一致(40228 字节)`。⚠️ 77.11 的 flash.sh 现在用不了(那里已经没有 J-Link 了), 烧录改回本机。
**板子健康**: RTT 抓 8s —— `seq=1..168` 连续无跳变、tick 每 100ms 稳定递增、**无 HardFault**; 状态 `en=0 mode=4 id=3 err=15`(0x0F=CAN 发不出去, 裸板无对端时的预期值)、`rxB/frame/ore/crc` 全 0(无 485 主站)。`nm` 确认 `damiao_tx` 在符号表里(S3 确实进了二进制)。
**待机灯效在板上实测确认**(用固件自带的编译期自检 `LED_DEMO=1` + RTT 打 `eff/rgb/ccr`, 每格 4s): **IDLE rgb=0,0,193 ccr(红,绿,蓝)=0,0,756** —— 纯蓝、红绿通道为 0, 与 RELEASE(rgb=6,0,128 / ccr=23,0,501)同一个色相。其余各格也对: HOLD 绿(rgb=1,3,112)、FAULT 红(73,0,0)、CAL 蓝白(255,72,72)、BOOT 白。顺手把 `nm[]` 里过时的 `IDLE-Rainbow` 标签改成 `IDLE-NeonBlue`。验完已把 `LED_DEMO` 关回 0 并重新烧录交付版(40228 字节, 再次校验一致)。
**M10 在板上实测**(gdb 直接调固件函数): 写 0x0016=65535 → `bridge_write` 返回 **-1**、`g_comm_timeout_ms` 保持 100; 写 100 → 返回 **0**、变量=100。改前 65535 会经 int16 符号扩展变成 4294967295 而通过下限检查。
未完成：**裸板上测不了的**: S2 过温冷却(需要真电机发热)、M1 广播判定/M9 寄存器写(需要 485 主站)、M2 位置累积器/M3 自动标定(需要电机反馈)。上位机那批修复仍只有 mock 验证。控制柜恢复前这些都验不了。
下一步：控制柜恢复后: ①重启后端 `cd web_gui && setsid python3 gripper_web.py --ip 192.168.5.4 --port 8080 &`; ②跑 `test_485_all.py --read-only` 与全量; ③重点复验 S2 —— 制造一次过温后确认能正常再使能(不再白等 10s/锁死)、以及 M1 广播(MB_BUF_SZ 连发场景)。

## 2026-09-20
完成：回答用户「docs/leap-robot-develop/robot 是不是大华的 SDK」——不是。① 它是自家 ROS2 ament_python 统一接口包（只出 server/gripper_server/dexterous_hand_server 三个节点，把 Dobot/Realman/Piper/Diana/LeRobot 抹平成同一套服务），厂商 SDK 是被【内嵌】进来的：dobot_api.py、RM_Base.dll+libRM_Base.so、libDianaApi.so、piper_sdk。② gripper/dh_ag|dh_pge 的 ag_gripper.py/pge_gripper.py 不是 SDK，是手搓 Modbus RTU 主机：只 import serial+crcmod，功能码 0x06 写/0x03 读、从站 0x01、CRC16-Modbus(0x18005 rev, init 0xFFFF)，寄存器硬编码 0x0100=165 使能 / 0x0101 力20-100 / 0x0103 位置0-1000 / 0x0104 速度(AG 0-1000, PGE 1-100) / 0x0105/0x0107/0x0108/0x0109 旋转 / 0x0200 状态 / 0x0202 回读；dh_ag95.md 只有 4 行接线说明。③ dh = 大寰(DH Robotics)，与大华(Dahua)是两家公司，全目录 grep「大华」0 命中。④ 顺带证实 PROJECT.md 记的工具口握手序列就是它的原样实现：DobotGripperBase._setup_modbus = SetToolMode(1,0) → SetTool485(115200,parity,1) → SetToolPower(1) → ModbusClose(0) → ModbusCreate(127.0.0.1,60000,slave_id,1)，parity 默认 'N'、slave_id 默认 1。
未完成：发现一条跨栈静默截断：上层 robot/main.py 默认 gripper_force=80 / gripper_velocity=500，走工具口直写 0x0101/0x0104；固件现把力档截断到 0..30，而上层自己的 in_range 是 20..100 → 写 80 【不报错】、静默只给 30。另：DobotGripperBase.read_pos() 返回缓存 gripper_state（初始化时读到的 0x0202 或 set_pos 的目标值），不是实测回读；PGE 分支速度默认 500 超出其 1..100 会 RuntimeError。均未实测、仅代码核对。
下一步：若上层以默认配置跑在转接板上：确认 gripper_force 到底该取多少（改 yml 配置 vs 接受固件截断到 30），否则现场会把「力只有 30」误判成固件坏了。

## 2026-09-20 · 台架测试
完成：用户要求两件事: ①待机灯效改成'运动中的蓝色'、去掉彩色霓虹; ②把上轮全量审查的修复点落地且不影响功能。**已完成并已编译/离线验证**。
**灯效**: LED_MODE_IDLE 由彩虹色相循环改为 render_neon(蓝), 色相取 0x0039 高字节、亮度仍用 0x0038; 交叉淡入(LED_XFADE_MS/hue_lerp/s_xfade_*)改造成'上一状态色相 -> 待机蓝'而非删掉, 所以既没有死代码告警、绿->蓝也不跳色; 0x003C 高字节(彩虹周期)废弃。led.h/led.c/485命令全表.md 注释同步。
**固件修复(改了 6 个文件, make clean && make 成功、自有代码 0 告警)**: S3 新增 damiao_tx() 作为 CAN 入队唯一出口并用 PRIMASK 包临界区(6 处调用点全部改走它); S2 过温判定改成'只在使能后已回过至少一帧反馈(g_rx_fb_cnt 变化)时才拿温度判', 并把清 g_motor_ovt_until 收敛到 CheckTemp 一处、Damiao_Enable 不再清(同时消掉 RT-2 那条绕过冷却的竞态); M1 modbus 广播判定改用移位前取出的 frame_addr; M2 失能读 0x50 时同步 s_pos_prev/s_pos_prev_valid; M3 见 decisions; M9 0x0105/0x0109 补 g_p_des_valid; M10 0x0016 改按 uint16 取值并加 60000 上限; M15 链接脚本 FLASH LENGTH 0x20000->0x1F800 给 NVM 页留位。hex 已 scp 到 77.11。
**上位机修复(gripper_web.py/index.html, 已上传 77.11)**: S1 默认封禁 7 个危险寄存器+--allow-dangerous-write; M4 safe_enable/disable 同步 _last_en; M5 post() 在拖动期间不再 poll(); M6 ERR_TEXT 按固件/文档重写(补 9/A/B/C/D/E/0x13, 纠正 8=超压); M7 _body 畸形 JSON 抛错->400(原来会被当成 position=0 真的关到全闭); M13 set_param 返回真实 ok; G1 慢变量兜底改用 _ok() 判'整组是否全 None'(原 or 写法因 [None,..] 是真值而永不生效); G3 直读补校验 rr[2]==0x50。
**验证**: 主机回归 56/56; arm-none-eabi 干净构建 0 告警; 上位机用 --bus mock 离线实测 —— 7 个危险寄存器全部 HTTP 400、安全地址(0x0037/0x0038/0x000B/0x0039)仍 200、三种畸形体全部 400、合法 {} 仍按既有语义执行、/api/status 与 /api/param 正常。
未完成：**固件没烧进去**: 77.11 上 J-Link 已从 lsusb 消失(会话中段还在 Bus 001 Device 015/Segger), flash.sh 报 'No J-Link device found'。**上机复验也做不了**: 控制柜 192.168.5.4 从 77.11 ping 100% 丢包、'没有到主机的路由'(77.11 的 enp1s0 192.168.5.100/24 还在)。两边同时消失, 判断是现场被断电/拔线。因此灯效与全部固件修复目前只到'编译通过+已上传 hex', 未上机。上位机修复已部署到 77.11 但真机后端起不来, 只做了 mock 验证。
下一步：等现场恢复: ①把 J-Link 插回 77.11 -> 跑 cd ~/projects/jin/gripper_test && ./flash.sh (hex 已在同目录); ②控制柜可达后重启后端 `cd web_gui && setsid python3 gripper_web.py --ip 192.168.5.4 --port 8080 &`; ③目视确认待机是蓝霓虹、且过温后能正常再使能(不再白等 10s/锁死)。

## 2026-09-20 · 台架测试
完成：**全量代码审查**（用户要求用 Agent Teams）。4 域：固件实时/通信（fw-rt）、固件应用/寄存器（fw-app）、上位机（host）、文档一致性+客观回归（Lead）。三份细报告 + 一份总裁决落在 .work/review-2026-09-19/。**Lead 的客观验证**：主机测试 56/56；固件 make clean && make 成功且**自有代码 0 告警**；代码写白名单与 docs/485命令全表.md §5.2 **45=45 逐项一致**；bridge_write 实现与白名单也是 45=45（无死代码、无静默失效）；全工程只有 modbus.c 的 0x06/0x10 两处进 bridge_write，**无白名单旁路**。**三条 🔴**（Lead 均逐行复核成立）：① **S1** 上位机 /api/write 是任意 485 寄存器直写口 —— write_raw 零校验(gripper_web.py:845-848) + 路由透传(:948-950) + 默认绑 0.0.0.0(:985) + 无鉴权无 CSRF，可一步绕过项目“先读 0x50→写 0x0002→给速度→才使能”的铁律，还能改零点(0x0007)、反复写电机 flash(0x0008)；② **S2** 过温冷却结束后首次使能被【残留温度】立刻再触发（damiao.c:153-156 只清 g_motor_ovt_until 不清冻结温度；1kHz 里的 CheckTemp 使能后 ≤1ms 又判 ≥90℃）—— 每次恢复至少白等 10s，最坏（电机来不及回帧刷新温度）无限重起冷却、**只能复位**；③ **S3** CAN 入队可重入 —— HAL_FDCAN_AddMessageToTxFifoQ 读 TFQPI(:2140)→写 RAM(:2143)→置 TXBAR(:2146) 三步无临界区，却被 TIM6 ISR(5)/主循环/FDCAN ISR(13) 三处调用，主循环发失能帧时被 TIM6 打断可能让**失能根本没发出去**。**七条 🟡 中 Lead 逐行复核成立**：广播判定用了移位后的 mb_frame[0](modbus.c:527)；失能读 0x50 不同步 s_pos_prev 导致 pos_abs=2P−A(damiao.c:357-363)；自动标定只有 AC_SEEK_* 有安全门、被打断仍 Nvm_Save(autocal.c:299-345)；_last_en 只有 snapshot 更新；拖动中 post() 内部无条件 poll() 抵消了暂停；ERR_TEXT 整表错位（8 标成过载实为超压、新加的板子过温 0x13 根本没有）；坏 JSON 被当 position=0 真的关到全闭。
未完成：**一条都没修**（本轮是审查，用户只要求检查）。M9~M15 未逐行复核，按队友报告原样列出。所有并发/时序结论均为代码推导，未做动态复现。
下一步：等用户点单再修。建议顺序：S1(加白名单+绑本地+token) → S2(过温时清残留温度，**不改会锁死**) → S3(CAN 入队加临界区) → M1/M2(各一行) → M3 → M4~M7(上位机四条)。

## 2026-09-19 · 台架测试
完成：用户要求「改一个电机参数试试」，照纪律做完并拿到明确结果。**先存档电机全部原值**（用已有的 485 直读口 0x0040/0x0041/0x0042/0x0044 逐个读）：ACC=20.0 DEC=-20.0 MAX_SPD=300.0 CTRL_MODE=4 Gr=7.0 PMAX=12.566 VMAX=50.0 TMAX=5.0 I_BW=1000 **KP_ASR=0.0023 KP_APR=54.0 KI_APR=0 KD_APR=32.0** Filter_BW=1.0。测基线：+50 步进 179→229 用 **505ms**，轨迹 112ms=192/168ms=206/224ms=215/280ms=220/336ms=222/505ms=228（一阶趋近，τ≈120ms，且与距离无关）。然后经 gdb call `Damiao_WriteReg(0x1B, 0x42D80000)` 把 **KP_APR 54→108**：+50 到位 **336ms**（112ms 时刻进度由 26% 提到 52%）。再推到 **216**（0x43580000）：+50 到位 **169ms**，112ms 时刻已走 84%，反向 -50 轨迹 284/274/275 也约 168ms 到位、**无过冲无振荡**。**合计快 3 倍。** 全部只写电机内存、未调 Damiao_SaveParams、未碰 flash。收工时已失能、`pgrep -x openocd` 无残留、后端已重启。
未完成：KP_APR=216 目前只在【电机 RAM】里 —— 电机 24V 一断电就退回 54，现像会「莫名变慢」。要固化三条路：①固件在启动时下发（编译期常量，可版本控制、无 flash 磨损，需改固件+重烧，**推荐**）；②调 Damiao_SaveParams() 写电机 flash（寿命约 1 万次，一次性可以）；③在 485 上开放通用写寄存器口，每次连接时下发。另：216 是在空载、力档 20 下测的，**带载夹持时是否啸叫/振荡/温度升高还没验**。
下一步：问用户怎么固化：建议走①固件启动下发，把 KP_APR 常量放 damiao.c 初始化里（顺带可让 KD_APR 一起可调）；固化后带载复测钳口夹持时的噪声与温度。若要继续找上限可再试 432。

## 2026-09-19 · 台架测试
完成：回答用户「能不能再低一点延迟」，把夹爪延迟彻底分段量化，得到决定性结论：① **命令链路 56ms/条确认无解** —— 同一条读命令走 dashboard 是 56ms，绕开 dashboard 直连控制柜 60000 口发**裸 Modbus RTU**（`01 03 0200 0001 <CRC>`，收 `01 03 02 0003 <CRC>`）也是 56.1ms；两条独立路径同值，证明开销在控制柜转发工具口 485 的节拍上。② 顺带纠正一个此前的误判：带 `0,{1000},` 前缀的命令会立刻返回错误码 -10000（0.8ms），**不带前缀才是对的** —— 之前正是拿这个错误回复当成了「裸 socket 只要 1ms」。③ **控制柜能批处理**：连发 16 条写只要 101ms（逐条要 900ms），N 条总耗时基本不随条数增长。④ **真正的大头是伺服到位时间**：力档 20 下实测 +50 / +200 / -250 三步，到位时间 655 / 656 / 708ms —— **4 倍距离用时几乎一样**，且轨迹呈一阶指数趋近（176ms 走完约 30~36%、416ms 走完约 90%），时间常数约 200ms，与距离无关。⑤ 查固件找到机制：`damiao.c` 用的是**模式 4（力位混控，0x300 帧）**，该帧只有 `p_des / v_des(限速) / i_lim`，**不含 kp/kd**（kp=5.0/kd=0.1 的编码是模式 1 MIT 帧 0x000 才用的，见 damiao.c:261-271），所以位置环由电机内部 `KP_APR=54` 决定、`v_des` 几乎不起作用 —— 这同时解释了长期悬而未决的『速度设置不生效』（写 0x0104=100/300/1000 全行程耗时 1353/1272/1253ms 几乎一样）。结论：**拖动手感的主要延迟是这 650ms 的伺服到位，不是网页、不是后端、不是控制柜**，要再快必须动电机的位置环参数。
未完成：伺服位置环增益怎么调还没做：需要给固件加「写电机寄存器」的 485 接口，再用它调 `KP_APR`/`KD_APR`（或评估切到模式 1 MIT 并想办法保留抓取力限幅 i_lim —— 模式的取舍：模式 4 有 i_lim、MIT 有 kp/kd，二者不可得兼）。另：批处理（不等回执连续下发 + 前端采样加密）尚未在后端实现。
下一步：加固件 485 写电机寄存器接口（建议 0x0045=寄存器号 / 0x0046+0x0047=32 位值小端），先用它读回并试调 KP_APR 看 +50 步进到位时间能否从 655ms 显著下降；改前必须确认电机参数写坏后的恢复路径（先记下 KP_APR/KD_APR 原值）。

## 2026-09-19 · 台架测试
完成：沿「网页上位机延迟大」继续深挖，找出并修掉三个真实缺陷（都实测对照）：① **直读电机位置地址错位** —— 固件是 0x0041/0x0042=值(低/高)、0x0044=状态，后端却写成 VAL=0x0044、OK=0x0045，导致「等状态==1」永不成立：安全使能每次空转满轮询、并且【从没真正读到过电机当前位置】、一直走降级分支（这是安全性问题，不只是慢）。改成一次 GetHoldRegs(0x0041,4) 拿全，日志实测读回 {29392,16249,80,1} → float 拼出 0.9744rad、状态 1、寄存器号 80(0x50) 全对。② **慢变量整组占锁** —— 原来 /api/status 里同步读端点/标定/角度/力矩温度/诊断 共 4~6 条命令(250~400ms)且整组占 self.lock，用户操作撞上就等：同一个 /api/disable 实测在 74ms 和 623ms 之间随机跳。已挪到后台线程 _slow_pump，每块单独抢锁。③ **慢变量索引越界** —— 顺带发现慢变量里「标定」那组从 0x0034 起读 3 个，而 0x0034 是【只写】的启动寄存器、读它返回 -1 让整条命令失败重试(白丢 56ms/2s)；改成读 0x0035~0x0036 但要补占位 None 保持 ac[1]/ac[2] 语义（我先漏了这步，服务一度返回 list index out of range，加了 traceback 才定位）。**耗时对照（都实测）**：/api/status 12s(分号误判)→0.3s→**57ms/1 条**；enable 3455ms→1168ms→**351ms**；move 172→60→**72/116ms**；disable 74~623 随机→**59ms 稳定**。另实测确认：**所有单条 dashboard 命令（纯读/单写/双写/失能）统统 56ms**，读 1 个和读 4 个寄存器一样 —— 56ms 是控制柜转发工具口 485 的固定节拍，与本条命令内容无关，所以提速只能靠减少条数。前端轮询也从 500ms 放宽到 200ms（后端单次只剩 56ms 一条命令）。
未完成：用户在浏览器里目视复验（Ctrl+F5 刷新拿新 index.html）；「每条命令 56ms」这个节拍本身没法再降，除非绕开控制柜直连 485。
下一步：请用户刷新页面后复验拖动跟手度、按钮响应、以及状态显示速度；若仍嫌慢，评估 USB-485 直连方案（绕开控制柜那 56ms/条）。

## 2026-09-19 · 台架测试
完成：**排查「速度设置不生效」**（用户反馈），查到达妙电机的运控参数这一层：① 实测：写 0x0104 = 100/300/1000，寄存器都正确读回，但**全行程耗时几乎一样**（1353/1272/1253ms）→ 确实不生效；② 用 0x0040 直读电机寄存器，拿到关键参数：**ACC=2.0 / DEC=-2.0 / MAX_SPD=300 / CTRL_MODE=4 / Gr=7 / PMAX=12.566 / VMAX=50 / TMAX=5**。手册明确（力位混控 0x300 帧）：`V_des 是限速值，单位 rad/s，放大 100 倍，uint16，范围 0-10000` —— 固件 damiao.c 的填法是对的；但手册另有一节说「位置速度模式除 v_des 外，另有加/减速度要设定」，寄存器表里 **0x04 ACC / 0x05 DEC / 0x06 MAX_SPD 都是 RW float**，**ACC 实测只有 2 rad/s²** —— 按这个加速度跑完 1.6rad 约需 1.2s，和实测 1.27s 吻合。③ 用 gdb 远程调 `Damiao_WriteReg(4, 0x41A00000)` / `(5, 0xC1A00000)` 把 ACC/DEC 改成 ±20（只写 RAM，未存 flash），读回确认 ACC=20；但再测全行程仍是 764/1092/1075ms（速度 100/300/1000）—— **仍无明确差异**。结论倾向：**模式 4 的实际运动快慢不由 v_des 或 ACC/DEC 决定**（或 gdb 只改了 RAM、电机侧另有机制），需要加一个 485 接口写电机寄存器，让现场能直接调/验证，或改用达妙调试助手。④ 本轮又踩一次同一个坑：`pkill -f openocd` 会把板子留在 halt（485 全不应答、/api/enable 返 500），必须用 `openocd ... -c 'init; reset run; shutdown'` 救回来 —— **这已是第二次**。
未完成：速度不生效的真正机制未定：v_des 是否被模式4采用、ACC/DEC 是否只在位置速度模式(模式2)生效、以及是否必须「保存参数」才进电机 flash —— 都没验证。测量精度受限于每次读寄存器 60ms，快慢差 <300ms 时分辨不出。
下一步：给固件加「写电机寄存器」的 485 接口(如 0x0045=寄存器号 / 0x0046+0x0047=32位值)，让现场能直接调 ACC/DEC/MAX_SPD 并观察；另外确认「失能」是否真只需要一条 0x0100=0(现在实现就是一条)。

## 2026-09-19 · 台架测试
完成：**解决「快速来回滑三次、夹爪只动一次」**（用户关键反馈）：根因是上一步为修「按钮被轨迹盖回去」而在 move() 里加的 self._trace.clear() —— 泵每播一个点就调 move()，于是每播一个点就把剩下的整条轨迹清空，中间过程全丢。改成 move(..., from_pump=False) 区分：外部绝对命令（按钮/执行）清轨迹，泵自己的回放调用(from_pump=True)不清。**实测对照**（12 个互不相同的点，间隔 20ms）：修复前只下发 **3 条**；修复后下发 **完整 12 条**且顺序一致。来回 3 次（900/100 ×3）的下发序列尾部 = 900,100,900,100,900,100 ✓ 三个来回都在。按钮仍能盖过轨迹（点张开 → 位置 1000 ✓）。同轮还完成了**分段对照测量**（用户要求分开测哪个链路最耗时间）：裸 socket 直连控制柜 = 读 56.2ms / 写 56.1ms；经上位机 = /api/status 58.1ms、/api/move **60.2ms**（修复后）；纯 HTTP <1ms、/api/pos 0.5ms → **上位机开销仅 2~4ms**。顺带修掉一个大头：/api/move 原来 172ms —— 因为 do_POST 结尾的 invalidate() 会清空快照缓存，而 move() 里的温度/使能检查依赖它，于是每次真读 2 条命令(+112ms)；改用不随 invalidate 清空的 _last_en/_last_temp 后降到 60ms。
未完成：跟手程度仍需用户在浏览器目视确认；控制柜↔板子的 56ms 是外部硬下限。
下一步：用户刷新页面后按「快速来回滑三次」复验，应能看到完整来回。

## 2026-09-19 · 台架测试
完成：**修掉用户指出的三个问题**（都实测通过）：① 轨迹对不上/延迟 → BUF_SEC 0.4→**0**（直接播最新点，最跟手）；② 点「张开/闭合」不对 → 根因是后台轨迹泵还在回放拖动残留的旧点、把绝对命令盖回去；改在 move() 里先 self._trace.clear()。实测：拖到 600 后立刻点张开 → **位置 1000** ✓，再点闭合 → **6** ✓；③ 实时显示假跳变 → 删掉前端 optimismDrag() 与 optimistic() 里对 s-pos/s-bar/s-state 的写入（那是目标不是实际值，先跳过去再被回读改回来）；实际值只由 /api/status 驱动。另外给泵加了「目标与上次相同就跳过下发」省总线。实测拖动：拖到 600、等 0.6s 后位置=600 ✓。
未完成：跟手程度最终仍需用户在浏览器里目视判断；写命令 172ms 仍是控制柜给的硬下限。
下一步：用户刷新页面(Ctrl+F5)后拖滑块 + 点按钮实测手感。

## 2026-09-19 · 台架测试
完成：**继续压拖动延迟，抓到并修掉泵的节奏 bug**：① 端到端实测：`/api/pos` 本身 0.6~1.1ms；但一次「目标→到位」整体约 1000ms，其中**大部分是我测量用的轮询开销**（每读一次 0x0202 要 56ms）。② 模拟拖动（每 80ms 一个目标、共 15 个）→ 发完立即读位置 443（滞后），0.5s 后 758（跟上）。③ **发现泵的硬伤**：`while True: time.sleep(0.15); move()` —— sleep 与 move 串行，实际下发周期 **322ms** 而不是 150ms。已改成「有新目标立刻下发、空闲才 sleep(0.05)」，周期回到 move 本身的 172ms。④ 修复后验证：每 40ms 连发 25 个来回目标，**最终精确停在 159（算出来是 160）** ✓。⑤ 前端另加：**拖动期间暂停状态轮询**（状态采样 56ms 会和下发抢同一把锁，叠加后页面数字和动作都更慢），松手 400ms 后恢复。
未完成：拖动流畅度仍未在浏览器里实测手感；**写命令 172ms 是控制柜给的硬下限**，所以夹爪跟随不可能快过约 0.2~0.35s。要再快只能绕开控制柜直连 485。
下一步：用户刷新页面(Ctrl+F5)后再拖一次；若仍不满意，评估 USB-485 直连方案。

## 2026-09-19 · 台架测试
完成：**解决「网页上拖滑块一卡一卡」**（用户关键反馈：「就算有 100ms 延迟，来回拖动也应该能跟着来回开合」—— 说得对，这不是延迟问题而是架构问题）：原实现是前端「发一条 move → 等 172ms 的写命令 → 再发下一条」，所以拖动时位置只能一跳一跳。改成 **前端只把「最新目标」丢给后端（POST /api/pos，只更新 self._want_pos，0.7~1.1ms 返回）**，由 **Session 启动的后台守护线程每 150ms 取当时最新的目标下发给板子** —— 中间过期的位置直接丢弃，不积压、永远跟手。实测：连发 20 个目标（间隔 50ms）后，板子精确停在最后一个目标（480）✓。同轮还做了：① **识别夹住不稳** → grip.c 新增 GRIP_DEB_OFF_MS=300u，退出「夹住」的去抖由 100ms 拉长到 300ms（进入仍 100ms），已烧录；② 排查中发现并修复一个**自己造成的故障**：调试后直接 `pkill -f openocd` 把板子留在 halt → 485 完全不应答（读 0/20、move 1.6~2.1s），用 `openocd ... -c 'init; reset run; shutdown'` 复位后恢复 20/20、56~72ms；③ 换短 485 线后单条命令仍是 56~72ms（读）/172ms（写），**延迟与线长无关，指向控制柜**。
未完成：拖动流畅度只做了接口层验证，**未在浏览器里实测手感**；识别稳定性也要用户夹东西复验。控制柜侧 56ms 轮询周期的根因仍未知（建议重启控制柜）。
下一步：用户刷新页面(Ctrl+F5)后拖滑块试手感；若仍不够，考虑绕开控制柜直连 485。

## 2026-09-19 · 台架测试
完成：**处理用户三个反馈**：① **「拖动不流畅」** —— 前端从「等上一个 move 回来再发下一个」改成「**每 120ms 定时发最新值 + 最多 2 个请求在飞**」：一次写命令 60~600ms 时，原来拖 2 秒只动两三下，现在能持续跟手又不至于堆积失控。② **「识别夹到特别不稳」** —— grip.c 新增 `GRIP_DEB_OFF_MS=300u`，**退出「夹住」的去抖从 100ms 拉长到 300ms**（进入仍是 100ms）。夹住后力矩会围着阈值波动，退出同样用 100ms 就灯忽绿忽蓝；退出迟钝 3 倍后一旦判成就稳得住。已编译烧录。③ **「延迟还大」** —— 用户新接的 485 线（说比原来近）实测单条命令仍是 **56~72ms**，没改善；排查中还发现一个**我自己造成的故障**：调试测试后直接 `pkill -f openocd`，**板子被留在 halt 上**，表现为 485 完全不应答（读寄存器 0/20 成功、move 1.6~2.1s）。用 `openocd ... -c 'init; reset run; shutdown'` 复位后立刻恢复 **20/20 成功、56~72ms**。⚠️ 教训：调试完必须 detach/resume 再关 openocd，直接 kill 会把目标 halt。
未完成：拖动流畅度只改了前端节奏，未在浏览器实测；识别稳定性也要用户夹东西复验。链路 56ms（vs 历史 20ms）的原因仍未定，用户换了更短的线也没变，指向控制柜侧。
下一步：用户刷新页面(Ctrl+F5)后试拖动与识别；若延迟仍大，建议重启控制柜或改直连 485。

## 2026-09-19 · 台架测试
完成：**验证「本地编译 → 上传 → 77.11 远程烧录」并确认能远程调试**：① 烧录流程就是用户说的那样：PC 上 make → scp hex 到 77.11 → 在 77.11 跑 `./flash.sh`（openocd+SWD，已实测 Verified OK）；② **远程 GDB 调试实测可用**：77.11 上起 `openocd -f interface/jlink.cfg -c 'transport select swd' -f target/stm32g4x.cfg -c 'bindto 0.0.0.0' -c 'gdb_port 3333'`，PC 上用 STM32CubeCLT 的 arm-none-eabi-gdb 连 `192.168.77.11:3333`，实测 halt 在 bridge_autocal_tick、读到 g_motor.state/pos_abs/t_rotor、detach 正常。**关键坑**：openocd 的 gdb_port 默认只 bind 127.0.0.1，PC 连不上，必须显式 `-c "bindto 0.0.0.0"`。③ 已把两个脚本放到 `~/projects/jin/gripper_test/`：**flash.sh**（烧录）与 **debug.sh**（起 gdb server）。④ 注意事项：openocd 会独占 J-Link（调试时不能同时烧录）；调试 halt 期间 485 不响应。
未完成：openocd 的 RTT 没试（配置比 SEGGER 原厂工具麻烦），所以 RTT 打印这条路暂时用不了；77.11 仍未装 ARM 工具链（编译还在 PC 做）。
下一步：需要 RTT 的话研究 openocd 的 rtt setup/start；或把工程+工具链搬上 77.11 实现完全独立。

## 2026-09-19 · 台架测试
完成：**打通「J-Link 直接插在 77.11 上烧录」的完整流程**（用户把 J-Link 从 PC 挪到了机械臂主机）：① 环境：77.11 上 `lsusb` 能看到 `1366:0105 SEGGER J-Link`，但**没有任何 J-Link 软件、没有 ARM 工具链**；SEGGER 官网的 Linux 包**下不动**（curl 返回 202 但 0 字节，需要交互接受许可）；USB 节点 `root:plugdev` 带 ACL。② 方案：`sudo apt-get install -y openocd`（候选 0.12.0-1build2）—— 实测**普通用户直接跑 openocd 即可访问 J-Link，不需要 sudo**。③ **关键坑**：openocd 的 `interface/jlink.cfg` 默认 **JTAG**，必须加 `-c "transport select swd"`，否则报 `JTAG scan chain interrogation failed: all zeroes` / `Invalid ACK (0) in DAP response`。加了 SWD 后一次认到：Cortex-M4 r0p1 / `device idcode = 0x20036468 (STM32G43/G44xx)` / flash 128 KiB / VTarget 3.296V。④ 完整流程实测通过：PC 上 make 出 hex → scp 到 `/home/leaprobotics002/projects/jin/gripper_test/interface-adapter.hex` → 在 77.11 上跑封装好的 **`flash.sh`**（openocd + swd + program/verify/reset），输出 `Verified OK` / `Resetting Target`，**免 sudo**。⑤ 另：77.11 的 apt 里有 `gcc-arm-none-eabi 15:13.2.rel1-2`，装上就能在该机直接编译、不再依赖 PC。
未完成：77.11 上还没装 ARM 工具链（所以目前编译仍在 PC 做）；flash.sh 只覆盖烧录。
下一步：如果要完全脱离 PC：在 77.11 上 `sudo apt install gcc-arm-none-eabi`（还需 make），把固件工程也传过去。

## 2026-09-19 · 台架测试
完成：**排查「延迟更大了」**。实测结论：① 用项目原版 SDK 脚本 quick_check.py 对照 —— 耗时 **1.98s**（历史基准 1.05s），说明**不是上位机的问题**，整条链路慢了近 2 倍；② 拆分控制柜延迟：纯 dashboard 命令(GetErrorID/RobotMode/GetPose)只要 **0.6/0.9/4.3ms**、工具口配置命令 1.4~3.6ms，但经 485 的 **GetHoldRegs/SetHoldRegs 都是 ~56ms**（原来是 20ms）→ **慢的全在「控制柜↔转接板」的 485 转发上**，且读/写同值，是固定轮询周期变长；③ 板子侧完全健康：0x0063 UART溢出=0、0x0064 CRC错误=0、0x0017 跑飞=0、0x001B CAN发不出=0；④ 安静时刻连续请求：status 真采样稳 **60ms**、缓存命中 <1ms（无排队），但 **move(只发一条写命令) 在 72~574ms 之间跳** → 写命令的处理路径本身不稳定，不是排队；⑤ 查清一个**我自己引入的开销**：温度保护 _temp_guard 每次移动都多读一条 0x0013（+56ms），已改为优先用快照缓存。软件侧已压缩到：快变量 1 条命令、拖动 1 条命令、轮询 500ms、拖动请求串行化。
未完成：链路 20→56ms 的根因未定（控制柜侧，非板子/上位机）；写命令 72~574ms 的抖动也来自控制柜。需要用户配合：重启控制柜看是否恢复、检查 485 线缆/接头；另外用户浏览器要刷新(Ctrl+F5)才会加载 500ms 轮询的新前端。
下一步：让用户刷新页面后再评体感；若链路仍 56ms，建议重启控制柜或改走直连 485。

## 2026-09-19 · 灯光/台架
完成：① grip.c 的 GRIP_EXCL 由 40 收到 **30**（编译烧录完成）——用户现场薄件优先。已在注释里写明风险：空爪顶硬挡实测停在 25~34/1000，取 30 时空爪若停在 31~34 会假报夹住(绿灯常亮)，现场若误报就回调到 35~40。② 查清用户问的「保护怎么变成 65」：**65 是网页后端 gripper_web.py 里 Session.TEMP_LIMIT**（早前加的温度拒绝线），与固件 damiao.c 的 DAM_TEMP_LIMIT=90 不一致；已把后端改成 90 并上传重启，两处同源。③ docs/485命令全表.md §2.10 的排除区同步成 <30/>970，并补了取值由来/假报风险/物理上限说明。
未完成：排除区 30 未真机复验；空爪位 31~34 时可能假报夹住 —— 需要用户观察空爪静止时灯是否变绿霓虹来判定。
下一步：请用户确认：空爪静止时是否误报夹住；夹薄件是否已能识别。两者冲突时取 35 折中。

## 2026-09-19 · 灯光/台架
完成：修「即将闭合时夹住识别不到」：grip.c 的 GRIP_EXCL 从 50 收到 **40**（编译烧录完成），原因是实测空爪顶到闭合硬挡停在 25~34/1000，原 50 的排除区把「夹住薄东西」常见的 40~50 位置也盖掉了，判定条件「非排除区 + |T|≥阈值」就永远不成立。同步更新 docs/485命令全表.md §2.10：进入阈值 0.30→0.15、退出 0.15→0.08、排除区 <50/>950→<40/>960，并把「阈值宏在 grip.c 而不是 main.c」改对、补了排除区取值由来与局限说明。
未完成：未真机复验（需要用户夹薄东西试）；比空爪位(25~34)还薄的东西仍无法与空爪区分 —— 这是位置+力矩判据的物理上限，要再进一步只能做「动态学习空爪基准位」或加装夹持检测传感器。
下一步：请用户夹薄东西复看识别率；若仍不足，考虑把 EXCL 做成 485 可调或引入动态空爪基准。

## 2026-09-19 · 灯光/台架
完成：**按用户要求把夹持力限制到 0..30**（原 20..100），固件 + 上位机都改并烧录/上传：① 固件：gripper_map.c 的 gripper_force_to_torque / gripper_force_to_ilim / torque_to_gripper_force 三个函数全部改成 0..30 档，t_ff = value/30×1.2 N·m（0~1.2）、i_lim = value/100（0~0.3）、>30 一律截断；下限从 20 放宽到 0（0=不出力）。app.h 注释同步。② 上位机：前端力滑块 min/max 由 20/100 改成 0/30；后端 safe_enable / move / set_param 三处 clamp 全部由 max(20,min(100,..)) 改成 max(0,min(30,..))。**验证**：固件侧写 0x0101 = 0/10/30 分别读回 0/10/30，写 31/50/100/200 **一律读回 30**；后端 /api/param force=80 也被夹到 30。③ 文档三份（485命令全表.md / 转接板功能说明.md / 软件方案.md）的力范围、换算公式、示例帧全部同步 —— 示例帧重算成 `01 06 01 01 00 1E 59 FE`（写 0x0101=30）。温度 47/44℃。
未完成：标定用的 AC_FORCE 仍是 30（在范围内，力度不变）；灯效映射上限 LED_HOLD_T_HI=1.00N·m 与力上限 30（i_lim 0.3）配套，暂不动。
下一步：用户刷新页面确认滑块范围；若 30 还嫌大力可再降。

## 2026-09-19 · 灯光/台架
完成：按用户「更明显一点」的要求加大 HOLD 灯效对比度（已编译烧录）：① 亮度下限 LED_HOLD_V_MIN 25%→15%；② 亮度映射加**平方**（hold_torque_k() 归一化后 k*k 再映射）—— 轻夹压得很暗、重夹直接拉满；③ 新增**饱和度**随力矩变（LED_HOLD_S_MIN=130 → 255），轻夹偏淡绿近白、重夹纯绿；④ 映射上限 LED_HOLD_T_HI 1.20→1.00N·m（力 100% 顶硬挡实测饱和 1.10，取 1.00 才能到满亮）。render_neon() 现在多收一个 sat 参数（RELEASE 传 255，HOLD 传 hold_torque_sat()）。RTT 打印同步加了 sat: `[led] HOLD T=<0.01N·m> pct=<%> sat=<0..255>`。新映射（实测力矩对照）：0.10N·m→15%/130、0.50→30%/188、0.83(力20%)→71%/231、≥1.00(力100%)→100%/255。
未完成：仍未在真机目视确认（要夹住中间位置的东西才进 HOLD）；「是否够明显」最终得用户看了才算。
下一步：用户夹东西复看；若还要更夸张，可继续压 LED_HOLD_V_MIN 或换立方曲线。

## 2026-09-19 · 灯光/台架
完成：过温保护阈值按用户要求从 **80℃ 调到 90℃**（DAM_TEMP_LIMIT，damiao.c），编译烧录完成；docs/485命令全表.md 里那句「线圈或 MOS ≥80℃」同步改成 ≥90℃。冷却期仍是 10 秒。其余逻辑不变（保护触发后失能 + 冷却期内拒绝使能 + 0x0015 读回 0x13）。
未完成：90℃ 距离电机自身的 100℃ 保护只剩 10℃ 余量 —— 若现场仍出现「冲到 100 才停」，说明升温速率快过 10℃ 的反应窗口，得再往下调。
下一步：用户夹东西复看灯效与轻夹识别；若需要可把 90 再调。

## 2026-09-19 · 灯光/台架
完成：**按用户三个反馈改固件并烧录**：① **过温保护（新功能，安全项）**：control.c 的 1kHz 节拍里调 Damiao_CheckTemp() —— 线圈或 MOS ≥80℃ 立即失能、强制冷却 10 秒，冷却期内 Damiao_Enable() 直接返回（不发使能帧）；故障码不写 g_motor.err（会被失能前最后一帧反馈覆盖），改用独立标志 g_motor_ovt_until，0x0015 读时映射成 0x13。**验证方法（可复用）**：把 DAM_TEMP_LIMIT 临时改到 45℃（当前温度附近）重烧，实测 MOS 到 45 即失能、err=19、冷却期连发 165 两次都不上电、10s 后重试因温度仍=阈值又被立刻拍掉；验完改回 80 再烧。② **夹持阈值调低**：GRIP_T_HI 0.30→0.15、GRIP_T_LO 0.15→0.08（用户反馈轻夹识别不到）。③ **灯效映射收窄**：LED_HOLD_T_LO 0.20→0.10、LED_HOLD_T_HI 2.00→1.20 —— 力档顶硬挡实测饱和在 1.10N·m，原来上限 2.0 永远到不了满亮。另外实测发现电机自己会先报 err=0x0C(线圈过温，此时表面温度才 42℃)：它的热模型比温度传感器早动手 —— 这是电机行为，不是板子的。文档 485命令全表.md 已补 0x0015 的 0x13 码与「过温冷却期内 165 被拒」。温度全程 ≤51℃。
未完成：「力矩越大越绿」仍未在真机上目视确认（要夹住中间位置的东西才进 HOLD，空爪顶硬挡不算夹住）；轻夹的识别阈值 0.15 也未实测（需要用户夹软物验证）。
下一步：请用户夹个东西复看绿的程度与识别率；若轻夹仍偶尔识别不到，把 GRIP_T_HI 再降到 0.10。

## 2026-09-19 · 灯光/台架
完成：**固件新增「力矩越大越绿」**：HOLD(夹住) 灯效在原本的霓虹亮度上再乘一个力矩系数 —— led.c 新增 hold_torque_pct()（|g_motor.torque| 从 LED_HOLD_T_LO=0.2N·m 到 LED_HOLD_T_HI=2.0N·m 线性映射到 LED_HOLD_V_MIN=25%..100%），render_neon() 多收一个 scale_pct 参数（RELEASE 传 100，HOLD 传力矩系数）；HOLD 分支加了限频 200ms 的 RTT 打印 `[led] HOLD T=<0.01N·m> pct=<%>` 供核对。led.c 需 include damiao.h 才能拿 g_motor。编译通过、烧录成功（39996 B）。**验证受阻并查清原因**：空爪顶到闭合硬挡时**不会**进 HOLD —— 夹持判定有两端各 5% 的排除区(EXCL=50/1000)，位置 0 落在排除区内，所以 RTT 里一条 [led] HOLD 都没有（只有 stat 周期行）。**要验证必须夹住中间位置的东西**（需要用户配合）。实测各力档顶硬挡的力矩：力20→0.83N·m、力60/100→1.10N·m（已饱和），按映射分别对应 pct≈51 / 62。温度 40→51℃（持续顶硬挡发热，已失能降温）。**踩到一个值得记的坑**：`stm32-dev.py rtt --out temp/xxx.log` 在 temp 目录不存在时会报 `Could not open/create log file` 并**提前退出且不 resume CPU** → 板子停在 halt，表现为 485 全不响应（web 后端读寄存器全 None、失能返回 ok:false）。恢复办法：`stm32-dev.py reset --elf build/interface-adapter.elf`。跑 rtt 前先确保输出目录存在。
未完成：灯效只做了代码级验证，**没有真机目视/数据确认**（需要夹住中间的东西才会触发 HOLD）。
下一步：请用户夹住中间位置的东西（或手捏）复看绿的程度；同时可用 RTT 抓 [led] HOLD 的 pct 核对。若想 485 可调这三个阀值，再加 2~3 个配置寄存器。

## 2026-09-19 · 台架测试
完成：上位机：**位置滑块改成拖动实时下发**（松手再补一发带力/速度的完整参数，保证落点）。优化两点：① 拖动时只发 position（后端 move 在 velocity=None 时不再每次重写 0x0104，只在本次连接还没给过 v_des 时补一次 —— 避免复位后 v_des=0 只写位置不动，也避免盖掉用户设的速度）；② 前端从「固定节流 150ms」改成「始终只保持一个请求在飞、回来立刻发最新值」，拖动过程中不堆积、松手时的最终值也不会丢。**关键性能基准（本轮实测，很重要）**：经越疆控制柜工具口时，**读命令 GetHoldRegs 约 20ms，而写命令 SetHoldRegs 要 75~460ms**（抖动极大）——所以拖动跟手的上限由网关写延迟决定，不是我们的代码；把一次 move 从 3 条命令压到 1 条也只省约 40ms。实测拖动路径（只发位置）耗时 75/82/131/200/202/361/360ms，带全参数对照 258/459ms。温度 39~43℃，已失能。
未完成：拖动体感约 3~5 次/秒，受网关写延迟限制；若需要更跟手只能减少命令数或换直连 485（不现实）。PROJECT.md 的坐标坑条与两批固件改动仍未 git 提交。
下一步：git 提交固件改动并开 MR；用户刷新页面试用拖动。

## 2026-09-19 · 台架测试
完成：**按用户选择(B)给固件加了高分辨率位置寄存器并烧录验证**：只读 0x001F、可写 0x010A，单位「开度×10」(0..10000)，分辨率 0.1/1000 开度。改动：gripper_map.c 加 gripper_pos_x10_to_rad / rad_to_gripper_pos_x10，app.h 声明，regs.c 加只读 case + 写白名单 + 写处理（负值拒绝）。编译通过(39768 B)，J-Link 烧录+reset 成功。验证：写 0x010A=5000/5003/5005/2500 → 0x001F 读回 4991/4993/5002/2501，与直读 p_m 换算的真值差 −0.7~−1.8/1000；写 0x0103=600 → 0x001F=5998（两者一致）；越界 0x010A=20000 被截断到全开(与 0x0103 行为一致)。**关键结论（重要）：提高寄存器分辨率并不能提高物理精度** —— 实测伺服重复性就是 **±1/1000 开度**（同一目标从两侧各逼近 4 次：系统偏差 0.4/1000、极差 2.1/1000、无迟滞、10 轮往复无累积漂移），所以 0.1/1000 的指令差异被重复性噪声淹没（写 5003 实测落在 5011）。高分辨率寄存器的价值在于**读数更细**（上层能拿到 0.1/1000 的位置值），不在于让位置更准。本次烧录**不需要重新标定**（端点逻辑未变，仍是 0/1624）。温度 36~42℃。
未完成：PROJECT.md 里「两个坐标（别混）」那条仍写的是旧的 0x51 描述，未更新为「位置一律 0x50」；固件改动（坐标统一 + 高分辨率寄存器两批）都还没 git 提交。
下一步：git 提交两批固件改动并开 MR；把 PROJECT.md 坐标那条改成以 0x50 为准；告诉用户 0.1/1000 的读数可在页面「高级·原始寄存器」里读 0x001F。

## 2026-09-19 · 台架测试
完成：**按用户要求改固件：位置坐标统一到 0x50(p_m)，并烧录 + 重标定 + 验证通过。**改动：① autocal.c 的 bridge_autocal_ask51() → ask50()（读 0x50，变量 g_ac_xout→g_ac_pm），端点从此是 p_m 坐标；② damiao.c 失能时初始化 pos_abs 的条件 data[3]==0x51u → 0x50u；③ control.c 手动标定模式（0x0030）的 50Hz 问询 ReadReg(0x51u) → 0x50u。**这三处必须同时改**，否则手动标定会因 pos_abs 不再被更新而失效。编译通过（text=38372，bin 38448 B），J-Link 烧录 + reset 成功（VERIFY 一致）。**烧录后重新一键标定**：0x0035=2、用时 5.9s、新端点 0/1624（旧值 1598，差的 26 正是那个传动柔性）。**验证**：设 0/300/500/700/1000 → 回读 0/295/497/695/995，6/6 落在 ±15 以内；修复前是 22/321/521/721（一律 +20）。温度全程 39~43℃。文档同步：docs/485命令全表.md、转接板功能说明.md、软件方案.md 的 0x51 操作性描述改为 0x50（保留 0x51 的测试用例与历史实测）；上机脚本 test_485_all.py / soak_485.py 的 safe_enable 改读 0x50 并已上传到 77.11。
未完成：**旧 NVM 端点是 xout 坐标**：刷回旧固件或用别的板子时必须先重标，否则位置偏。未做：固件改动的 git 提交（仓库根 interface-adapter/，main 是保护分支需开 MR）；PROJECT.md 里「两个坐标（别混）」那条尚未改写为「位置一律 0x50」。
下一步：git 提交固件改动到 feature/firmware-import 并开 MR；把 PROJECT.md 的坐标坑条更新成以 0x50 为准。

## 2026-09-19 · 台架测试
完成：**查清了「位置回读和设定差几十」的根因，并修掉一个速度 bug**。结论：**不是标定错、也不是电机没劲** —— 直读电机内部寄存器实测（目标 500，端点 0/1598）：p_des=0.7990、**p_m(0x50)=0.7994（电机闭环误差仅 0.0004rad，非常准）**、xout(0x51)=0.8145、**pos_abs(0x0010/0x0202 用的累积器)=0.8300（偏 +0.031）**。即固件里三个坐标混用：端点是用 **xout(负载侧)** 标定的，p_des 按负载侧换算，但电机按 p_m(电机侧) 执行；而 pos_abs 又在标定过程【失能状态下读 0x51】时被钉到 xout 坐标（damiao.c 有意设计：失能没反馈帧只能用 0x51 初始化 pos_abs）。实测偏置恒定：标定重置后逐点测 目标0/300/500/700/1000 都精确差 **+0.0310~0.0320 rad**，与位置无关、与负载无关（力矩 0~-1.24 变化时偏置不变）、与运动方向无关、不随时间收敛。→ **回读偏大约 20/1000 开度，负载侧实际偏大约 10/1000**。**修复需要改固件统一坐标（重烧），上位机只能做近似补偿**（柔性随负载在 0.015~0.038 间变），用户未拍板。另外修了两个真 bug：① **「速度设了没用」** —— move() 里多写了一句 0x0107=2，而 0x0107 与 0x0104 写的是同一个 v_des，把用户设的速度覆盖了；删掉后实测 velocity=100(2rad/s) 0.4s 只走一半、velocity=1000(20rad/s) 0.4s 已到位 ✓ ② **/api/read 与 /api/write 的 addr 只吃字符串**（int(b['addr'],0)），脚本直调 API 传数字会 500，已改成两种都吃。还给后端加了 **温度保护 65℃**（到线拒绝使能/移动）与**滑块松手即写 0x0101/0x0104**。全程温度 41~47℃ 未失控，实验结束已失能。
未完成：坐标统一要不要改固件重烧未定；改完还需重做一遍端点标定复验。另：safe_enable 里 0x51→0x50 的改动【没有解决偏置】（因为污染发生在标定流程内部，不在使能路径）。
下一步：等用户决定：改固件统一坐标（彻底，需重烧）还是上位机加近似补偿（立刻可用，±0.01rad）。

## 2026-09-19 · 台架测试
完成：**端到端延迟分阶段实测**（用户要求，J-Link 在机上）。实测数据（2026-09-19）：① 本机 PC(WiFi 192.168.77.125) ↔ 机械臂主机：ICMP avg 47ms / **max 124ms**，TCP 建连 med 9ms / max 74ms；② 后端 HTTP 处理：**0.47ms**（主机环回 /api/config）；③ 后端 → 控制柜**单条 Modbus 命令往返：20.4ms**（min 19.3 / max 23.7，n=20）；④ 一次状态刷新（2 条命令）：**41.9ms**；缓存命中 0.6ms；⑤ PC 端到端真实采样约 100~146ms（含 WiFi 抖动）。**归因**：20.4ms 里 485 双向传输约 1.5ms、转接板固件 <1ms（项目既有 DWT 数据：组帧 624cyc=3.67µs）、TCP <1ms → **剩下约 18ms 全是越疆控制柜 dashboard 网关自身的开销**，占 88%。**瓶颈不在我们的板子，而在越疆网关**；其次是 WiFi 抖动。据此优化：状态快变量从 3 条命令压到 2 条（角度降为慢变量），刷新 62ms → **41.9ms**。**板子侧精确耗时没测**：固件里 g_mb_last_byte_cyc/g_mb_frame_cyc 是内部变量、没有映射到 485 寄存器，要精确测必须加一个打点寄存器并重烧固件（未做，用户未要求）。
未完成：板子侧精确耗时未实测（需改固件加打点+重烧）；若 PC 能用有线接入 77 网段，网络抖动可从 avg 47ms 降到 <1ms（最大单项收益）；状态刷新还能再降到 1 条命令（力矩/温度也降慢变量）但会牺牲力矩刷新率，未做。
下一步：把这份分解结论交给用户决定是否上有线网络；要更细的板子侧数字就加打点寄存器重烧。

## 2026-09-19 · 台架测试
完成：排掉用户报的两个问题（网页上位机）：① **「点自动标定不会重新标定」**：后端完全正常 —— 绕开网页直接 POST /api/autocal 跑通全程（0x0035=1 → 2、0x0036=1598、端点更新 0/1598、9.3s）。根因在前端用了 window.confirm 做二次确认：**浏览器一旦拦截弹窗，confirm 静默返回 false，代码直接 return**，表现就是点了没反应。已改成页面内确认条（确认标定 / 取消），不再依赖浏览器弹窗。② **「实时位置和设定差几十」**：实测对照 设 0/300/500/700/1000 → 回读 11/309/510/710/1000，固定偏 **+10 左右**（约 1% 行程）；1000 端因 rad_to_gripper_pos clamp 显示成 1000 所以「不差」；0 端顶硬挡偏差更大（+11~34、力矩约 -1.2N·m）。查 gripper_map.c 确认正反换算是对称的 → **是伺服的稳态位置误差，不是换算 bug**。③ 顺带发现并修掉一个更隐蔽的坑：**失能时写 0x0103 夹爪根本不动、0x0202 回读保持旧值**，所以 move 现在会在失能时先自动安全使能（与上层 ag95_gripper 一致）。④ 页面新增「目标 vs 实际 偏差」显示（>60 变黄）。
未完成：标定后的端点已是 0/1598（本轮标定成功）；夹爪已失能、位置 500 附近；页面 UI 仍未目视确认；服务仍是手动 setsid 启动。
下一步：用户刷新浏览器（Ctrl+F5）后重试一键标定与位置控制；确认手感后可加 systemd 自启。

## 2026-09-19 · 台架测试
完成：网页上位机加了**延迟可视化**（用户要求）：状态卡新增 4 个指标 —— 链路延迟（dashboard 命令往返 avg/peak/n，>200ms 变黄）、本次采样耗时与命令数、数据年龄（本地每 200ms 刷新，不用等下一次请求）、上次操作耗时（点击到服务器回话）；另加「测延迟」按钮（连发 8 条最轻的只读命令报 min/avg/max，后端 POST /api/ping）。实现：DobotBus._cmd 记每条命令往返进 lat（最近 20 条）并计数 cmd_n；Session.snapshot 返回 lat_*/sample_ms/sample_cmds/age，缓存命中时复制一份再填 age（不污染缓存对象）。**实测基准（2026-09-19，经控制柜工具口）**：单条 dashboard 命令往返 **约 20ms**（min 19.5 / max 24.7）；一次状态刷新 3 条命令 **约 62~67ms**；本机 PC 端到端 38ms（缓存命中时）。**以后页面变慢就拿这三个数对照，20ms 是正常线。**
未完成：页面 UI 仍未目视确认；服务仍是手动 setsid 启动，重启机器不自启。
下一步：用户刷新浏览器确认延迟数字与手感；需要常驻就加 systemd user service。

## 2026-09-19 · 台架测试
完成：网页上位机二轮优化（77.11 的 gripper_test/web_gui/，服务已在跑）：① **延迟**：状态接口从 12s 降到 **0.07~0.15s** —— 根因是状态一次发 13 条 dashboard 命令，改成每轮只发 3 条快变量（0x0200~0x0202 状态/使能/位置、0x0010 角度、0x0012~0x0015 力矩+双温度+总状态），端点/标定进度/诊断这类慢变量改成 5s 一次（标定中自动提速 0.3s），加 0.12s 采样缓存；前端改成 250ms 轮询（上一轮回来才排下一轮）+ 点击立即乐观更新。② **自动标定「没反应」的真凶**：autocal() 开头 `if self.cal_running: return False`，而 cal_running 只在进度码==2 时复位 → 一旦失败就永久卡住、之后点击全部静默返回 False；现在加了 30s 超时兜底 + snapshot 进度看门狗 + 结构化返回（前端会显示具体失败原因）。③ **自动标定还有第二个坑**：使能状态下写 0x0034=1 会被固件拒绝，必须先失能且 0x0034 先清 0 再写 1（照抄 test_485_all.py 的 group_F）。补上后真机实测**成功**：0x0035=2、0x0036=**1597**、端点更新为 0/1597、约 9s；失败码已在前端翻译成中文（0x10~0x17）。④ 标定首次跑过一次 0x15「行程角不合理」（AC_TRIM 收余量收到 p_open<0.5 触发），**重跑即成功**，失败不写 NVM、旧端点保持有效。
未完成：0x15 那次失败的诱因未定性（怀疑与标定前夹爪停在张开端、刚建立零点有关），只做过一次对照重跑；页面 UI 仍未目视确认；服务仍是手动 setsid 启动，重启机器不自启。
下一步：让用户在浏览器实操确认手感（重点：连续点击、一键标定、高级寄存器）；要常驻就加 systemd user service。

## 2026-09-19 · 台架测试
完成：新建**网页上位机**并上机实测通过。架构按用户要求：py 后端直连控制柜、网页只做显示。后端 `tools/web_gui/gripper_web.py`（**零第三方依赖**，纯标准库 HTTP 服务 + 自实现 dashboard 文本客户端），前端 `tools/web_gui/index.html` 单页（状态/位置滑块/力/速度/使能/失能/一键标定/原始寄存器读写/日志）。已部署到 77.11 的 `/home/leaprobotics002/projects/jin/gripper_test/web_gui/`（含 README.md）；启动 `python3 gripper_web.py --ip 192.168.5.4 --port 8080`，浏览器开 http://192.168.77.11:8080/ 。真机验证（停掉 leap-os 栈后）：/api/status 首次 0.31s；使能 1.89s→state=1/en=1/pos=1000；闭合 0 后 state=3/pos=27/角度0.04/T=-1.2N·m；张开 1000 后 state=1/pos=1000/角度1.63/T=0.08；失能 0.17s→state=0。本机验证：mock 总线 + 假 dashboard(mock_dobot_server.py --fake) 全通；断链 0.2s 返回 link=false、2s 冷却后自愈重连并重发工具口 485 配置。
未完成：页面 UI 未目视确认；一键自动标定按钮未在真机点过（只验了 enable/move/disable）；服务是手动 setsid 启动，机器重启不自启；README 未更新到 git（仓库根只放固件）。
下一步：让用户在浏览器实际点一遍（重点：一键自动标定、高级寄存器）；要常驻就加 systemd user service。

## 2026-09-19 · 台架测试
完成：断 CAN 自愈**阶段 1 通过**（用户拔掉电机 24V 后跑 `tmp/can_selfheal.py COM21`）：起始 en=0/err=0x00，写 `0x0100=165` 后 t=0.53 s 固件自动判出 CAN 无应答 → **err=0x0F、自动失能 en=0、`0x0200` 返 0**（上层据此重发 165），此后一直保持失能。三个判据（err==0x0F / en==0 / 0x0200==0）全 True。与 2026-09-13 的历史实测（0.53s 判出）一致。
未完成：阶段 2（`tmp/can_selfheal2.py`，要求电机 24V 恢复、能读回 0x51）未跑 —— 等用户插回电机 24V。
下一步：用户插回电机 24V 后跑 `tmp/can_selfheal2.py COM21`：预期写 165 能清错重使能、err 被反馈帧刷回 0x01、控制立即恢复。

## 2026-09-19 · 灯光/台架
完成：应「运动的蓝/绿霓虹范围改小到 5 + 加一段从蓝色到五颜六色的过渡」改 `led.c`：① **`LED_NEON_SPAN` 14→5**（蓝/绿霓虹的色相摆幅 ±20°→±7°）；② 新增 **`LED_XFADE_MS = 1000`** + `hue_lerp()`（按最短路径插色相，折到 ±128 再插，避免 0/255 边界跳色）+ 运行态 `s_xfade_on/from/t0`；`Led_SetMode()` 在**从 RELEASE/HOLD 进 IDLE** 时记下原色中心色相并起计时，`render()` 的 IDLE 分支在 1 s 内把色相从原色 lerp 到彩虹当前色，之后交回彩虹。改动纯在灯效渲染层，不碰任何 485 寄存器/NVM。**验证方法（可复用）**：临时把 `LED_DEMO` 置 1（`LED_DEMO_MS=3000`）并把 demo 序列改成 BOOT→**RELEASE→IDLE**→HOLD→FAULT→CAL（原序列 IDLE 在 REL 前面，永远走不到那个边界），重烧后用 `stm32-dev.py rtt --address 0x<nm 取的 _SEGGER_RTT>` 抓 40 s，从 RTT 的 `[led] rgb=` 反算色相：REL 实测 hue 165.4~173.6（=233°~244°，旧 ±14 时是 39° 摆幅）、HOLD 81~88.2、进 IDLE 后色相 171→163→157→…→彩虹，过渡成立。验完已改回 `LED_DEMO 0`（demo 序列保持新顺序，因为它能覆盖新过渡）。生产固件重编重烧（bin **38448 B**）。**同步了 `docs/485命令全表.md`**（±20°→±7° + 1 s 过渡说明）。**关键坑（新发现）**：`tmp/test_all.py` 要求**开局电机是失能的** —— `rs485_link_check.py` 第 8 步会写 `0x0100=165` 把电机使能，紧接着跑 test_all 会让 E 组的 `0x0007 设零点` 被固件拒绝（使能时不许设零点），表现为 3 项 FAIL（`0x0007` 两项 + 端点恢复）。跑之前先 `0x0100=0`。按此重跑 **83/83 全过**。
未完成：灯效手感需用户目视确认；断 CAN 自愈（can_selfheal，要拔电机 24V）仍未做。
下一步：用户看完灯后如需调：`LED_NEON_SPAN`（摆幅）、`LED_XFADE_MS`（过渡时长，0=关）、`LED_FADE_MS`（输出斜坡）、`LED_LUM_TARGET`（色相亮度归一化）都在 led.c 顶部；接着跑 can_selfheal 收尾。

## 2026-09-19 · 台架测试
完成：电机接上后跑完整的台架全量回归（板上固件 = 当前源码，烧录校验 38204 B，reset 后自动识别到 ESC_ID=3/MST_ID=19）：① 主机测试 56/56；② `rs485_link_check.py --port COM21` 31/31；③ `tmp/test_all.py COM21` **82/83**；④ `rs485_perf.py`：首字节 avg 15.56ms（p95 16.14，n=200），**89% 是 USB-485 适配器+主机开销**，固件静默等待仅 0.35ms；一问一答 64 次/秒；连发 N=20 全回包、N=40 起超 MB_BUF_SZ=320B 丢包（符合设计）；⑤ `tmp/travel_soak.py COM21 30`：30 轮 / 26.3s，峰值力矩 0.84N·m，温度 31→34℃，跑飞 0、0x001A=0；⑥ 一键自动标定三件套全过：`autocal.py`（0x0035=2 / 0x0036=1594 / 端点 0-1.594，0x0103 0/500/1000 → 0.05/0.84/1.63 rad）、`autocal_falsify.py`（故意打错零点再标回来）、`autocal_fail.py`（标定中写 165 被拒且照常跑完；标定中写 0 → 0x0035=23=0x17 且 **NVM 未动**；再恢复标定成功）。单独连跑 4 次一键标定：**1594/1595/1595/1595，离散度 1 mrad**，与历史 1595 一致。**两处测试脚本修复（不是固件问题）**：`tmp/autocal_fail.py` 的 s16() 不容忍单次读超时 None（违反项目铁律）导致必崩，已加自动重试包装；`tmp/test_all.py` 的 `0x003E=0 被拒` 是过期期望（led.c 第 537~539 行明确 0 合法、>1000 才拒），已改成「0 接受 + 1001 被拒」。**遗留一条**：test_all 的 F 组窗口 1560~1620 偏紧 —— E 组把零点/端点改过之后 F 组落在 1618~1621，会偶发差 1~2 mrad 报 FAIL，但单独跑一键标定稳定在 1594/1595，属测试序列污染而非固件问题。
未完成：断 CAN 自愈（can_selfheal/can_selfheal2）未做 —— 需要有人把电机的 24V 拔掉；F 组容差窗口要不要放宽未定。
下一步：让用户拔掉电机 24V 后跑 tmp/can_selfheal.py + can_selfheal2.py（判据：err=0x0F、en 自动失能、0x0200 返 0），插回后写 0x0100=165 应自愈。

## 2026-09-19 · 台架测试
完成：全板焊完后的首轮台架回归（COM21）：① 主机测试 56/56（注意：跑完会因打印 ✔ 触发 UnicodeEncodeError 退出并打 traceback，**不是失败**，成绩看 `===== 结果: 56 PASS, 0 FAIL =====` 那行）；② `tools/gripper_test/rs485_link_check.py --port COM21` **31/31**（协议/CRC/广播/多寄存器/背靠背三帧/一次读 41 寄存器/50 帧写回读/诊断寄存器 0x0060~0x0065 全过，应答 15.6ms）；③ `tmp/test_all.py COM21` **65/82**，A 协议层 / B 只读全扫 / C 可写逐个 / G 三色灯全过，**17 项失败全部来自电机不在线**（0x0040 直读全 None、0x0020/0x0021=1/0、err=15、直读后 0x001B +1 = CAN 上没有任何节点 ACK）。④ 两个本机陷阱：**`tmp/test_all.py` 没有分组参数**，E 组会写 0x000B/0x000C 并 `0x000D` 存 NVM，F 组靠一键自动标定恢复 —— 没有电机时 F 必然失败，会留下被改过的端点，所以无电机跑全量前要先备份端点（本次 0/1590）跑完手动写回；**`0x003E=0 拒绝` 是一条过期用例**，固件早已有意允许 0（上电白灯额外时间 0~1000，默认 0），却仍被判 FAIL，别再当固件 bug 查。⑤ 本机 J-Link 诊断教训：`stm32-dev.py doctor` 里那几个 `[OK]`（含 `_SEGGER_RTT` 地址）只是**从 ELF 里读符号**，不代表探针在线；`Get-PnpDevice` 里有没有 SEGGER 设备才是真判据。
未完成：电机侧连通性待复验（用户说接好了）；J-Link 是否被系统识别待复验；D/E/F 组、rs485_perf、travel_soak、断 CAN 自愈未跑。
下一步：用户接好电机后再探 0x0040 直读 → 通了就跑 test_all 全量（先备份 0/1590、跑完写回）+ rs485_perf + travel_soak 30 + autocal 三件套 + can_selfheal。

## 2026-09-19 · Buck/24V-5V
完成：真相：用户**输出电容根本没接**，VCC5V 实测 6V~36V / 30kHz / Vpp 30V（就是输出节点，不是探错点）。判定为决定性原因：输出电容是降压环路的一部分而非滤波附件 —— 没有它，电感电流全部灌进几乎无电容的节点，电压自己冲（36V 甚至高于输入 24V，是电感储能倒灌），芯片 OVP 反复跳，形成 30kHz 弛豫振荡。由此：『13V』『FB=2.2V』『97µA』『输出随输入变』等全部是无效测量，**前面所有据此做的推算作废**；两颗新片表现一模一样也顺理成章（是板子每次都这么对待它们）。更严重：Vout 冲到 36V 时 FB 分压 = 36×14.3/57.3 ≈ **9V**，而 FB 绝对最大只有 **6V**（对应 Vout ≤ 24V）→ 超 50%，**这颗片（乃至之前几颗）的 FB 输入很可能就是被这么打伤的**。
未完成：输出电容未装；芯片是否已伤未验。
下一步：断电补输出电容（手册 5V 推荐 2×22µF；板上 C8/C17 10µF + C10/C11/C19 100nF 至少先焊上）→ 限流 500mA~1A 上电 → 应见 VCC5V ≈ 5.0V、纹波几十 mV、fsw ≈ 300kHz。在此之前不要再上电。

## 2026-09-19 · Buck/24V-5V
完成：用户确认 VCC5V 波形是 DC 耦合、**Vpp = 30V**、f = 30kHz、Vrms = 14V。用锯齿波关系 Vrms² = mean² + Vpp²/12 反推：Vpp=30 / Vrms=14 ⇒ **mean ≈ 11V，波形在 −4V ~ +26V 之间摆** —— 这正好是 **SW 节点的电压范围**，而挂着几十 µF 输出电容的 5V 轨（即便 L3 只有 6.8µH）纹波也只有 ~36mV 量级。**结论：他测的这个点极可能根本不是输出，而是 SW（或 L3 的 SW 侧）；30kHz 也可能是 300kHz 的欠采样混叠。** 若成立，则此前『5V 出 13V』『FB=2.2V』『97µA』『输出随输入变』整套结论全部建立在错误的测量点上，需推倒重来。这与『EN 拉低→0V』（SW 也会归 0）也自洽。
未完成：探头实际落在哪一侧未确认；SW 与 L3 另一端是否同时看过未做；30kHz 是否混叠未排除。
下一步：① 断电通断档确认探头点：量到 U3.pin5 是 0Ω 还是量到 C8 正端（VCC5V）是 0Ω；② 双探头同时看 SW 与 L3 另一端；③ 断电量 SW↔VCC5V（应约 0Ω）与 VCC5V↔GND 的电容充电跳变；④ 时基 1µs/div 排混叠。

## 2026-09-19 · Buck/24V-5V
完成：用户报 DM40B 示波器测 VCC5V：锯齿波、f=30kHz、**Vpp=30、Vrms=14**（单位未说清）。关键矛盾：若 Vpp 真是 30V，则这个 5V 轨根本不是直流轨，之前所有基于万用表 13V 的推算（含 97µA 那套）全部作废，且已超输出电容 10V/16V 耐压；但 13V 直流配 30V 摆幅的 RMS 应在 16~20V，与 Vrms=14 不符 —— 反倒是『13V 直流 + 窄尖峰到 30V』能同时给出 Vpp 30V / Vrms 14V，这种尖峰形态更像探头长地线假振铃，或板上 SW 振铃耦合（手册点名散热焊盘虚焊会导致 SW 更高振铃与过冲）。若 Vpp 其实是 30mV，则 13V 是真直流，回到分压/反馈那条线。待确认：单位（V/mV）、DC/AC 耦合、最高/最低电平、探头地线长度。
未完成：单位与耦合方式未知；30kHz 是否混叠仍未排除；100pF 未拆。
下一步：请用 DC 耦合 + 短地线（弹簧地）重测：报 VCC5V 的最高/最低电平与 Vpp，同时报 SW 的波形；并拆掉那颗 100pF 复测。

## 2026-09-19 · Buck/24V-5V
完成：用户用 DM40B 示波器看 VCC5V：呈锯齿波，频率 30kHz。芯片额定固定 300kHz —— 相差 10 倍。两个可能：(a) **欠采样混叠**（DM40B 标称 50MSa/s 是 ADC 速率，慢时基会抽点；300kHz 用约 270kSa/s 采，混出来正好 30kHz）；(b) 真的只有 30kHz。若是真的，COT 芯片跑到 1/10 频率意味它进了跳周期/打嗝/轻载 PSM（下游器件没装，负载只有分压约 200µA），或环路不稳。**关键待确认：振幅 Vpp 与高低电平** —— 若输出是在很大范围来回的大振幅锯齿，那万用表读的 13V 只是平均值，前面基于 13V/2.2V 的整套算术全部作废；若只有几十 mV，则 13V 是真直流，问题回到分压/反馈。
未完成：30kHz 是真是假（混叠）未定；输出锯齿的 Vpp / 高低电平未报；SW 波形与占空比未看。
下一步：① 时基打到 1~2µs/div 看 SW：能看见 300kHz 开关就是混叠；② 同时报 VCC5V 的 Vpp、最高/最低电平，以及 SW 的频率与占空比；③ 拆掉用户加的 100pF 再复测。

## 2026-09-19 · Buck/24V-5V
完成：用户换上第二颗新片后现象**完全一样**：VCC5V = 13V、FB = 2.2V。且报「降低输入电压、输出也跟着降」。关键推演：用 Rtop=43k、Rbot=14.3k 反算，FB 节点被抽走 (13-2.2)/43k - 2.2/14.3k ≈ 97µA。若模型是「芯片内部比较器节点 1.2V + 到引脚的等效串联阻抗 ~10.3k + 该电流」，则 Vpin = 1.2 + 97µA×10.3k = 2.17V、Vout = Vpin + 43k×(Vpin/14.3k + 97µA) = 12.9V —— **与实测 13.0V / 2.2V 数值上严丝合缝**，说明环**可能是闭着的**，只是 FB 脚在吸 ~100µA。但正常 SCT2A25 的 FB 是 CMOS 比较器输入（数据手册电气表**没有** FB 输入电流这一项，参考设计 R1=95k/R2=30k 的分压电流才 40µA），吸 97µA 连手册自己的示例都算不对。叠加『Vout 随 Vin 变』（开环特征）与模型不自洽。**两颗同批次表现一模一样 → 更像这批货不是真的 SCT2A25，而不是随机损坏。** 修正上一轮『芯片输入级坏』的单点结论。
未完成：未用示波器看 SW/FB/VCC5V 真实波形（万用表平均值可能骗人）；用户加的 100pF 未拆；芯片丝印/渠道未核。
下一步：① 用 DM40B 的示波器看 SW（频率/占空比）、FB（是否大纹波、谷值）、VCC5V（纹波）；② 拆掉那颗 100pF 复测；③ 不依赖平均值的判据：翘起 R10 上端，用 1k 从外部给 FB 加 0→2V，示波器看 SW 是否在 FB>1.4V 时停（停=FB 输入好）；④ 从**另一家渠道**买一颗 SCT2A25STE 试。

## 2026-09-19 · Buck/24V-5V
完成：用户报新观察：降低输入电压，输出电压跟着降（Vout 大致正比于 Vin，24V 时 12.9V，约 0.54 比例）。这是「反馈环完全没闭上」的教科书特征 —— 闭环的话只要输入高于 dropout，输出应钉在分压决定的定值上、不随输入变。与前面所有证据一致（FB 脚吸 104µA、2.1V 高于 OVP 阈值却不打嗝、pin1 对引脚电压不响应），再次确认 FB 输入没在工作，而不是分压/板子问题。也解释了为什么开路落点一次 7.7V、一次 12.9V —— 开环值本来就没有意义。用户问「两个电阻都取掉、FB 悬空会怎样」：按现在这颗的表现，FB 悬空约等于把控制电压拉到 0 → 占空比拉满、输出会往 VIN(24V) 冲，会打掉 10V/16V 档的输出电容，也让挂在同轨的 U4 超压 → 不要做；而且这条观察已经给出了它想得到的结论。
未完成：新片未换。
下一步：结束实验，直接换 SCT2A25STE（防静电、预热、低风量、散热焊盘预上锡），限流 500mA~1A 上电量 VCC5V 应 5.0V。

## 2026-09-19 · Buck/24V-5V
完成：用户补测全部正常：pin3(VIN)-24V = 0Ω、pin3(VIN)-GND > 100k、pin1(FB)-GND ≈ 14.3k（= R9）→ **板子这条线上没有别的通路**。于是 FB 节点上只有 R10、R9、U3.pin1 三个东西，前两个已验好，那 104µA 只能流进 pin1。数据手册里 FB 是高阻比较器输入（nA 级），104µA = 芯片输入级坏；再叠加 2.1V > 1.44V OVP 阈值却不打嗝 = 比较器根本没看到引脚电压。**排除板子，锁定芯片。** 另：用户问'去掉 47.5k 能不能看出变化' —— 能，但方向危险（FB 被 R9 拉到 0 → 芯片满占空比把输出往 24V 顶，而输出电容是 10V/16V 档），不建议；要做就做安全方向（FB-VCC5V 并 100k，输出只会往下降）。
未完成：新片未换、未复测。
下一步：直接换 SCT2A25STE（防静电、预热 100~120°C、热风 300~330°C 低风量、散热焊盘先预上锡），限流 500mA~1A 上电量 VCC5V 应 5.0V。若仍 12.9V，再上'FB-VCC5V 并 100k'的方向性测试。

## 2026-09-19 · Buck/24V-5V
完成：用户断电用二极管/通断档量 SW-GND：红笔 GND(6/8)、黑笔 SW(5) 读 0.2V / 200 多 Ω。判定：这是板上续流肖特基 D4（SS36/SS510）在导通，不是芯片低边管体二极管。依据：C7.1/D4.1/L3.1/U3.5 同网（D4 阴极在 SW）、D4.2 在 GND（阳极）→ SW-GND 之间并联两条路：芯片内部低边管体二极管（Si 约 0.6V）和外部肖特基（约 0.2~0.3V），肖特基压得低、主导读数。所以 0.2V/200Ω 是正常且预期值，顺带证明 D4 已装、方向对（阴极朝 SW）。注意：我之前给的判读表把这一格写成约 0.5V 是错的（漏算了 D4），已纠正。
未完成：反方向（红 SW / 黑 GND）是否 OL 未报；pin3(VIN)-24V、pin3-GND、pin1(FB)-GND 三个电阻值仍未报。
下一步：引脚级短路检查到目前全正常，与 FB 输入不响应这个功能性故障无关 —— 别再在这上面花时间，直接换片（SCT2A25STE，防静电+预热+预上锡）后量 VCC5V。

## 2026-09-19 · Buck/24V-5V
完成：查了 SCT2A25 手册（Rev 0.8 / 2021，`.work/sct2a25/full.txt`）里跟焊接/装配有关的内容：**只有三类**——① ESD 等级：HBM ±1kV、CDM ±500V（JS-001/JS-002，全引脚），手册注明 JEP155「500V HBM 即可用标准防静电流程安全生产」、JEP157「250V CDM 即可」→ 等级属常规，但**只在标准 ESD 流程下保证**，手工焊不接地不算；② **散热焊盘 pin9 三处强调**：Pin Functions「Must be connected to ground plane on PCB for proper operation」、封装图注「Thermal pad shall be soldered on the board」、Layout Guideline「center thermal pad should always be soldered…Improper soldering thermal pad to ground plate will cause SW higher ringing and overshoot besides downgrading thermal performance」，工艺建议 8mil 散热过孔、怕吸锡就 plugging/tenting；③ 布线：输入陶瓷电容贴 VIN、输出电感贴 SW、BST 电容走底层。**手册没有**回流曲线/峰值温度、没有 MSL/湿度等级、没有烘烤参数、没有手工焊接温度时间 —— 别再去里面找。订货号 **SCT2A25STE**（ESOP-8L，4000/reel）。
未完成：无。
下一步：以后换 U3：接地手环+接地烙铁头+防静电台垫；散热焊盘先预上锡再用低风量热风；无铅回流按通用 245~250°C 峰值、>217°C <60s（非手册值）；受潮按 MSL3 处理（125°C 烘 24h，通用做法）。

## 2026-09-19 · Buck/24V-5V
完成：用户实测：分压实装 43k/14.3k（比值 3.0，1.2V 基准下 = 4.81V，分压正常）；换新片后 U3 pin1 脚上与 FB 焊盘都是 2.1V；**EN 短到 GND 后输出掉到 0V**。结论链：① 掉到 0 证明 12.9V 确实是芯片开关出来的、功率级和 EN 都好，排除 VIN 漏电/高边击穿/VIN-VOUT 焊桥；② 但 FB 脚稳在 2.1V（>1.44V OVP 阈值）芯片既不进 OVP 放电、也不收输出 → **芯片没在采 FB 脚上的电压**；③ 按 43k/14.3k 做 KCL：(12.9−2.1)/43k − 2.1/14.3k ≈ **104µA 从 pin1 流走**，而正常 FB 只吃 nA。合起来 = **功率级活、反馈输入死**，符合输入级被 ESD 打坏（该片 HBM ±1kV / CDM 仅 ±0.5kV）或压根不是 SCT2A25。
未完成：芯片丝印未核；用户加的那颗 100pF 是否为漏电/裂片电容未排除；换片后是否复现未验。
下一步：顺序：① 拆掉那颗 100pF 再量一次 FB；② 看丝印确认 2A25；③ 换新片（防静电、烙铁接地、热风别反复吹）；④ 换前断电量 VIN-GND 与 pin1-GND（应约 14.3k）；⑤ 上电限流 500mA~1A，VCC5V 应 5.0V。若仍 12.9V → 隔离测试：翘起 R10 上端，用 1k 给 pin1 加约 2V，健康片必进 OVP 把输出压下去。

## 2026-09-19 · Buck/24V-5V
完成：用户报：换新片后 VCC5V=12.9V，**U3 pin1 脚上也是 2.1V**（和 FB 焊盘一致），且分压电阻实装值正确。据此做算术：FB/Vout 实测 0.163，而分压比应为 0.24（47.5k/15k 或 32.4k/10k）→ 按 47.5k/15k 在节点上做 KCL：(12.9−2.1)/47.5k = 227µA，2.1/15k = 140µA，**pin1 被抽走约 87µA**；健康 FB 脚只抽 nA 级。与 09-15 那次（7.7V/FB 1.8V，比值 0.234 恰好等于分压比、说明当时 pin1 完全不吸流）对比，这次 pin1 明显在吸流。结论：**FB 脚稳在 2.1V 本身就不可能是好 SCT2A25 的行为**（稳压时脚=1.2V；>1.44V 就该 OVP 打嗝把输出压到 ~6V，不可能稳在 12.9V）→ 要么芯片没在工作、要么 pin1 上不是正常 FB 输入。
未完成：芯片是否在开关（SW/BST 未量）；VIN 是否真 24V 未复测；12.9V 是开关出来的还是高边漏出来的未定；芯片丝印未核。
下一步：决定性一测：把已翘起的 EN 脚短到 GND（EN<1.23V 关芯片）—— 输出掉到 0 = 芯片确实在开关、问题在反馈；输出仍是 12.9V = 这电压不是开关出来的（高边管漏电/击穿），芯片已坏。同时报 VIN(pin3)、SW(pin5) 直流、BST−SW、断电 VIN-GND 与 pin1-GND 电阻。

## 2026-09-19 · Buck/24V-5V
完成：用户报换新片后 FB 焊盘 = 2.1V、VCC5V = 12.9V（并怀疑是电感）。判定：不是电感（电感只决定纹波/EMI/电流，直流设定值由 FB 分压 + 1.2V 基准决定）。两条硬判据证明芯片没看见这个 FB：① OVP 阈值 FB>1.44V 就该打嗝把输出压到 ~6V，2.1V 之下不可能稳在 12.9V；② FB/Vout 应等于下臂占比 0.24（32.4k/10k 或 47.5k/15k），实测 2.1/12.9 = 0.163 偏低 → 焊盘上多了一条下拉路径。与 09-15 的 7.7V/FB 1.8V（比值 0.234 约等于 0.24，比例完全对）对比：当时 FB 与芯片完全没接上，现在变成半通 —— 芯片脚被闭环压在 1.2V，经一个几 kΩ~10kΩ 的虚接把焊盘拽到 2.1V（按 47.5k/15k 反算 Rj 约 10kΩ）。典型冷焊/裂焊/过孔接触不良。
未完成：U3 pin1 脚上（不是焊盘）的实际电压未量；R9/R10 实装值、FB 对地阻抗未量；12.9V 是否稳定未报。
下一步：上电把表笔直接点在 U3 pin1 金属脚上量对地电压：读到约 1.2V 而焊盘 2.1V → 补焊/飞线定案；若也是 2.1V → 转查分压实装值与 FB 对地阻抗。

## 2026-09-19 · Buck/24V-5V
完成：用户换上新 SCT2A25（这颗 EN 从没被 24V 打过），EN 保持悬空，上电后 VCC5V = **12.9V**。推导：① 基准 1.2V、网表 47.5k/15k → 应为 5.00V；板上曾实测 32.4k/10k → 5.09V；12.9V 对应比值 9.75:1，两颗电阻任何组合都出不来（要 47.5k 配 4.9k）。② **OVP 死判据**：手册 FB>120%×1.2V=1.44V 时高边关、低边放电，<115% 才恢复；12.9V 对应 FB 焊盘 3.0~3.1V，若芯片真看见就该一直打嗝把输出压在 ~6V，不可能稳在 12.9V → **芯片没有看见 FB**。与之前 7.7V/FB 1.8V 是同一个病（FB 路开），只是落点不同。判据：量 FB 焊盘 ≈3.0V → FB 脚虚焊/过孔断；=1.2V → 分压比错（有效下臂≈4.9k）。
未完成：**FB 到底有没有接到芯片这条从 09-15 起一直没验过**；12.9V 稳不稳（是稳的则闭合环论成立）未报；U3 pin3 的 VIN 是否真是 24V 未复测。
下一步：断电用表笔直接点在 U3 **pin1 脚上**（不是焊盘）量到 FB 焊盘的通断，不通就飞线；同时量 R9/R10 实装值与 FB 对地有无焊桥。后级（RT9013/CAN）先别装 —— 12.9V 超 RT9013 的 5.5V 上限。

## 2026-09-19 · Buck/24V-5V
完成：用户报：把 SCT2A25 的 EN 脚翘起来后量到 11.3V，并担心之前误用欧姆档量过会打坏芯片。核对手册确认两件事：① EN(pin7) 是**内部上拉电流源**（电气表 IEN：EN=0V 时 0.3µA、EN=1.5V 时 2.3µA；“Floating to enable”、门槛 1.24V）→ 翘起的 EN 是个 MΩ 级悬空高阻点，读数 = 上拉电流 × 万用表电压档输入阻抗（~10MΩ）≈ 10V 量级，**不是真实电平**，换表换档就变；② 欧姆档开路电压 <1V（二极管档 ~3V）、电流 <1mA，而该芯片所有脚绝对最大 ≥6V、EN/FB 是高阻控制脚 → **打不坏芯片**；真正的风险是带电用欧姆档会烧万用表输入、表笔打滑短路 24V、以及该片 ESD 极低（HBM ±1kV/CDM ±0.5kV）。另复核用户发来的 09-19 网表：`U3.7(EN)` 仍挂在 `24V_PROT`（EN 绝对最大 6V = 4 倍超标），即 EN 致损根因在这版原理图里**并未改掉**。
未完成：未确认那 11.3V 是量在翘起的 EN 脚还是 VCC5V（两者后果完全不同：后者=5V 轨跑飞，超 RT9013 的 5.5V 上限）；芯片是否已因 EN 接 24V 永久损坏未验。
下一步：让用户确认节点：EN 脚 → 无事；VCC5V → 断电并查 FB 虚焊（pin1 对分压中点通断）。同时按“先拆旧片→EN 改悬空或 464k/42.2k 分压→清屑→装新片→补焊散热焊盘→限流 500mA~1A 上电→量 BST–SW≈5V”重做。

## 2026-09-17
完成：用户再次发同一份网表问“是不是完全没问题”。本轮做了**逐脚 + 悬空脚全量核对**，结论：**电气连接本身没问题**。
**新核实并确认 OK 的**：`U3` SCT2325 六脚全对；`U4` RT9013 的 `pin4(BP)` **悬空是允许的**（官方定位“无需旁路电容”，不是漏画）；`U5` 查 AF 表确认 **PA8=I2C2_SDA / PA9=I2C2_SCL**、PA11=FDCAN1_RX、PA12=FDCAN1_TX、PB9=TIM4_CH4、PA2/PA3=USART2；`U6`/`U8` 逐脚；`Q3` P‑MOS 拓扑（D=输入、S=输出、G 经 R8 到地 + D1 钳位 → Vgs=−15V）与 `D1` 齐纳方向；`D6` PSM712 datasheet 明写 pin3 是公共端。
**最有价值的一条（差点误报）**：**共模电感内部脚对因厂家而异** —— `L2` TDK ACT45B 是 **1‑4/2‑3**，`L4` Chilisin CUW21T 是 **1‑2/3‑4**；网表 CANH 走 `L2.1↔L2.4`、CANL 走 `L2.2↔L2.3`，B 走 `L4.1↔L4.2`、A 走 `L4.3↔L4.4` —— **两颗都接对了，且都从同名端进**。我是去拉立创符号图才确认的（`https://lceda.cn/components_pngs/<C号>_sch.png`）。
**全量扫描结果**：新写了 `.work/netcheck.py`（逐器件比对脚数 vs 网表出现脚），**除 `U4.4` 外全板没有一个未连接引脚**。
**仍存在的**：🔴 `D5` 保护不到 `U3`（未定）；🟡 `U9` 针序镜像、`R2` 两处重名、`U2` 是电容却用 U 位号、**`LED2`/`LED3` 极性网表表达不出来**（需上电目视）；📐 两条取向：两组 120Ω 终端与 560Ω 偏置都接在**共模电感收发器侧**、`U7` 的 24V(6/7/8) 与 GND(3/4/5) 相邻。
未完成：🔴 `D5` 方案未定（卡在工具口 24V 实测）；`U9` 针序未改；docs 未同步到 `U3`+`SCT2325`+`L3`；第二个 TRIG 引脚未定。
下一步：把本次核对结果写成一页进 `docs/硬件方案.md`，并一并把三份文档从旧的 `U11`+`SCT2332TVBR`+`L1` 同步到 `U3`+`SCT2325TVBR`+`L3`。

## 2026-09-17
完成：用户说“又加了一个 trig，有哪些 GPIO 还能用”。本轮把 `U5`（LQFP48）的完整引脚库存核出来了：
① **来源与校验**：数据手册里的引脚图是矢量图形，`rows.py`（按坐标重建表格行）抽不出来 —— 新写了 `.work/stm32g431/render_pages.py`（整页渲染）与 `crop.py`（按矩形放大裁切，PyMuPDF），渲染 **DS12589 Rev6 第 45 页 Figure 8** 才读出 LQFP48 映射；再用网表的电源脚反向校验（`19=VSSA 20=VREF+ 21=VDDA 23=VSS 24=VDD 35=VSS 36=VDD 47=VSS 48=VDD` 全部对上），确认无误。
② **`U5` 只用 27/48 脚，空闲 21 脚**：✅ 干净可用 15 个 = `PB10(22) PB11(25) PB12(26) PB13(27) PB14(28) PB15(29) PB0(16) PB1(17) PB2(18) PA10(32) PA0(8) PA4(12) PA5(13) PA6(14) PA7(15)`；⚠️ 能用但别优先 6 个 = `PA15(39) PB3(40) PB4(41)`（SWJ 五脚）、`PB8(45)`（**`PB8-BOOT0`**，板上 R3 10k 下拉）、`PC13(2) PC14(3) PC15(4)`（备份域/OSC32）。
③ **两个坑**：**`PC4`/`PC5` 在 LQFP48 上根本没引出**（只有 UFQFPN48 才有，别当备选）；**`EXTI` 线号在同名脚间共享**（PA5/PB5/PC5 同一条 EXTI5）→ 第二个 TRIG 别选 `PA5`，推荐 **`PB11`(EXTI11)** 或 `PB10`。
④ **`CN1` 是 5P 且已用满**（`1`TRIG/`2`SDA/`3`SCL/`4`3V3/`5‑7`GND）→ 再加一路 TRIG 必须另加座子或把 I2C 挪走。
⑤ **关键发现：`U5.45` 挂着 `R3` 10k 到 GND = `PB8-BOOT0` 下拉**（所以 BOOT0 正确接地，但 PB8 不是干净 GPIO）。
已全部写进 `PROJECT.md`（新增“MCU `U5` 空闲引脚清单”条 + 工具条补 `render_pages.py`/`crop.py` 用法与“第 45 页 Figure 8”）。
未完成：**第二个 TRIG 的引脚还没定**（用户只问哪些能用）；网表/原理图未改；docs 仍未同步到新 Buck；`D5` 仍卡在工具口 24V 实测。
下一步：用户选定第二个 TRIG 的引脚（建议 `PB11`）后，同步网表相关文档：`docs/硬件方案.md` §4.5 扩展口 + `docs/软件方案.md` 的 CN1 条 + 新增引脚说明。

## 2026-09-17
完成：用户反问“换一个 TVS 不行吗，有啥推荐” —— **我上一条“换 TVS 解决不了”说得太绝对，本轮纠正**：能换，只是不能“保证完全钳在 34V 以内”，但能把窗口从 0 打开。关键在于**钳位电压随电流变化，不能只看手册那个单点 `Vcl`**：`V(I) ≈ Vbr_max + R_dyn·I`，`R_dyn ≈ (Vcl − Vbr_max)/Ipp`。按这个算：`SMF28CA` **I=0 时已 34.4V（窗口 0）**；`SMF26CA`（Vrwm 26/Vbr 28.9~31.9/Vcl 42.1@4.8A）窗口 ≈**1.0A**（典型 1.7A）；`SMF24CA`（Vrwm 24/Vbr 26.7~29.5/Vcl 38.9@5.1A）窗口 ≈**2.5A**（典型 3.2A）；`SMAJ24CA` ≈4.9A；`SMBJ24CA` ≈**7.4A**。
**且找到更好的判据**：SCT2325 **推荐工作范围 3.8~32V**，所以目标是把浪涌压到 ≤32V —— `SMF24CA` 在 ≤1.4A 浪涌下做得到（`SMF26CA` 几乎做不到）。
**推荐（均已核立创现货）**：**首选 `SMF24CA`（C3720035 / C5333993，SOD-123FL —— 与现 `D5` 同封装同焊盘，只换料号）**；次选 **`SMF26CA`（C19077516 / C3720541，同封装）**，Vrwm 有 2V 余量但窗口只 ~1A；要真兜住用 **`SMBJ24CA`（SMB/DO-214AA，要改焊盘）**。另出一条更彻底的路：**在 `U3.VIN` 前串 2.2Ω + 一颗 30V 钳位**（buck 输入仅 ~62mA，掉 0.14V），把“低钳位窗口”和“高 Vrwm 安全性”解耦。
**数据来源已注明**：`SMF26CA` 取 Semiware 官网手册页，`SMF24CA` 取立创参数表；钳位均为线性近似（膝点偏软，低电流下实际略低）。已全部写进 `PROJECT.md` 的 D5 坑条。
未完成：**`D5` 方案未定，卡在一个实测前置条件：机械臂工具口 24V 的实际上限**（空载/带载/电机 4A 峰值时；24.0~25.0V 可用 24V 档，25.2V 以上必须退到 26V 档）。这个数目前未量。其余未办：`U9` 针序未改；`YSPI0530T` 的 Isat 定义未核厂家手册；三份 docs 未同步到新网表。
下一步：用户量出工具口 24V 上限后 → 定 `D5` 料号（同封装 `SMF24CA` 或 `SMF26CA`，或改焊盘上 `SMBJ24CA`），然后一并同步 `docs/硬件方案.md`。

## 2026-09-17
完成：用户发来新网表（**22925 B / 222 行**，工作区 `PCB/` 里那颗已是同一份）问“这个是不是可以”。逐脚核过 —— **电气上可以，而且正是我建议的那套**：
① **`U3 = SCT2325TVBR`（C509064）** 六脚全对：`1=VS→VCC5V`（固定输出版就该直连 VOUT）、`2=EN→24V_PROT`、`3=VIN→24V_PROT`（手册允许 EN 直连 VIN）、`4=GND`、`5=SW→C7.2+L3.1`、`6=BST→C7.1`。
② **分压/前馈四颗件 `R4/R5/R6/C16` 已全删**，`5V_FB` 网消失，`U3.1` 直连 `VCC5V` —— 比我说的“R6 改 0Ω”更干净，**顺带消掉了“上臂虚焊→占空比跑满→输出冲 24V 烧 LDO/MCU”这条真踩过的失效路径**。
③ **`L3 = YSPI0530T-6R8M`**（C46595887，6.8µH / Isat 4.3A / DCR 76.2mΩ / 5.7×5.2mm）= 我推荐的那颗。
④ **`U9.4` 已在 `VCC5V`**，灯板共阳 5V 落实。输出电容 `C8 10µF + C11/C18/C20 3×22µF + 2×100nF` 符合手册 1~3×22µF；输入 `C2/C4 22µF/35V + U2 10µF/35V + C35` 充足。
🔴 **新发现（已写进 PROJECT.md 坑）：`D5` SMF28CA 保护不到 `U3`** —— TVS `Vbr 34.4V` / `Vcl 45.4V`，而 SCT2325（和 SCT2332）的 `VIN/SW/EN` 绝对最大就是 **34V** → **对 U3 等于没装**。这是 09-17 从 `SCT2A25`（**110V** 绝对最大）换到 34V 器件时**静默失效的一条设计意图**。**换 TVS 解决不了**（24V 轨任何一档钳位都≥36V）。三条出路已记：接受 / `U3.VIN` 前串 2.2Ω+30V 钳位（buck 输入仅 ~62mA，掉 0.14V）/ 换回高耐压 buck。**尚未决定。**
⚠️ 另提醒：`U9` 针序**仍是镜像的**（还需交叉线）；`L3` 焊盘名挂的是 2.2µH 型号的封装名，需核 land pattern。
未完成：🔴 `D5` 保护不到 `U3` 未定方案；`U9` 针序未改；`YSPI0530T` 的 Isat 定义未核厂家 datasheet；**`docs/硬件方案.md`（~20 处）与 `docs/软件方案.md`（3 处）仍写着旧的 `U11`+`SCT2332TVBR`+`L1`，未同步到 `U3`+`SCT2325`+`L3`**。
下一步：把 `docs/硬件方案.md`/`软件方案.md`/`转接板功能说明.md` 同步到新网表（`U3`+`SCT2325TVBR`、`L3`=YSPI0530T-6R8M、§5.2 从“可调+分压+前馈”改成“固定 5V 无分压”、供电树/器件表/§8.1/8.3/8.4），并加上 D5↔U3 那条红线。

## 2026-09-17
完成：用户追问“你确定 6.8µH 可以吗”、“ΔI / DCM 都是啥意思”、“DCM 影响大吗”。这一轮的真结果：
① **纠正我自己的推销话术**：`YSPI0530T-6R8M` 与 `YSPI0530T-100M` **同封装（5.7×5.2×3.0）**，选 6.8µH **没有体积收益** —— 体积由封装档位（0420/0530/6045）决定，同系列换感值只影响纹波/DCR/Isat。选 6.8µH 的真理由改成：**Isat 4.3A + DCR 76mΩ**（10µH 是 3.5A/128mΩ）。
② **新的硬理由（我之前没说）**：**Isat 标在 25°C，高温要打折（按 −15% 估）** → 3.5A 的件≈**3.0A，跌破 SCT2325 限流上限 3.1A**（`0420T-4R7M`、`0530T-100M` 都踩）；4.3A 的件≈3.65A 仍有余量。（4.7µH/6.0A 那颗 0530 也可以，但只有 4.7µH）。
③ **6.8µH 数值全部验过**（24V→5V, 1.1MHz）：ΔI=0.53A(p-p)±0.27A；最坏情况（0.99MHz、−30% 感值）0.84A；峰值 0.52A≪限流 2.5A；I_rms 0.29A×76mΩ=**6.5mW**；t_on 189ns>80ns；式(6) 要 Cout 3.0µF<<板上 3×22µF。手册图 14 自己就用 6.8µH，24V/2A 下式(4) 下限 6.66µH。
④ **DCM 评估（本轮重点）**：本板 5V 负载实为 **0.15~0.25A（额定 7.5~12.5%）**。PSM 进入点≈ΔI/2：**1.1MHz 下 10µH 是 0.18A、6.8µH 是 0.265A** → 10µH 时工作区正好骑在门口（**模式抖动**），6.8µH 全工况稳定在 PSM。但 **DCM 对本板影响很小且方向是好的**：效率变好（少发脉冲→开关损耗降）；纹波被 `U4 RT9013` 的 PSRR 50dB 吃掉（5V 直连的灯板/TCAN1042/LED2 都无感）；环路反而更稳（DCM 退化成单极点）；唯一要留意的是**电感啸叫**（只能上电耳朵听，测不出来）。**结论：别把“进 DCM”当缺陷去修。**
未完成：用户尚未拍板选哪颗电感/换不换 SCT2325；`docs/硬件方案.md` 的 BOM 与 §5.2/§8 未改；`YSPI0530T` 系列我只看过立创页参数，未核厂家 datasheet 里 Isat 的下降百分比定义。
下一步：若用户要落地方案：先把 `YSPI0530T-6R8M` 的厂家 datasheet 下来看 Isat 定义（25°C 还是 100°C、降 10% 还是 30%），再改 `docs/硬件方案.md` §5.2/§8 与 BOM（SCT2325 + `R4/R5/C16` 不贴 + `R6`=0Ω + `L1` 换件）并提醒改 PCB 焊盘。

## 2026-09-17
完成：用户“帮我推荐一下电感吧要小一点的”。已从立创商城逐个核实参数并给出小体积替代方案（全部一体成型合金电感，现货）：

| 型号 | C 号 | L | Isat | 温升电流 | DCR | 尺寸 mm | 体积/现在 |
|---|---|---|---|---|---|---|---|
| YSPI0420T-4R7M | **C46595873** | 4.7µH | 3.5A | 2.6A | 105mΩ | 4.4×4.2×**2.0** | 23% |
| **YSPI0530T-6R8M** ⭐ | **C46595887** | **6.8µH** | **4.3A** | 4.0A | 76mΩ | 5.7×5.2×**3.0** | 55% |
| YSPI0530T-100M | C46595888 | 10µH | 3.5A | 2.75A | 128mΩ | 5.7×5.2×3.0 | 55% |
| YSPI0530T-4R7M | C46595885 | 4.7µH | 6.0A | 4.6A | 53mΩ | 5.7×5.2×3.0 | 55% |
| （现在）CR6045-100M | C520361 | 10µH | 3.0A | 2.5A | — | 6.0×6.0×4.5 | 100% |

**首选 `YSPI0530T-6R8M`**：高度 4.5→3.0mm、体积 55%，6.8µH 正好是手册图 14 的示例值（24V 下 ΔI=0.53A p-p ≈ ±0.27A），Isat 4.3A 对 SCT2325 限流上限 3.1A 留 1.2A 余量，且 DCR 76mΩ 比现在更低。再小一档是 `YSPI0420T-4R7M`（体积 23%，代价：4.7µH 在 24V 下纹波 0.77A(p-p)，深 DCM，DCR 105mΩ）。**⚠️ 0420 的 6.8µH/10µH 别选**（Isat 只有 2.8A/2.3A，不达标）。
**关键结论：小电感与换 SCT2325 绑死** —— 手册要求 Isat > 芯片限流；SCT2332 上限 5A → 需 Isat ≥5A（小体积件做不到，现有 L1 的 3A 本来就不合规）；SCT2325 上限 3.1A → Isat ≥3.5A 即可。已写进 `PROJECT.md` 的 SCT2325 条（含“L1 现封装 SLW6045 6.0×6.0×4.5mm，换件要改 PCB 焊盘”）。
**工具经验（省大量 context）**：抓立创单页参数**别用 `web_fetch`**（整页 HTML 上万字），用 PowerShell `Invoke-WebRequest -UseBasicParsing` + 正则抽 `描述/商品编号/商品封装` 几行即可。
未完成：用户尚未拍板选哪颗/换不换 SCT2325；`docs/硬件方案.md` 的 BOM 与 §5.2/§8 未改。
下一步：用户选定后：改 `docs/硬件方案.md` §5.2/§8 与 BOM（SCT2325 + `R4/R5/C16` 不贴 + `R6`=0Ω + `L1` 换 YSPI0530T-6R8M），并提醒改 PCB 焊盘。

## 2026-09-17
完成：用户贴出 SCT2325 手册图 14（12V In / 5V 2A Out，**L1 = 6.8µH**、C3=22µF、纹波 ±0.3A、1.1MHz）质问“这个都是 6.8”，并要求翻译手册关于电感的整节。翻出了 `.work/sct2325/page14.png` / `page15.png`（新写的 `.work/sct2325/render.py` 用 PyMuPDF 按关键词渲染整页 —— **PDF 里的公式与图是图片，`cmap2.py` 抽不出来**），把 `Inductor Selection` 整节＋表 1 逐句翻译并抄下式(3)~(8)。关键更正：**图 14 典型应用用 6.8µH（12V/2A）、表 1 评估板推荐件是 10µH（Würth 744314101）—— 两个都是“官方值”，不是矛盾**；根因是**式(4) `L_INDMIN = VOUT·(VINMAX−VOUT)/(VINMAX·KIND·IOUT·fSW)` 里 `IOUT` 在分母**，24V→5V 在 `KIND=0.3`/`0.99MHz` 下化简为 `L_INDMIN[µH] = 13.3/IOUT[A]` → 2A 时 6.7µH（所以 6.8µH 压限、10µH 留余量），**我们 0.25A 时要求 53µH**。另核实：图 14 的“±0.3A”是**峰值**（ΔI_LPP=0.6A、KIND=0.3）**且已含 −30% 电感容差**（6.8µH→4.76µH 算出来正好 ±0.28A）。已把式(3)~(8)、两个推荐值、渲染工具写进 `PROJECT.md`。
未完成：换不换 SCT2325 仍未拍板；`docs/` 里那页选型对比未做。
下一步：用户拍板要换：改 `docs/硬件方案.md` §5.2/§8/BOM（`R4/R5/C16` 不贴、`R6`=0Ω、`L1` 沿用 10µH）。

## 2026-09-17
完成：用户问“SCT2325 是 1.1MHz，电感为啥不能小”。算完写入 `PROJECT.md` 坑：**24V→5V 化简后 `ΔI[p-p] ≈ 3.96/(L[µH]×FSW[MHz])` A，保持 CCM 需 `L > 3.96/(FSW[MHz]×2×Iout)`**。关键结论：**本板 5V 实际负载只 ~0.25A**，现有 `SCT2332`（400kHz）+`L1 10µH` 给出 `ΔI≈0.99A ≫ 0.25A` → **一直在 DCM/脉冲跳跃跑，这是正常轻载行为不是故障**；同负载下要 CCM：400kHz 需 L>19.8µH、1.1MHz 需 L>7.2µH —— 所以换成 SCT2325（1.1MHz）用 10µH **刚好落回 CCM**。反面结论也一并记下：**“频率高 ⇒ 电感可以小”在本板是反的**，纹波是安培数不是比例，`L ∝ 1/FSW` 只在满载附近成立。另外补了一条选型约束：**SCT2325 是内部固定补偿的峰值电流模式，没有 COMP 脚可调**，偏离手册推荐 L（10µH）只能台架验相位裕度/负载跳变（手册原文要求 bench evaluation）。
未完成：换不换 SCT2325 仍未拍板；未做选型文档、未改 BOM。
下一步：用户拍板要换：`docs/硬件方案.md` 加一页 SCT2325 对比 + 改 §5.2/§8/BOM（`R4/R5/C16` 不贴、`R6`=0Ω）；不换就保持现状。

## 2026-09-17
完成：用户问“SCT2325 会不会更好”。已把 `SCT2325.pdf` 下到 `.work/sct2325/`（复用 `.work/sct2332/` 那套 `cmap2.py`/`ctx.py`）并解出完整参数：**固定 5V±1%、2A、TSOT23‑6L、1.1MHz（±6% 展频）、`ILIM_HSD` 2.5/2.8/3.1A、LSD 谷值限流 2.8/3.2/3.6A、`RDSON_H/L` 130/70mΩ、`tON_MIN` 80ns、软启动 4ms、Iq 20µA、SW 无振铃；绝对最大 `VIN/SW/EN −0.3~34V`、`BST −0.3~40V`、`VS −0.3~6V`；引脚 1=VS / 2=EN / 3=VIN / 4=GND / 5=SW / 6=BST；手册推荐 `L=744314101 10µH / Isat 3.5A`、`Cout 1~3×22µF`**（立创 C509064）。与现在的 SCT2332（可调 0.8V、3A、`ILIM_HSD` 4/4.5/5A、400kHz）逐项对比后给出结论：**对 24V→5V、实际 ~0.25A 的本板，SCT2325 在三件事上更好——① 固定输出省掉 R4/R5/R6/C16 四颗件并干掉“分压/前馈虚焊→占空比跑满→输出冲 24V 烧 LDO/MCU”这个真踩过的失效模式；② `L1` 的 Isat 3A 与 SCT2325 典型限流 2.8A 基本匹配（原来 3A vs >4A 差得远），且手册推荐电感就是 10µH；③ 展频 + SW 无振铃对同板的 CAN/485 是白拿的 EMI 改善**。更差的地方：只有 2A/2.8A 余量、固定 5V 不可调、1.1MHz 开关损耗更大（本负载下可忽）。**关键坑：不能直接替换**——SCT2332 的 pin1 是 `FB`（分压中点），SCT2325 的 pin1 是 `VS`（必须直连 VOUT）；直接换会让 VS 看到 0.8V→占空比跑满→烧后级。迁移办法：`R4/R5/C16` 不贴 + `R6` 2k 改 0Ω（**只改 BOM 不用改板**）；`L1 10µH` 与 `3×22µF` 都能沿用。已把参数、手册读法、迁移办法写进 `PROJECT.md`“东西都在哪”与 `SCT2325 要点`。
未完成：**换不换还没定**（用户只问了会不会更好，未拍板）；没有做出选型对比文档，也没有改原理图/BOM。
下一步：用户决定要换，就在 `docs/硬件方案.md` 加一页 SCT2325 选型对比，并把 §5.2/§8 的 Buck 段与 BOM（R4/R5/C16 不贴、R6=0Ω）改成 SCT2325。

## 2026-09-17
完成：用户告知“灯板改成 5v 了”（澄清：**只在原理图/PCB 上改了，实物还没接 5V**）。已把两份方案与项目记忆同步到 5V：`docs/硬件方案.md` §4.4 针序表 `U9.4` 标为“✅ 2026‑09‑17 已改，原为 VCC3V3”、§8.8 headline 与做法第 1 条标为已完成并注明“**手上 09‑17 那份网表里 `U9.4` 仍挂在 `VCC3V3`，只改了原理图就需重导一次网表**”；`docs/软件方案.md` §2 引脚表与灯板段同步为已改 5V，并写明“**3V3 时期调好的亮度/白平衡作废**（蓝从 ~1.25mA 涨到 ~8mA）”；`memory/PROJECT.md` 状态灯段同步。代码未动。
未完成：5V 是原理图/PCB 上的改动，**实物还没有 5V、未目视复测**；`PCB/Netlist_Schematic1_2026-09-17.tel` 仍是旧网（`U9.4` 在 VCC3V3）；未确认 `U9` 针序是否一起改成与灯板 1:1。
下一步：重导网表后（或实物接上 5V 后）：先看 `U9.4` 是否真在 `VCC5V` 网络，再复测三色亮度/白平衡，必要时重调 `LED_LUM_TARGET`（现 140）与默认亮度 100。

## 2026-09-17
完成：用户给了新版 `Netlist_Schematic1_2026-09-17.tel`（**24251 B**，工作区 `PCB/` 里那颗已是同一份，逐行 diff 无差异）并说“trig 已经改成 pb5 了、现在已经有上拉电阻了”。核实网表：`TRIG ; CN1.1 U5.42`（**U5.42=PB5**，LQFP‑48；从 PB4/U5.41 改过来）、`I2C_SCL ; CN1.3 R9.1 U5.31(PA9)`、`I2C_SDA ; CN1.2 R10.2 U5.30(PA8)`，而 `R9.2`/`R10.1` 都挂 `VCC3V3` —— **R9/R10 = 4.7kΩ I2C 上拉，板上已有**（CN1.4 仍是 VCC3V3）。已改两份方案（“你两个都修改一下”）：① `docs/硬件方案.md`：架构图/接口表 `PB4`→`PB5`；§4.5 针序表改 `PB5`（U5.42）并重写两条注（I2C 上拉已有；TRIG 改 PB5 的理由；外部自带 4.7k 并联≈2.35k 可接受）；§5.4 BOM 新增 `R9`/`R10`（C406731，4.7kΩ）行；§8.10 “没有上拉”那条划掉改为已解决。② `docs/软件方案.md` §2 灯效段下的 CN1 条：`PB4`→`PB5`、删“没有上拉”改为已有 R9/R10。两文件全文 grep `PB4|U5.41` 只胆有意保留的“从 PB4 改到 PB5”历史说明。
未完成：固件仍未初始化 CN1（PB5/PA8/PA9 均未配置），I2C2 未开；CN1.4 仍是 3V3（未定是否改 5V）。
下一步：要用 CN1 时：先把 PB5 配成 GPIO（不再有 SWJ 顾虑）、I2C2 初始化（PA8/PA9，上拉已在板上），再接模块实测。

## 2026-09-17
完成：顺带清了一条会误导的旧记录：`PROJECT.md` 坑里“Buck 反馈分压 `R10 47.5k`(上)/`R9 15k`(下)”是**旧位号**，而 09‑17 网表里 `R9`/`R10` 已经是 CN1 的 I2C 上拉 —— 已改成“现在是 `R4 158k`/`R5 30k`”并显式标出旧位号已作废；同时把“下臂开路只出 ~1.2V”改为 SCT2332 的 **0.8V** 基准、`L2/D4` 换成 `U11.2/L1`。并在坑里新增一条 `CN1` 针序 + TRIG=PB5 + 板上已有 I2C 上拉，注明旧文档/旧记忆里“没有 I2C 上拉”“TRIG=PB4”两条已作废。

## 2026-09-17
完成：应“灯效切换看得墨迹 / 把等两秒删了 / 上电就判断，5s 判断完有问题就直接红”：① `led.c` 的 `LED_FADE_MS` 400→**100**，并把 `slew8()` 从“一阶逼近”改成**线性斜坡**（`step = ceil(255/n)`，先判 `n==0`）—— 改之前最小步长恒为 1，不管把 `LED_FADE_MS` 设成多少，满量程过渡都固定 255ms，所以此前调这个值“没反应”，这是根因。② 上电白灯的额外下限删掉：`cfg_boot_ms` 默认 20→**0**、`Led_WriteCfg(0x003E)` 与 `Led_LoadNvm` 的 `t[7]` 都允许 0、`grip.c` 的 `boot_ms` 初值 2000u→**0u**；`0x003E` 语义变成“上电白灯**额外**持续时间(0..1000, 0.1s)”，默认 0。③ `led.h` / `docs/485命令全表.md` §2.10 / `docs/软件方案.md` 同步。④ 重编重烧（**38204 B，校验一致**）+ reset，RTT 实测：`CCR2(红)=1000`、`CCR1=0`、`CCR4=0`、`cfg_boot_ms=0`、`cfg_boot_v=100`、`g_boot_done=1` ✓。⑤ 向用户说明：**无电机的台架上白灯仍是 ≈5.4s，卡的是 `Damiao_AutoDetect` 扫 ID 1..16（`control.c` `scan_delay(300u)`×17），不是刚删掉的那 2s**；接上电机且 NVM 有 ID 时 ≈0.5s。用户随后回“不动了”→ 扫描时长不改。
未完成：台架上想更快只能缩短 `control.c` 的扫描（用户明确不动）；`U9.4` 改 VCC5V、灯板 1:1 针序、`L1` Isat≥4A、C35 耐压、CN1 的 I2C 均未动。
下一步：焊上 Buck 后先测 VCC5V 是否 5.01V，再把 `U9.4` 改到 VCC5V 复测灯板亮度/白平衡。

## 2026-09-17
完成：① **代码改回**：`led.c` 的 `LED_DEMO` 改回 **0** → 重编重烧（**38180 B，校验一致**）+ reset。实测收尾：`CCR2(红)=1000 / CCR1(蓝)=0 / CCR4(绿)=0`，`cfg_boot_v = cfg_idle_v = cfg_fault_hv = 100` ✓。（亮度 100、色相亮度归一化、输出平滑三项**保留**，那是用户要的改进。）
② **两份方案已同步到 2026-09-17 网表**：
- **`docs/硬件方案.md` 整篇重写**：架构图/供电树/器件表全部换成 `U11 = SCT2332TVBR`（TSOT‑23‑6 同步整流）；**删掉 U3 / L3(33µH) / D4(续流管)**；§5.2 改成 `L1 10µH / R4 158k / R5 30k / R6 2k / C16 330pF / C11+C18+C20 3×22µF`；**新增 §4.5 `CN1` 扩展口**（I2C2 + TRIG + 3V3）；§8 红线按新芯片重写：**8.1 EN 结论反转**（SCT2332 绝对最大 34V、手册明确允许直连 VIN）、**8.2 TSOT‑23‑6 无散热焊盘**（旧 ESOP‑8 那条不再适用）、8.3 前馈拓扑（FB 脚经 R3(2k) 到中点、Cf 并在上臂）、**8.4 5V 上限 ~1.5A**（Isat 3A 低于手册要求的 >4A + RθJA 89°C/W 双限制）、8.5 输出电容已达标、8.7 续流管已删。
- **`docs/软件方案.md`**：位号注同步到 09‑17（含 `U11` / `CN1`）、§2.2 的 Buck 改 `U11`、灯板段补了“默认亮度 100 / 色相亮度归一化 / 输出平滑 / `LED_DEMO` 自检”和 **`CN1` 的三条注意**（固件未初始化、无 I2C 上拉、PB4 属 SWJ 五脚）。
未完成：并未真机验证：CN1 未接外部模块、I2C 未初始化、TRIG 未用；5V 轨无真实带载数据（未焊 Buck）。
下一步：焊上 Buck 后：① 先测 VCC5V 是否 5.01V，② 把 `U9.4` 改到 VCC5V 后复测灯板亮度和白平衡，③ 再试 CN1 的 I2C2。

## 2026-09-17
完成：用户反馈“全对”，但提了两条观感不足：**改变颜色会影响亮度**、**灯效之间切换突兀**。已在 `led.c` 加了两个编译期调校并用 RTT 验过：
① **`LED_LUM_TARGET`（默认 140）色相亮度归一化**：按 Rec.601 算 `Y=(77R+150G+29B)/256`，**超目标就等比压下来**；关键是**按饱和度加权**（纯白 S=0 完全不动）—— 所以**“上电白/标定白/全亮”仍是 255 满亮，不抵消刚改的默认亮度 100**。实测（IDLE 彩虹去重 RGB）：纯绿 `0,255,0`→**`0,237,5`**、黄段 `255,246,0`→**`161,155,0`**，所有过亮色统一到 **Y≈139~140**；\<140 的红(76)/品红(105)/蓝(29) 不动。设 0 = 关。
② **`LED_FADE_MS`（默认 400）输出一阶平滑**：新增 `slew8()`，在 `led_out()` 里对三个通道做趋近 —— 实测切换瞬间抓到中间态 `rgb=24,0,231`（蓝→红途中）、`rgb=255,24,24`（红→白途中）✓。设 0 = 关。
③ 烧录校验一致（**38616 字节**）+ reset。
**顺带一条工具经验**：重编后 `_SEGGER_RTT` 地址会变（38412→38616 字节时 `0x20000bac`→`0x20000bb0`），`rtt --address` 得每次用 `nm` 重新取；另外 RTT `NO_BLOCK_SKIP` 模式下主机跟不上会**掉一大段数据**（实测丢过 39 s），分析日志前先看 `t=` 是否连续。
未完成：纯蓝 Y 天生只有 ≈29（视见函数），软件归一化压不到它；灯板共阳仍只有 3.3V（没焊 Buck）→ 蓝光更弱。
下一步：焊上 Buck、灯板共阳改 5V 后复测白平衡；如果归一化后整体偏暗，调 `LED_LUM_TARGET`（调低=更均匀但彩色更暗，调高=更亮但差异更明显）。

## 2026-09-17
完成：灯板调试闭环（用户确认“现在灯是**红色**”→ **颜色映射 / 排线针序全对**，之前的“蓝灯”确实只是空芯片现象）：
① **默认亮度 60 → 100**：`led.c` 的 `cfg_boot_v`/`cfg_idle_v` 改 100，`cfg_rel_hv` 0xAB3C→**0xAB64**、`cfg_hold_hv` 0x553C→**0x5564**、`cfg_fault_hv` 0x003C→**0x0064**（即 0x0037/38/39/3A/3B 的默认值）。**同步改了 `led.h` 注释与 `docs/485命令全表.md` §2.10 的默认值表。**
② **在 `led.c` 加了编译期自检 `LED_DEMO`（默认 0）**：=1 时每 3 s 自动循环 9 格（BOOT 白 / IDLE 彩虹 / RELEASE 蓝霓虹 / HOLD 绿霓虹 / FAULT 红 / CAL 蓝白 + 只红/只绿/只蓝），并在 `Led_Tick` 里每 200 ms 把 `mode/test/rgb/ccr/gain` 打到 RTT（`LED_DEMO && DEBUG_RTT`）。
③ **靠它把全部灯效验完（RTT 实证）**：BOOT `255,255,255 / ccr 1000,1000,1000`；IDLE 取了 18 个不同色相（青→蓝、红→黄→绿）；RELEASE 蓝霓虹 `64,0,230 → 0,3,255 → 0,78,230`，蓝 CCR 756~1000；HOLD 绿霓虹 `2,179,0 → 0,124,5 → 41,132,0`，绿 CCR 486~701；FAULT `255,0,0 / 1000,0,0`；CAL 两个值 `255,255,255` ↔ `0,9,255`（500ms 交替）；**三通道单亮 test=1/2/3 → ccr=1000,0,0 / 0,1000,0 / 0,0,1000，映射全对**。
④ **收尾**：`LED_DEMO` 改回 0 → 重编重烧（**校验 37976B**）+ reset，实测 `CCR2(红)=1000 / CCR1(蓝)=0 / CCR4(绿)=0` = **故障红 100%**（无电机→CAN 故障态，预期行为）。
未完成：灯板共阳现在仍只有 3.3V（还没焊 Buck）→ **亮度和白平衡依旧不作数**；未测 5V 下的表现。
下一步：焊上 Buck、有 5V 后复测灯板亮度与白平衡（蓝通道 Vf 最高，5V 下 240Ω 可行；3V3 下只有 ~1.25mA）。

## 2026-09-17
完成：新板（**只焊了主控 `U5` + 灯板**）首次上电调试，用 stm32-dev 技能跑了一圈：
① **发现板子原本根本没有固件**：`0x08000000/04` 都读回 `0xFFFFFFFF`，`verify` 报“37560 字节不同、从 +0x0 就不同”就是这个原因 → `make`（text 37900）+ 烧录，**校验一致（37976 字节）**。
② **固件运行正常**：RCC_CR `HSERDY=1 / PLLRDY=1`、RCC_CFGR `SWS=PLL`（170MHz）、RTT `cyc/ms=169999`、`seq` 严格递增不掉帧、主循环 `avg=126 cyc`。
③ **业务状态是预期的“无电机”态**：`en=0 err=0x0F st=31 fb=0`（没焊 CAN 收发器/没接电机 → CAN 一直发不出去）→ **灯效进入「故障红」**。
④ **TIM4 实测配置与 `led.c` 一致**：`CCER=0x3033`（CH1 蓝 / CH2 红 / CH4 绿 三路使能，`CCxP=1` = 低电平点亮）、`ARR=999`（1kHz）、`CR1.CEN=1`；稳态 `CCR2(红)=356` / `CCR1(蓝)=0` / `CCR4(绿)=0` → **同一瞬间固件点的是「红」而不是「蓝」**。
⑤ **工具用法**：`rtt` 的自动搜索在本板**必然失败**，必须 `--address 0x20000BA4`（`_SEGGER_RTT` 符号地址）才抓得到。
未完成：用户还没回报“现在灯是什么颜色”（他之前说的“一上电蓝灯亮”是**空芯片**时的观察）。
下一步：按用户看到的颜色分叉：**红** → 映射正确、灯板无问题（空片时 GPIO 高阻/ROM bootloader 扫 I2C 会异常点亮，属于正常）；**蓝** → 颜色映射错位，就是 `led.c` 头部警告的那个坑：排线用了 **1:1 直连线**而不是**交叉 1↔4、2↔3**（主板 U9 = PB9/PB7/PB6/3V3，灯板 = 共阳/蓝/红/绿）。要逐通道验针序：用固件自带的 `0x003F`（1只红/2只绿/3只蓝/4全亮/5全灭，**不落盘、优先级最高**），但它只能走 485 写 —— 没 485 收发器时得 `make BENCH=1` 编出 RTT 注入钩子，再用 RTT 下行发 `W 003F n`。

## 2026-09-17
完成：**更正上一条关于 `PB4` 的说法（我上一轮写得不确定。）：**
- **手册里根本没有 “NJTRST” 这个词** —— STM32G431 datasheet 用的是 **`JTRST`**（只在框图里出现一次）。上一轮我写成 NJTRST 是凭记忆。
- **“带内部上拉”我没找到任何依据** —— 那是我推测的，应当删掉，不要当事实用。
- **能确认的只有两条**：① **`PB4` 属于 SWJ 五脚之一（PA13 / PA14 / PA15 / PB3 / PB4）**；② datasheet **§3.37.1 原文明确允许复用**：“Debug is performed using 2 pins only instead of 5 required by the JTAG (**JTAG pins could be re-used as GPIO with alternate function**)”。
- **未能从手册抽到的是**：这五脚**复位后的默认状态**（是 JTAG 复用还是普通输入）。所以正确表述只能是“**复位后到固件配置 GPIO 之间，该脚状态不由设计者控制**”，**不能断言它带上拉或高电平**。
- 想完全避开这段不确定：CN1 的 `TRIG` 可改接 **`PA10`（`U5.32`）或 `PB5`（`U5.42`）** —— 网表里也是空脚，且不在 SWJ 五脚内。
（上一轮同一件事里 **`I2C_SCL/SDA = PA8/PA9 = I2C2`、`CN1` 针序、以及“I2C 线上没有上拉电阻”这三条仍然成立**，不受本次更正影响。）
未完成：PB4 作为 TRIG 是否可接受，取决于外部模块的 TRIG 输入是否做上电边沿检测 — 未确认。
下一步：若外部模块对 TRIG 的上电电平敏感，把 CN1.1 从 U5.41(PB4) 改到 U5.32(PA10) 或 U5.42(PB5)。

## 2026-09-17
完成：核了用户新发的 **09-17 网表（sha 06817f11，23475B）**：**新增一个 5 脚接口 `CN1`（HC-SH1.0-5PWT，1mm 卧贴，C53454623）**，其余与上一版（37f707ce）一致。
**CN1 针序（已核）：** `1=TRIG / 2=I2C_SDA / 3=I2C_SCL / 4=VCC3V3 / 5-7=GND`（5P 座，6/7 是固定脚）。
**MCU 引脚序号→端口名（本轮用板上已验证的连接反推、自洽）：** `U5.30 = PA8`、`U5.31 = PA9`、`U5.41 = PB4`。推导锚点：33/34=PA11/PA12（CAN 实测可用）、35/36=VSS/VDD、37/38=PA13/PA14（SWD 实测可用）、39~42=PA15/PB3/PB4/PB5、43~46=PB6~PB9、47/48=VSS/VDD。
**结论：✅ I2C 接法正确** —— **PA8 + PA9 正好是 `I2C2` 的 SDA/SCL**（I2C2_SDA=PA8、I2C2_SCL=PA9），没接反、没跳外设；且 PA8/PA9/PB4 在网表里都是空脚，与灯板口 U9（PB6/PB7/PB9）、U10、RS485、CAN 都不冲突。
**⚠️ 三条要注意：** ① **I2C 线上没有上拉电阻**（SCL/SDA 各自只有 CN1 和 MCU 两个节点）—— 外部模块自带就算了，不带就要在板上补两颗 4.7k 到 VCC3V3；② **TRIG 用的 PB4 是 NJTRST**（带内部上拉，上电到固件配置之间有高电平毛刺；想干净可换 PA10=U5.32 或 PB5=U5.42，也是空脚）；③ `CN1.4` 给的是 `VCC3V3`，外部模块要 5V 的话得改。
（另：位号 `CN1` 旧版是 SWD 调试口，现在是 U10 —— BOM 对照时别串。）
未完成：用户未说 CN1 接的是什么模块；I2C 上拉、TRIG 电平、CN1 供电电压都要看那块模块才能定。
下一步：确认外部模块是否自带 I2C 上拉、要 3V3 还是 5V、TRIG 的极性/上电要求；必要时补两颗 4.7k 上拉或把 CN1.4 改到 VCC5V。

## 2026-09-17
完成：用户问“这个芯片哪个是 I2C 引脚”。结论：
- **`SCT2332TVBR` 没有 I2C**（TSOT-23-6 六个脚全是电源/反馈：FB/EN/VIN/GND/SW/BST）。
- 有 I2C 的是 **`U5 = STM32G431CBT6`，带 `I2C1 / I2C2 / I2C3` 三个（G431 无 I2C4）**。
- 引脚（来源：STM32G431 datasheet Table 12 + WeAct G431 板的实际可用用法，两处对照；**未在本地逐脚独立复核，LQFP-48 上没有的选项用括号标出**）：
  - **I2C1**：SCL = **PA15 / PB6 / PB8**；SDA = **PA14 / PB7 / PB9**
  - **I2C2**：SCL = **PA9**（/PF6）；SDA = **PA8**（/PF7）
  - **I2C3**：SCL = **PA8**；SDA = **PB5**（/PC9）
- **两个硬约束**：① **I2C2 与 I2C3 不能同时用** —— `I2C2_SDA` 和 `I2C3_SCL` 都落在 **PA8**；② **PA14/PA15 是 SWCLK/SWDIO**，占不得。
**⚠️ 项目特有的一条（这是本轮真正值钱的结论）：这块板上 `PB6 / PB7 / PB9` 已被灯板口 `U9` 占用**（TIM4 三通道 PWM 驱三色灯）—— 所以：
- 要用 **I2C1** 只能改成 **`PB8`(SCL) + `PB9`(SDA)**（或占掉 PA14/PA15 = 与 SWD 冲突）；
- **更干净的是走 `I2C2`（PA8+PA9）或 `I2C3`（PA8+PB5）—— 这三根在 09-17 网表里都是空脚。**
未完成：用户只是“问一下”，没说要用 I2C；板上未加任何 I2C 器件。
下一步：若真加 I2C 器件：优先 I2C2（PA8+PA9）或 I2C3（PA8+PB5）；别再考虑 PB6/PB7（已被灯板占）。

## 2026-09-17
完成：核了 **2026-09-17 网表（两个版本）—— Buck 已从 `U3=SCT2A25STER`（ESOP-8）换成 `U11=SCT2332TVBR`（TSOT-23-6，同步整流）**，并逐项对了手册。
**查实全部正确：**
- **`L3(33µH) 已删，换成 `L1 = CR6045-100M 10µH`**（Isat 3A）→ 对上手册 Table 1 的 5V 行 ✓
- **`D4` 续流管已删** ✓（SCT2332 是同步整流，不需要）
- `R4 158k`(上)/`R5 30k`(下) → `0.8×(1+158/30)=5.01V` ✓；`C11/C18/C20 = 3×22µF` ✓；`C7 100nF` 跨 BST–SW ✓；U11 六个脚（1=FB/2=EN/3=VIN/4=GND/5=SW/6=BST）逐脚核过 ✓
- **`EN` 直接接 VIN 对 SCT2332 是手册明确允许的**：Pin Functions 原文 “This pin supports high voltage input up to VIN supply to be connected VIN directly”；绝对最大 `VIN,SW,EN: −0.3~34V`、`FB: −0.3~5.5V`。**和 SCT2A25 的 EN 只 6V 完全相反。**
**❗ 更正了我上一轮的错误结论：** 我曾把 SCT2332 的前馈网络说成“R3+Cf 串联并在上臂 R1 两端”，**错了**。把手册 Figure 15 的**矢量连线**（`m`/`l` 路径）抠出来读，实际是：**FB 脚 —R3(2k)— 分压中点；Cf(330pF) 并在上臂 R1 两端**。而用户网表里的 `R6 2k` 串在 FB 脚与 `5V_FB` 之间 + `C16 330pF` 跨 `VCC5V`–`5V_FB` —— **与手册一模一样，是对的**。（新写的工具：`.work/sct2332/{pos.py,wires.py}` —— 前者解文字坐标、后者列矢量连线；图是分页 XObject，要按下标取流。）
**⚠️ 发现待改项：** `L1` 的 **Isat 只有 3A**，而手册要求 “saturation current **higher than the current limit**”，SCT2332 的 `ILIM_HSD = 4A(min)/4.5A(typ)/5A(max)`。
**✅ 旧结论作废：** “5V 只能带 1A” —— 现在是 3A 芯片 + 10µH 电感；但 TSOT-23-6 **无散热焊盘、RθJA = 89°C/W**，24V→5V 连续输出约 **1.5A** 就到 100°C 温升（实际负载 ~0.25A 远未到）。
未完成：① `L1` 的 Isat 3A 待换（手册要 >4A）；② `docs/硬件方案.md` / `docs/软件方案.md` §5.2、§3 等**尚未同步到 09-17 换片后的 Buck（U3→U11、L3→L1、D4 删除、EN 可接 VIN）**；③ Buck 输出 7.7V/12V 那个异常（怀疑 FB 虚焊）在旧板上未验，现已换片。
下一步：把 `docs/硬件方案.md` 的 §5.2/§8 与 `docs/软件方案.md` 里的 Buck 段同步到 SCT2332（含删掉“EN 绝不能接 VIN”“散热焊盘”“33µH/2×22µF/Cff 68pF”这些已随换片作废的条目）。

## 2026-09-17
完成：核了备选器件 **SCT2332TVBR（芯洲）**，数据来自官网 goods 页直链的 datasheet（表格又是第三方字体，把 `.work/sct2a25/` 的 `cmap2.py`/`table2.py`/`ctx.py` 拷到 **`.work/sct2332/`** 后用 `table2.py` 解出乱码的 Table 1）。
关键参数：**3.8~32V 输入 / 3A / 同步降压（集成 80mΩ 高边 + 42mΩ 低边，不需要续流二极管）/ TSOT23-6L / 400kHz / 轻载 PSM / 最小导通时间 80ns / **VREF = 0.8V ±1%**（≠ SCT2A25 的 1.2V）。
手册 **Table 1「Recommended External Components」**（列：Vout / L1 / COUT / R1上 / R2下 / R3 / Cf）：
| Vout | L1 | COUT | R1 | R2 | R3 | Cf |
|---|---|---|---|---|---|---|
| 3.3V | 6.5µH | 3×22µF | 93.5k | 30k | — | 330p |
| **5V** | **10µH** | **3×22µF** | **158k** | **30k** | **2k** | **330p** |
| 12V | 22µH | 3×22µF | 422k | 30k | 2k | 330p |
（验算：0.8V×(1+158/30)=5.01V ✓）
**关于前馈电容**：手册有专门的 “Output Feed-Forward Capacitor Selection” 一节，原话是“**if the output capacitor has ultra-low ESR, the converter results in low phase margin**，To increase the converter phase margin, a feed-forward cap Cff is used”——**陶瓷输出电容就是超低 ESR，所以要用**；而且不是单电容，是 **R3+Cf 串联（2kΩ + 330pF）并在上臂 R1（VOUT→FB）两端**（Table 1 和 5V 典型应用图都给这一组：R1 158k / R2 30k / C5 330pF / R3 2k）。
其他：饱和电流手册要求写的是“**大于电流限值**”（不是大于峰值）；输入电容推荐 35V/10µF；固定用 R2≈30k 后算 R1。
⚠️ **陷阱**：它跟 SCT2A25 基准不同 —— **若只换芯片不改分压，0.8V × (1+47.5/15) = 3.33V，不是 5V**。
未完成：用户说“准备做备选器件”，**尚未决定是否替换**；未选具体电感型号（只知要 10µH）。
下一步：若真要换 SCT2332：分压改 R1=158k / R2=30k、加 2k+330pF 前馈、电感换 10µH、**去掉续流二极管 D4**（同步整流不需要）。

## 2026-09-16
完成：应要求把**软/硬件方案与全仓文档同步到 2026-09-16 网表**，并把本轮硬件调试得到的所有注意事项落进文档。
**改了的文件：**
① **`docs/硬件方案.md` 整篇重写**（原版是旧位号，与本版网表完全对不上）。新结构含：以新位号绘制的系统架构图、供电树、4 个接口（U1 机械臂端子 / U7 电机端子 / U10 调试口 / U9 灯板口）、按新位号重列的器件选型总表（含立创 C 号）、保护与 EMC、以及**新增的 §8「硬件设计红线与注意事项」**（EN 绝对最大 6V、散热焊盘必焊、C7/Cff 必装、**5V 轨只能带 1A**、输出电容偏小、24V 电容耐压、D4 用 SS36、灯板共阳必须 5V、位号重名/易混点、上电顺序）。
② **`docs/软件方案.md`**：位号 U1→U5/U6/U8/U7/U10；**共阳 3V3 → 必须 VCC5V**；调试口定型（旧的 J2/J5 待核那条已删除并结清——**U10 不再引出 RS485_TX/RX**，台架必须用 USB‑485 接 U1）。
③ **`docs/转接板功能说明.md` §3.10**：状态灯 `PB14/13/12` → **`PB6 蓝 / PB7 红 / PB9 绿`**（TIM4 + 外置灯板），SWD 改 `U10`，删掉 J5 行。
④ **`interface-adapter/Core/Src/fdcan.c`** 头部注释：`U2 TCAN1042` → `U6 TCAN1042`（并补上 pin5=VIO 接 VCC3V3）。
⑤ **`docs/diagrams/software-architecture.json` + `.html`**：`THVD1450·U3→U8`、`TCAN1042·U2→U6`、`M4F 170MHz·U1→U5`；顺手改掉图里与已决事项矛盾的 “通信超时自动去使能” → “通信超时只置标志(不失能)”。
**核实过没问题的**：`interface-adapter/` 固件里 **没有** PB12/13/14 残留（`led.c` 已经是 PB6/PB7/PB9）。
**本轮新定的一条设计结论**（已写进 PROJECT + 两份方案）：**灯板共阳必须接 `VCC5V`（`U9.4` 从 VCC3V3 改过来），不能接 3V3** —— 该灯珠蓝/绿 Vf 为 2.8~3.2V，3.3V 下只剩 0.1~0.5V 压差、只能出 1~2mA；按 5V 配的那套 240/360/270Ω 就刚好给 ≈8mA，灯板电阻一颗不用动。同时建议把 `U9` 针序改成与灯板 1:1 直连（现在是镜像，必须用交叉线）。
未完成：① `U9.4` 改接 VCC5V、`U9` 针序改 1:1 —— 尚未改进原理图/PCB；② C35 输入电容 25V→50V 未拍板；③ 输出电容 2×10µF→2×22µF 未改；④ L3 的 Isat 1.8A 未换（待确认 5V 实际负载）；⑤ 散热焊盘实物未确认；⑥ Buck 输出 7.7V 的根因（怀疑 FB 虚焊）未验。
下一步：先在原理图上落 `U9.4→VCC5V` 与针序，再处理 Buck 的 FB/散热焊盘两条；文档侧下一轮只需在改完原理图后同步一次网表位号。

## 2026-09-16
完成：核了用户发来的新网表 **`Netlist_Schematic1_2026-09-16.tel`（sha ff307596，22635B）**，逐网络读完。
**✅ 已改好的：** ① **`U3.7`(EN) 已从 24V 上摘掉、现在完全悬空**（不再出现在任何网络里）—— 手册 “Floating to enable” ✓ **致命根因修复**；② **L3 6.8µH → 33µH**（`TNR6045S-330MTF`，C42380237）；③ **Cff 已加：`C16 = 150pF` 跨在 `VCC5V`–`5V_FB`（即 R10 两端）**；④ FB 分压 R10 47.5k / R9 15k；⑤ `U3.9`（散热焊盘）在 GND 网络；⑥ **调试口针序修好了**：新的 U10 = `1=VCC3V3 / 2=SWDIO / 3=SWCLK / 4=GND`（旧 CN1 那个 1=3V3/2=GND/3=SWCLK/4=SWDIO 已废）；⑦ CAN VIO(U6.5)→VCC3V3、CAN 终端 R7(0Ω)+R17(120Ω)、RS485 对称 R18/R19=560Ω + R16 120Ω 均仍正确。
**⚠️ 新发现：L3 感值对了但电流档不够** —— `TNR6045S-330MTF` 额定 1.6A / **Isat 1.8A**，而手册要求 “saturation current rating **greater than 4A**”（芯片限流 4A）。2A 输出时 `ILPEAK = 2 + 0.40/2 = 2.2A > 1.8A` 会饱和；**只在实际负载 ≤1A 时才安全**。已让用户报 5V 实际负载。
**⚠️ D4 对不上**：这份网表写 **`SS36`（SMA，60V/3A）**，而用户说板上装的是 **`SS510`（SMB，100V/5A）** —— **SMA/SMB 焊盘不兼容**，必须统一。
**🟡 未改的：** 输出电容仍 `C8+C17 = 2×10µF`（0402/10V；原来的第三颗 10µF C16 被挪去做 Cff 了），手册要 2×22µF；C35 仍 47µF/25V；散热焊盘只确认了“网表接了 GND”，实物是否真焊上未知。
**🟡 其他：** **LED1/LED4/LED5 全部删掉**，只剩 LED2（5V 指示，R1=4.7k）、LED3（3.3V 指示，R11=2.7k），电流仅 0.6/0.4mA 偏暗；但固件还用 PB12/13/14 驱状态灯、`docs/软件方案.md` 也那么写 → 不匹配。DI_1/DI_2/DO_1/DO_2 仍只接 U1 未进 MCU（旧问题）。新加了 `U9`（4P：PB6/PB7/PB9 + 3V3 + GND）。
未完成：L3 的 Isat 待提升（等用户报 5V 实际负载）；D4 SS36/SS510 待统一；输出电容、C35 耐压未改；散热焊盘实物未确认。
下一步：按 5V 实际负载定 33µH/Isat≥4A 的具体型号；统一 D4；确认并补焊散热焊盘后上电量 VCC5V。

## 2026-09-16
完成：用户澄清：**板上 D4（续流管）实装的是 `SS510`（100V/5A/SMB），不是网表/BOM 写的 SS36**。→ **该条完全不用改**：100V 同时满足手册的硬要求（`must be ≥ VIN(max)`=24V，或 TVS 钳位 45.4V）和 preferred（100V），5A 峰值 > 芯片 4A 限流；而且 **SS510 就是手册设计示例里自己选的那颗**。
**两个修正：**
① **我早先“把 SS510B 降到 SS36”的建议作废** —— SS510B 是 **SMB**、SS36 是 **SMA**，换过去要改焊盘，我当时没提这点；保持 SS510 不动板子。
② 那份第三方原理图审查报告里 “D4 = SS36、反压仅 60V” 是按网表说的，**网表又与实物不符**（与前面“板上分压实装 32.4k/10k 而非网表的 47.5k/15k”同一类）。
**改板清单收敛为**：🔴EN 接法（悬空或 464k/42.2k）/ 🔴散热焊盘补焊 / 🔴加 Cff~136pF（R_top=47.5k）/ 🟡L3→ 33µH（要 2A 的话）/ 🟡输出电容→2×22µF(0805) / 🟢D4 不改 / 🔵C35 输入电容 25V→50V（待用户拍板）。
未完成：改板清单里的项都还没落地；FB 是否虚焊未验证；冒火花根因未定。
下一步：先落 EN 与散热焊盘两条致命项，再加 Cff；电感/输出电容按是否真要用到 2A 决定。

## 2026-09-16
完成：用新写的 `.work/sct2a25/table2.py`（对同一个内容流把 10 张 ToUnicode CMap 全解一遍再挑）挖出了手册里一直乱码的 **Table 2 典型输出电压元件清单**：

| Vout | L1 | COUT | R2(上) | R1(下) | C7(Cff) |
|---|---|---|---|---|---|
| **5V** | **33µH** | **2×22µF** | 95K | 30K | **68pF** |
| 12V | 68µH | 2×22µF | 271K | 30K | 150pF |
| 24V | 100µH | 2×22µF | 191k | 10K | 150pF |

**据此更正我上一轮的错误结论**：我曾把 Cff 按 12V 示例（270k 配 150pF）折算成本板 ~820pF~1nF，**错了**，手册 5V 方案就是 **68pF**（本板 R_top 小一半，取 68~150pF）。已同步修正 `PROJECT.md`。
评了用户拿来的第三方原理图审查报告（5 条 P1）：
- ✅ **L3 6.8µH → 手册 5V 推荐 33µH**、**缺 Cff（推荐 68pF）**、**输出电容 2×10µF → 应 2×22µF** —— 三条**完全正确**，Table 2 原文确认。
- ⚠️ **输入电容耐压那条论据错**：它用“TVS 钳位 45.4V > 25V 额定”论证 —— 这正是本项目已纠正过的方法论错误（MLCC 额定 = 持续工作电压，击穿是额定的 2~3 倍）。**结论仍可支持，但理由要换成**：① 24V+5% = 25.2V 已超 25V；② 直流偏置容值衰减。此条**重启了已决事项**。
- ⚠️ **D4 60V 那条偏严**：手册硬性要求是 “reverse voltage rating **equal to or greater than VIN(max)**”（24V 下 60V 满足）；100V 只是 “**preferred**” 且理由是“允许瞬态到芯片额定 100V”，是给 48V/100V 系统的。（把 SS510B 换成 SS36 是我上一轮的建议，可被重新审视。）
- ❌ **报告漏了最致命的两条**：**EN(pin7) 接 24V 超绝对最大值 6V 四倍**、**散热焊盘必须真焊上**。原因：它只做 “pin-to-net 错配 + 浮空引脚” 检查，**发现不了额定值违规**（EN 接 24V 在 net 层面是“有意连接”）。
未完成：上述审查发现的 Cff/电感/输出电容三项都还没改 BOM；EN 走线已割但分压未补；FB 是否虚焊未验证；芯片丝印未看。
下一步：优先把致命项改掉（EN 分压或悬空、散热焊盘补焊）；再按 Table 2 把 Cff=68~150pF、L3=33µH、输出 2×22µF 写进 BOM。

## 2026-09-15
完成：Buck 新现象与推理（接 EN 割线冒火花之后）：**输出稳定 7.7V、FB 稳定 1.8V**。
① **板上实际装的 FB 分压是 `32.4k/10k`（上一版的值），不是网表写的 47.5k/15k** —— 比值 4.24，对应 5.09V；**这也把之前“5V 节点对地 40k 偏低（理论 62.5k）”的疑点结清了：32.4+10 = 42.4k ≈ 40k**。以后别再照网表的 47.5/15 推算。
② **用手册 OVP 逻辑把这个数判死了**：手册原文 “When FB exceeds **120%** of 1.2V, HS turns off and **LS turns on to discharge the output**；when FB falls below **115%**, HS can turn on again” → 合法 FB 上限是 **1.38~1.44V 的迟滞带（输出被压 ~6.1V）**。而 FB = 1.8V = **150% VREF**，既不在稳压点（1.2V）也不在 OVP 带。**两个合法状态都不是 → 芯片“看到”的 FB 电压 ≠ 万用表量到的 1.8V。**
③ → **首要怀疑：U3 的 FB 脚虚焊/过孔断**。焊盘上仍是分压算出的 7.7×10/42.4 = 1.82V（正好对得上实测），但芯片内部 FB 悬空 → 闭环没闭上、也检测不到 OVP → 停在平衡点 7.7V。
④ **决定性测试（无需示波器）：在 FB 与 `VCC5V` 之间并 100kΩ**（把 FB 抬高 → 芯片向下压输出，**这个方向是安全的**）。等效上臂 32.4k∥100k = 24.5k → 比值 3.45 → 输出应掉到 **~4.1V**。**掉 → 芯片的 FB 接着（芯片本身坏/错料）；纹丝不动 → FB 没接到芯片（补焊/飞线）**。
⚠️ **反过来在 FB 与 GND 并电阻会把输出顶到 ~13V，别那么做**（而且现在 5V 轨上就是 7.7V，CAN 收发器若已焊已被过压）。
⑤ 已搜过网络：**没有 SCT2A25 输出电压异常的实战案例**，只有 AI 拼的二手中文资料站（拍明芯城那篇“FB 超过 1.2V±1% 锁定输出”与手册对不上，不可用）。
未完成：FB 是否真的接到芯片未验证；芯片丝印是否为 `2A25` 未看；没有示波器看过 VOUT 是直流还是锯齿；EN 走线已割但分压未补、芯片未确认换新。
下一步：先做 FB 并 100k 的判别测试 → 再核芯片丝印（应为 `2A25`）→ 若 FB 虚焊则补焊/飞线；同时把 EN 改对（悬空或 464k/42.2k 分压）、补 Cff~1nF。

## 2026-09-15
完成：应要求把 SCT2A25 手册**全文通读**了一遍（之前只读了片段）。为了让表格可读，写了 **`.work/sct2a25/cmap2.py`：按每个内容流挑最匹配的 ToUnicode CMap 再解 `<hex>` 串** —— 比上一代 `cmap.py`（全局套一张，约 1/4 字乱）好得多，绝对最大值表/引脚功能表/电气表全变成可读正文，输出在 `full.txt`。
通读新发现（之前没提的）：
① **散热焊盘**：Layout Guideline 原文 “**Improper soldering thermal pad to ground plate on PCB will cause SW higher ringing and overshoot**” + Pin Functions “Must be connected to ground plane on PCB for proper operation” → **`pin8 对 GND 通` 不等于底部焊盘焊上了**（ESOP-8 焊盘在芯片底下，手工焊最易漏）；这是“反复烧芯片/冒火花”的机制之一。
② **前馈电容 Cff 是漏项**：手册原文 “**Feedforward cap Cf is necessary** to provide flexibility for optimizing the loop stability and transient response”，典型应用图里 R_top 并了一颗（示例 R2=270k 配 150pF）。本板 R10=47.5k → 同时间常数折算约 **820pF~1nF**。
③ **EN 结论不变**：整本手册**没有任何一处**让 EN 接 VIN —— 电气表测试条件只用 `EN=0` / `EN floating` / `EN=1.5V`，典型应用图 EN 只画一个端子，设计示例 EN 配 R3=464k/R4=42.2k 分压；绝对最大值仍是 **EN,FB,TM −0.3~6V**。
④ 其他核过没问题的：BST 电容 0.1µF 且需 ≥10V 耐压（C7 是 50V ✓）；续流管反向耐压 ≥VIN(max)（SS36 60V ✓）；RθJA 42°C/W、RθJC 45.8°C/W；**ESD 额定很低：HBM ±1kV、CDM 仅 ±0.5kV**（手工焊不做防静电很容易打死芯片，是“两颗都坏”的另一个可能）。
未完成：用户还没回答“底部散热焊盘到底焊上了没”；冒火花根因仍未定；EN 走线已割但没补分压；Cff 未补；新芯片未换。
下一步：按顺序：拆 U3→检查/补焊散热焊盘 → 割 EN 走线并确认不通 → 清铜屑 → 补 Cff（~1nF 并在 FB 上臂）→ 装新芯 → 上电，量 VCC5V 应出 5V。

## 2026-09-15
完成：用户按建议割断了 EN→24V 走线，**一上电就冒火花**（根因未定）。
已给出的两个候选原因：**A. 割线操作产生铜屑/毛刺把相邻网络（尤其 24V/GND）短路了**（刀割走线很常见）；**B. 那颗芯片早已被 24V 打坏（EN 超 4 倍绝对最大值），一割线 EN 悬空=使能，坏掉的高边直通 VIN→SW→GND**（我倾向 B，因为他只动了一根线、没碰别的）。
**⚠️ 流程教训（必须遵守的顺序）：割线要在没装芯片的时候做 —— ① 先拆旧 U3 → ② 再割断 EN→24V 走线（量一下 EN 焊盘对 24V 确实不通）→ ③ 清铜屑、酒精擦干净 → ④ 最后装新芯片 → ⑤ 上电。** 否则会出现“割完一上电芯片炸”。我上一条建议漏了这个顺序。
已让用户断电后量：U3 `VIN 对 GND`（稳定几百欧 = 芯片内部已打穿）、`24V_PROT 对 GND`、`F2 是否还通`、`Q3 的 D-S`；并让用“全新同型号芯片 + 同一块表同一档位”做对照法判好坏（不要靠经验值，欧姆档测试电压各表不同）。
未完成：冒火花根因未定；用户未回报上述四个电阻值，也未确认是否已拆下 U3。板子可能新增了短路，**在查清前不要再上电**。
下一步：拿到 VIN/GND、24V_PROT/GND、F2、Q3-DS 四个数 → 定性是“芯片打穿”还是“割线短路”；然后按“先拆芯片→割线→清屑→装新芯→上电”重做，出 5V 后再做 24V 轨带载与 C35 耐压回归。

## 2026-09-15
完成：**Buck 输出 0V 根因定位（确定）：SCT2A25 的 `EN`(pin7) 被直接接到了 24V，而它的绝对最大只有 6V。**
定位路径：从手册 PDF（Identity-H 字体，用 `.work/sct2a25/cmap.py` 解 ToUnicode CMap 才能读）里解出绝对最大值表：`VIN −0.3~110V / BST −0.3~116V / SW −1~110V / BST−SW −0.3~6V / **EN,FB,TM −0.3~6V** / TJ −40~150°C / TSTG −65~150°C`。网表里 `U3.7(EN)` 挂在 `24V_PROT` 上 → **超 4 倍 → “permanent damage”**。
证据链（全部对上）：VIN/EN/GND 三脚电压“正常”（VIN 24V、EN 24V、GND 0V）但 **SW 恒 0V、BST–SW=0V、静态零电流、换几颗芯片表现完全一样**；“上电 0.5W 过几分钟归零” = 芯片在这几分钟里退化/损坏。排查中已逐个排除：C7=100nF ✓、D4 方向 ✓（OL / 0.3V）、L3 通 ✓、VIN/GND 通路 ✓、5V 对地 40k 无短路 ✓、芯片未装反 ✓、原理图拓扑与手册典型应用一致（唯一差别：手册 FB 上臂并了一颗前馈电容 150pF，只影响瞬态）。
**为什么难发现**：典型应用图只把 EN 画成一个端子，正文只说 “Floating to enable the converter”，**只有绝对最大值表里写 6V**。
修法（已告知用户）：① 割断 EN→24V 走线后让 EN 悬空（内部上拉电流源，悬空即使能）；或 ② 按手册 Figure 8 用分压把 EN 压在 1.24~6V（示例 R3=464k 上 / R4=42.2k 下，@24V ≈2.0V）。**修完必须再换一颗新芯片（前两颗已永久损坏）。**
未完成：原理图/PCB 还没改：`U3.7(EN)` 必须从 `24V_PROT` 上拿掉；`docs/硬件方案.md` 也还没同步。用户还没回报是否已割线+加分压并试新芯片。
下一步：让用户割断 EN 走线 + 加分压（或悬空）+ 换新芯片后上电量 VCC5V；若出 5V，再做整套 24V 回归（电机挂同一条轨的带载能力、C35 25V 耐压、有效容值）。

## 2026-09-15
完成：Buck 0V 排障推进（三件事）：
① **网表核了 Buck 段连接，全对**：`C7.1→U3.4(BST)`、`C7.2→D4.1+L3.1+U3.5(SW)`；`24V_PROT` 含 `U3.3(VIN)+U3.7(EN)`；`GND` 含 `U3.6(TM)+U3.8+U3.9(散热焊盘)` → **原理图层面排除**，不用再重查。（注：网表里 Buck 电感位号是 **L3**，`L2` 是 CAN 共模滤波器 CUW21T-900M-N。）
② 从手册补到 **HS 过流后的行为**：“Once trigger HS overcurrent, the present on-time period is immediately terminated, and will force LS turn on a **non-resettable off timer**” —— 即**打嗝**；所以“上电一捅就有电流、然后长时间没电流”是打嗝，不能读成“芯片死了”。
③ ⚠️ **一个差点被误判成故障的排障伪影（重要）**：我上轮让用户把电源限流设到 **100mA**；而 24V 轨有 ~100µF bulk，**100mA 下充满约需 24ms** → 现象就是“**上电一瞬间有电流、电压被拉到 18V、然后回到 24V 就没电流**”。**这是正常充电，不是打嗝也不是短路。** 教训：以后让人限流排障时要一并说明这个副作用，或直接建议 **500mA~1A**。
④ 另一条会重犯的判据：**欧姆档量不出肖特基反接**（万用表欧姆档测试电压低，SS36 正向也可能读成几十 kΩ）—— 用户量到 5V 节点对 GND 40k（理论上 R10+R9=62.5k），**不能据此排除 D4 装反，必须用二极管档**（红笔 SW / 黑笔 GND 应 OL，反接 ~0.2~0.4V）。
未完成：Buck 0V 根因仍未定位。已给的分叉判据未回收：把限流放到 1A 后盯电流表 10s —— 周期跳=打嗝（查 D4 方向）／不跳=完全没启动（翘起 L3 一端空载试）。**SW(pin5) 对 GND 的直流电压一直没拿到。**
下一步：回收分叉结果；若判定为打嗝 → 先用二极管档核 D4(SS36) 方向；若完全没启动 → 翘 L3 一端空载上电看 SW 是否开关（开关=故障在输出侧 U4/U6 输入击穿或电容短路；不开关=换 U3 并补焊散热焊盘）。

## 2026-09-15
完成：读通了 SCT2A25 官方手册（PDF 表格是 Identity-H 字体，普通文本提取拿不到 —— 用自写脚本 `.work/sct2a25/pdftext.py`（解 FlateDecode + 提取 () 字面量）+ `cmap.py`（解析 ToUnicode CMap 解 <hex> Identity-H 串）才拿到）。官方 PIN FUNCTIONS 原文：
① **6 TM = Test mode pin for factory use only. Connect TM to EN pin, ground or leave floating** → **网表里 U3.6(TM) 接 GND 是合法的，排除 TM 这个疑似根因**；
② **4 BST = 必须接 0.1µF 电容到 SW**（“must be connected ... for proper operation”）→ **BST–SW 读 0V 的第一嫌疑就是 C7 漏焊/虚焊/焊错位**；
③ **7 EN 内部有上拉电流源，悬空即使能**（pull below 1.24V to disable）→ 接 24V 正常；
④ **9 散热焊盘电连 GND，must be connected to ground plane on PCB for proper operation**；
⑤ 手册测试条件明写 **BOOT-SW = 5V** → BST 对 SW 正常就是 ~5V，读 0V = 没充上电；
⑥ **VREF = 1.2V** 确认（47.5k/15k 分压正确）；VIN UVLO 上升阈 4.75~5.3V；芯片内部还集成一个低边 MOSFET 周期性给自举电容刷新。
新判据（无示波器）：**量 SW(pin5) 对 GND 的直流电压** —— ≈5V = 在开关（问题在 L2/输出）；0V = 没开关；≈24V = 高边常通。
另指出：**5V 节点对 GND 实测 10kΩ，而理论应为 R10+R9 = 62.5kΩ**（偏低 ~12k 额外通路），让用户分别核 5V→FB=47.5k、FB→GND=15k。
未完成：Buck 0V 根因仍未定；待用户回报 C7 是否在位的目视结果、SW 直流电压、D4 方向、U3 方向与散热焊盘、R10/R9 在板阻值。
下一步：按 ①C7 → ②SW 直流 → ③D4 方向 → ④U3 方向/散热焊盘 → ⑤换芯片 的顺序推进。

## 2026-09-15
完成：Buck 排障进展（24V 进 / 5V 出为 0V）：用户回报 **BST 对 SW = 0V、FB = 0V** → 判定为**自举电容没充上电 → 芯片根本没在开关**（不是分压问题）。FB=0V 只是输出 0V 的必然结果，没有独立信息量。
待用户回报的三个数：**U3 pin3 VIN / U3 pin7 EN / 5V 节点断电对地阻抗**；另让目视 **C7（100nF 跨 BST–SW）是否漏焊或短路**、**U3 ESOP-8 方向 + 底部散热焊盘是否焊上**。
判读表：VIN/EN 都 24V 且 5V 对地不短路 → 芯片坏或底焊盘虚焊；5V 对地 <10Ω → 打嗝（查焊桥、D4 方向）；EN=0V → EN 没连上；VIN=0V → 上游（F2/Q3/走线）。
未完成：Buck 0V 根因未定位；VIN / EN / 5V 对地阻抗三个数未拿到。
下一步：拿到三个数后按判读表定位；若是“VIN/EN 正常但 BST-SW 仍 0V” → 先补焊 ESOP-8 散热焊盘，再换芯片。

## 2026-09-15
完成：上电排障（进行中，用户自己焊的板）：**24V 进 / Buck 5V 出实测 0V**。给出并落盘一条可复用判据 —— **输出 0V 只可能是「芯片压根没开关」，不是分压焊错**：FB 对地短路→芯片拉满占空比→**输出冲向 Vin（会烧 LDO/MCU）**；FB 下臂开路→输出稳在 **≈1.2V**（= SCT2A25 基准）；两者都不是 0V。
0V 的成因清单（按手工焊嫌疑排序）：① **C7 自举电容（100nF，跨 BST–SW）漏焊/虚焊** —— 芯片绝不会开关；② **L2 6.8µH 虚焊/开路** —— SW 有正常波形但输出 0V（最典型）；③ **D4/SS36 装反**（阴极必须朝 SW）→ 打嗝；④ **ESOP-8 底部散热焊盘没焊上** → 完全不工作；⑤ 5V 轨短路 → 打嗝；⑥ EN(pin7)/VIN 没连上。
不用示波器的「芯片活了没」判据：万用表**跨在 BST–SW 上**量到 ~5V。初版本轮误当成 12V 输入，用户纠正为 **24V 输入**（24V 下占空比 ~21%）—— 流程不变。
已提醒：**上电前先把电源限流到 24V/100mA**，否则万一 FB 短地跑飞会烧 LDO/MCU。
未完成：Buck 输出 0V 的根因未定位；等用户回报 VIN / EN / BST-SW / 5V 对地阻抗 四个数。
下一步：拿到四个测点数值后定位：若 BST-SW=0V → 查 C7；若 SW 有波形而输出 0V → 查 L2；若 5V 对地 <10Ω → 查 D4 方向与焊桥。

## 2026-09-14
完成：给 `.c` 里的 **13 个 `static` 内部函数**补完整注释（commit `8bcef9d`）—— 上一轮只把头文件的对外 API 补全了，而 `static` 函数在头文件里没有“家”，所以用户看 `led.c` 的 `render_neon` 时发现还是单行说明。本轮补的：
· `led.c` 7 个（v_lin_of / sin_at / hsv_to_rgb / duty_of / led_out / render_neon / render，含 gamma 2.0 的由来、**色相混合必须在光域做**、CH1=蓝/CH2=红/CH4=绿 的引脚映射陷阱、H|V 打包与呼吸周期）
· `autocal.c` 2（fail/seek）、`control.c` 2（gripper_state_tick 的两条“不能只看…”坑、scan_delay 为何不能用 HAL_Delay）、`modbus.c` 3（crc16 / append_crc / tx_finish）、`nvm.c` 1（nvm_crc 为何故意不覆盖后加字段）、`regs.c` 1（白名单与 0x10 原子性）、`damiao.c` 2（SendRaw / SendSystemCommand 的 0xFC/0xFD/0xFB/0xFE 命令字）、`main.c` 1（DWT_Init 必须在 SystemClock_Config 之后）、`stm32g4xx_it.c` 1（fault_report 栈帧顺序与自动复位上限）。
**同时修正了上一轮写进 PROJECT.md 的不完整约定**：“对外 API 文档在头文件、`.c` 里只留实现说明”会让未来的会话漏掉 static 函数的文档 —— 已改成“对外 API 在头文件，**static 在 `.c` 定义处**按同样格式写”，并把“故意不补”的名单（`MX_*_Init`/`HAL_*_MspInit`/`*_IRQHandler`/`main`/`SystemClock_Config`/`system_stm32g4xx.c`）写进去。
**验证**：`make` 通过、零告警；`text/data/bss` 始终 **37900/76/4660**（纯注释）。已推 `feature/firmware-import`，MR !2 head ref 核对一致。
未完成：**MR !2 未合**；固件**仍未烧录**（改版 PCB 未做、J-Link 未接）；三色针序未目视确认。
下一步：合 MR !2 → 等改版 PCB（或飞那 4 根线）→ `stm32-dev.py flash` + `reset` → 发 `0x003F`=1/2/3 校三色针序 → 跑 G 组。

## 2026-09-14
完成：把工程代码里的注释整体整干净，并给对外 API 补全完整函数注释（共 3 个 commit：`71449e5` / `1bd30dd` / `10ee5f2`，已推 `feature/firmware-import`）：
① **清 CubeMX 模板垃圾**（16 个文件）：264 个 `USER CODE BEGIN/END` 标记、16 份 ST 版权/`@file` 块(换成一行出处说明)、分节横幅、`@brief` 空壳、单词式分块标签、单行框线。`system_stm32g4xx.c` **故意不动**（ST 自己的库文件）。
② **修 3 处过时/不准确注释**：main.c 的「标定模式」只写了手动兜底流程→改成「主路径 = 写 0x0034=1 一键自动标定，手动模式是兜底且不改电机零点」；control.c 跑飞保护写的「超出 MIT ±PMAX 16 位量程」在本机 mode 4(float p_des) 下不成立→改成真实成因并说明 `kp>0.1` 门控的来历（顺带确认保护没失效：Damiao_Init 默认 kp=5.0）；damiao.c 的 mode 初值注释。
③ `g_mit_*` → `g_tx_*`（它统计的是【任意模式】的发帧耗时，RTT 标签 `MIT/TIM6` 同步改 `TX/TIM6`）。
④ **5 个头文件补完整 API 注释**：`app.h`(93→208 行) / `led.h`(34→95) / `damiao.h`(74→160，含达妙协议速查：4 种模式的帧 ID 与字段、特殊命令 0xFC/0xFD/0xFB/0xFE、寄存器帧 0x7FF+0x55/0x33/0xAA) / `modbus.h`(72→100) / `blackbox.h`(32→53)，每个函数都是“作用/逐个参数/返回值/前置/副作用/示例”。
⑤ 顺手修一处会编译失败的笔误：`#define MB_TX_TIMEOUT_MS 20u;` 多写了个分号。
**验证**：全程 `make` 通过、零告警，`text/data/bss` 始终 **37900/76/4660**（纯注释+改名不改代码生成）；清 CubeMX 注释那轮另外用“剥掉两份代码的注释后逐字符比对”—— **16 个文件全部一致**。
未完成：**MR !2 未合**；固件**仍未烧录**（改版 PCB 未做、J-Link 未接）；三色针序未目视确认。
下一步：合 MR !2 → 等改版 PCB（或飞那 4 根线）→ `stm32-dev.py flash` + `reset` → 发 `0x003F`=1/2/3 校三色针序 → 跑 G 组。

## 2026-09-14
完成：CMSIS 再瘦一轮（commit `6c5299a`，已推 `feature/firmware-import`）：
① **删 `CMSIS/Core/` 整个目录** —— 它的 `Core/Include/` 与 `CMSIS/Include/` 是同一份 core 头的**两份拷贝**（各 24 文件 / 1.7MB），而 **Makefile 的 `-I` 用后者、`.vscode` 原指前者**（编译与编辑器各用一份）→ 统一到 `CMSIS/Include`。
② 删 `CMSIS/Include` 里**别的内核**的头：`core_cm0/0plus/1/3/7/23/33/35p`、`core_armv8m*`、`core_sc000/sc300`、`mpu_armv8.h`、`tz_context.h`、`cmsis_iccarm.h`。**保留名单按 `#include` 传递闭包算**（从工程真正编译的文件出发，得到 `core_cm4.h`+`cmsis_*.h`+`mpu_armv7.h` 共 8 个；闭包是纯文本扫描、不管 `#if`，宁可多留）。
③ `.vscode/c_cpp_properties.json`：去掉已失效的 `CMSIS/Core/Include`，并**补上原来漏掉的 `Middlewares/SEGGER_RTT`**（否则 IntelliSense 报找不到 `SEGGER_RTT.h`）。
④ `Drivers/README.md` 补第二轮精简的说明与恢复办法。
**验证**：`make clean && make` 通过、**text 仍是 37900（与精简前逐字节同尺寸 → 没删到要用的东西）**；**Drivers 9.0 → 5.9 MB / 104 文件**（最初 72.7 MB / 1161 文件），源码树 **150 文件**。推送后**专门查了 MR !2 的 head ref = `6c5299a` = 本地 HEAD**（用上一轮记下的方法，不再想当然）。
未完成：**MR !2 未合**；固件**仍未烧录**（改版 PCB 未做、J-Link 未接）；**三色针序未目视确认**。
下一步：合 MR !2 → 等改版 PCB（或飞那 4 根线）→ `stm32-dev.py flash` + `reset` → 发 `0x003F`=1/2/3 校三色针序 → 跑 `test_all.py COM21`（或经机械臂 `test_485_all.py`）的 G 组。

## 2026-09-14
完成：代码审查 + 优化（commit `5d94bfe`），加验一件真 bug：
① **`grip.c` 的 `bridge_led_tick()` 去抖是假的** —— 它跑在自由循环的主循环里（~1MHz），而 `g_hold_cnt++` 是“每次调用 +1”，注释/文档写的 100ms 实际只有 **~0.1ms**，力矩瞬间过阈就判“夹住”。加 **1ms 限流**修正（顺带省掉每圈一次 `rad_to_gripper_pos` 浮点除法 + 两次配置寄存器查询）。
② 清掉拆模块时留下的**重复注释**（grip.c/regs.c 各 3 份，其余 2 份，只留一份文件头）；`regs.c` 大 switch 里的“通信诊断/标定”两处**分节标记保留**（不是重复）。
③ `led.c`：`hled` → `htim4`，与 htim6/hiwdg/hfdcan1/huart2 命名一致。
**验证**：`-Wall -Wextra -Wshadow -Wconversion` 在 Core/Src **零告警**（54 条全来自 Drivers 的 ST 头文件）；`-fanalyzer` **零发现**；`make clean && make` 通过且零告警，text 37884 → **37900**（+16 = 限流那几行），data/bss 不变。
（注：前两次报 -Wshadow/-Wconversion “干净”是**假的** —— PowerShell 把 `-I` 串成单个 argv，编译 fatal error 被我的 grep 吞了；改用数组传参重跑才是真结果。已进坑）

**MR 重开**：本轮先查了旧 MR 的真实状态 —— `refs/merge-requests/1/head` 一直停在 `e8e0ec5`，**后面 4 个提交根本没进 MR**（GitLab 的 MR ref 只在源分支被推送且 MR 活着时更新，说明 !1 已被关掉）。因为 GitLab 对“同一源→目标已存在 MR”会**忽略** `merge_request.create`，只能换分支名：新推 **`feature/firmware-import`** 并自动建 **MR !2**；用 `refs/merge-requests/2/head` 核对 = 本地 HEAD `5d94bfe` ✓、合并 ref 里含全部 5 个提交 ✓。旧远端分支 `feature/initial-import` 已删。
未完成：**MR !2 未合**；固件仍**未烧录**（改版 PCB 未做、J-Link 未接）；三色针序未目视确认；CMSIS 还有 ~3MB 可再瘦（CMSIS/Include 与 CMSIS/Core/Include 重复，用户未答复做不做）。
下一步：合 MR !2 后，等改版 PCB（或飞 4 线）→ `stm32-dev.py flash` + `reset` → 发 `0x003F`=1/2/3 校三色针序 → 跑 G 组。

## 2026-09-14
完成：上一轮“只规划”的三件事本轮全做完并推上远端分支（每个都带验证）：
① **精简 66.7 MB / 170 项**（Drivers **74 MB → 9 MB**：删 `CMSIS/DSP`(44MB)/`NN`/`RTOS`、非 G431 的器件头与启动模板、Makefile 没编译的 HAL 模块、`MDK-ARM/`、`.mxproject`）。**验证**：`make clean && make` 通过、**text 38064 与精简前逐字节同尺寸**、零告警。
② **拆 `main.c`（1871 → 639 行）**为 `gripper_map`/`nvm`/`regs`/`control`/`autocal`/`grip` + `app.h`（应用层共享状态 + 模块间原型）。**验证**：`.map` 逐函数对比 —— **无函数丢失**；11 个符号由“被内联”变为各自独立副本、`bridge_read`/`bridge_write` 相应变小；text 38064 → **37884**，data/bss 不变（76/4660）。
③ **补注释**：12 个模块加文件头（职责/关键参数/坑）+ README 四层模块表。**验证**：text 仍 37884（注释不改代码）。
三个 commit 已推 `feature/initial-import`：`906b21a`(精简) → `0c44906`(拆模块) → `da0cc21`(补注释)。
途中踩到并修了的坑：脚本按“模块名”猜头文件保留名单会漏 `hal_gpio_ex.h`/`Legacy/`（改成算 `#include` 传递闭包）；`rm_glob` 把 `Inc/Legacy/` **目录**当文件删了；`USED` 正则漏掉无模块后缀的 `stm32g4xx_hal.c`（改成从 Makefile 解析真实编译清单）；生成 app.h 时 extern 带上了初始化器变成定义；搬到模块里的共享变量（`g_grip`/`g_ac_run`/`g_boot_done`…）忘了进 app.h；文件头里写 `HAL_UART_*/HAL_FDCAN_*` 的 `*/` **把注释提前闭合**导致编译失败。
未完成：**MR !1 未合**（等用户点 merge）；**固件仍未烧录**（改版 PCB 未做、J-Link 未接）；**三色针序仍未目视确认**。
下一步：改版 PCB 到手（或飞 4 线）后：`stm32-dev.py flash` + `reset` → 发 `0x003F`=1/2/3 校针序 → 跑 `test_all.py COM21`（或经机械臂跑 `test_485_all.py`）的 G 组。

## 2026-09-14
完成：本轮把固件建成了 git 仓库并上远端：仓库根 = **`interface-adapter/`**（只放固件），首个 commit `e8e0ec5`“初始导入：夹爪 RS485-CAN 转接板固件”（1209 个文件，含完整 Drivers + MDK-ARM 原始状态）；新增 `README.md`（模块结构/编译烧录/**改代码前必读的 8 条约定**）、`.gitignore`（排除 build/ 与全部编译产物，逐项核过都是 0）、`.gitattributes`（`* -text`，防假 diff）。
踩到两个 GitLab 坑并绕过：① **443 关着、明文 http 被 GCM 拒** → 只能走 SSH（`id_ed25519_leap`）；② **`main` 是保护分支，`jin.tian` 推不上去** → 改用 `git push origin HEAD:refs/heads/<分支> -o merge_request.create -o merge_request.target=main`，已推分支 **`feature/initial-import`** 并自动开 **MR !1**（http://gitlab.leaprobotics.ai/hardware/gence-gripper/-/merge_requests/1）。本地 `.work/backup/` 已删（远端分支即备份）。
另外：清理脚本 `tmp/cleanup_project.py` 已跑过一轮（**66 MB / 173 项**，日志 `.work/cleanup_log.txt`），但为让第一个 commit 是完整原貌，已从备份**还原**（Drivers 1161 文件 / 72.7 MB、MDK-ARM 73 文件，已核）。
未完成：**MR !1 未合**（等用户点 merge，或放开 main 保护）；66 MB 精简**未重跑/未提交**；`main.c` 未拆；注释未补。
下一步：重跑 `tmp/cleanup_project.py` → `make clean && make` 验证没删错（缺文件就从 `e8e0ec5` `git checkout` 取回）→ 提交；然后拆 main.c 为 6 模块 + app.h（用 .map 逐函数对比大小验证等价）。

## 2026-09-14
完成：用户明确**改版 PCB 还没做好 → 暂停烧录**（上一轮那句“把 J-Link 接回去”作废）。为确认“新固件烧到手上这块旧板会不会冲突”，用 `PCB/Netlist_Schematic1_2026-09-07.tel`（**09-13 那份已被用户从 `PCB/` 里替掉、不存在了**）核了 MCU 的 4x 脚：**`U5.43/44/46`（PB6/PB7/PB9）不出现在任何网络 = 空脚**，整个 40~48 脚里只有 `U5.45`(PB8/BOOT0) 有下拉 → 结论：**烧旧板电气上安全，但指示灯全灭**（旧三颗灯在 PB12/13/14，新固件已不再驱动）—— 已写进 PROJECT.md（含飞 4 线按**信号**接已有灯板提前验灯的接线）。
途中修了 `tmp/nettool.py` 的**续行判据**：网表长行折行，**含 `;` 的行才是新网络**；我原先用“不以引号开头”当续行判据 → 把后面所有网络都吞进前一个，输出过两次垃圾结果。
未完成：仍未烧录（等改版 PCB，或飞 4 线）；**三色针序（B9=绿/B7=红/B6=蓝）仍未经目视确认**（只有“网表镜像+阻值排序+用户口述”三条推导证据）。
下一步：改版 PCB 到手（或飞好 4 线）后：`stm32-dev.py flash --elf build/interface-adapter.elf --device STM32G431CB` + `reset`，然后发 `0x003F`=1/2/3 校针序、跑 `test_all.py COM21`（或经机械臂跑 `test_485_all.py`）的 G 组。

## 2026-09-14
完成：三色灯固件**已写完并编译通过**（尚未烧录）：① 新建 `Core/Src/led.c`+`Core/Inc/led.h`——TIM4 三通道 PWM（PB6=CH1 蓝/PB7=CH2 红/PB9=CH4 绿，AF2，1kHz，`OCPOLARITY_LOW` 低电平点亮）、65 项正弦表 + HSV→线性光 RGB + **gamma 2.0**（写 60 就是感知 60%）的灯效合成、5 种灯效（上电白/待机彩虹/蓝霓虹/绿霓虹/红故障 + 蓝白交替标定）、调试模式（只红/只绿/只蓝/全亮/全灭）；② `main.c`：`bridge_led_tick()` 重写成「抓取判定→选灯效」（排除区只挡“进入夹住”、退出不受限、取 |T|、掉料计数走“闭合意图”）、新增寄存器 `0x001C~0x001E`（抓取/灯效/掉料计数）与 `0x0025~0x0027`+`0x0037~0x003F`（增益/亮度/色相/周期/调试）、NVM **尾部追加 12 字且不参与 CRC**（旧记录照过校验，擦除态整块用默认值）；③ `main.h` 换 RGB 引脚宏、`gpio.c` 摘掉旧灯（LED 引脚+AF 全归 `Led_Init()`，“先写高再切 AF”）；④ Makefile 加 led.c。
**验证到哪一步**：`make` 退出码 0、**零编译告警**，text **32368 → 38064**（128KB flash）。（“make 退出码 1”是 PowerShell 把 ld 的 newlib `_close/_read` note 当成错误，不是构建失败。）
**同步完的文档/脚本**：`docs/软件方案.md`（GPIO 表换成 PB6/7/9+TIM4、旧三颗灯行删除、加镜像针序告警）、`docs/485命令全表.md`（新增 §2.10 三色灯与抓取判定 + §4.14 校针序/调灯 + §5 读写白名单与连续区间全面更新；**重新自动复核 53/53 帧 CRC 全对**）、`tmp/test_all.py` 新增 G 组、`tmp/robot/test_485_all.py` 新增 `group_G` + `RO_SINGLE` 扩充（两脚本 `py_compile` 均通过）。
未完成：**没烧录、没实测**：板子在机械臂上而 **J-Link 已被拔**（实测 USB 上无任何 SEGGER 设备、无残留进程；`cleanup` 后仍然 `Cannot connect to the probe`）。三色针序（B9=绿/B7=红/B6=蓝）还是“网表镜像 + 阻值排序 + 用户口述”三证据推出，**没用眼睛验过**——烧录后先用 `0x003F`=1/2/3 校一次。
下一步：把 J-Link 的 SWD 重新接到转接板（或把板子拿回台架接 COM21），然后 `stm32-dev.py flash --elf build/interface-adapter.elf --device STM32G431CB` + `reset`，再跑 `test_all.py COM21`（或经机械臂跑 `test_485_all.py`）看 G 组。

## 2026-09-14
完成：本轮把三色灯方案**定案**（仍未写码）：按用户新要求从“静态色码”改成**动效**——上电白 2s → 2s 后链路（485 收过帧 + CAN 能直读 0x51）不正常则红；正常但没动过则**待机多色循环**；夹爪动/松开则**蓝霓虹**（蓝色系内缓慢游走）；检测到夹住则**绿霓虹****（夹住时保持）；空爪静止 >10s 回待机彩虹；掉料直接回蓝霓虹（不做单独报警态）；亮度统一 60%；`EXCL` 改 50。
同时把新网表解析完、硬件事实敲实：`BOM/Netlist_*_2026-09-14.*` 已进 `PCB/` 且**灯板是独立原理图（Schematic2）**。灯板 `Netlist_Schematic2_2026-09-14.tel`：`U9`=SH1.0-4P卧贴，`U9.1`=共阳（LED7.1/3/5 并）、`U9.2/3/4` = R12 **240Ω**/R20 **360Ω**/R13 **270Ω** 分别到 LED7.6/.4/.2；`LED7`=TC5050RGBF08-3CJH-AF53A。主板 `Netlist_Schematic1_2026-09-14.tel`：`PB9/PB7/PB6/3V3 = U9.1/2/3/4`。**两板针序镜像**（交叉 1↔4、2↔3）才能同时满足“用户口述 B9=绿/B7=红/B6=蓝”与阻值排序（240蓝/270绿/360红）——已写进 PROJECT.md 坑位上方。
**解析工具**：`tmp/nettool.py`（网表长行折行，grep 会拿到片段；“续行”判据是**该行不含 `;`**，不是“不以引号开头”——我第一版搞错了，把后面所有网络都吞进前一个）。
下一步：写 TIM4 三通道 PWM + 灯效状态机（建议单独 led.c）+ 485 配置寄存器 0x0037~0x003F / 状态 0x001C~0x001E，并同步 docs/软件方案.md 的 GPIO 表与 docs/485命令全表.md。

## 2026-09-14
完成：本轮只做规划、**未写任何代码**。产出三色灯状态方案（待用户批准）：① 颜色语义：**黄=爪里没东西（待机/就绪）· 青=正在动 · 绿=夹住 · 红=异常 · 蓝=标定 · 紫=通信 · 白=上电**；② 10 个状态按优先级：0 上电白常亮（到电机识别完成，最长 6s）/ 1 电机·CAN 故障红常亮 / 2 **掉料红快闪 5Hz 锁存 5s** / 3 通信失联（`0x001A`=1 且已使能）紫慢闪 / 4 自动标定蓝慢闪（`0x0035`=1）/ 5 手动标定模式蓝常亮（`0x0030`=1）/ 6 运动中青快闪 / 7 **夹住绿常亮** / 8 已使能空爪就绪黄常亮 / 9 未使能黄慢闪；③ 抓取/掉料判据：`T_HI=0.30N·m` 进入、`T_LO=0.15N·m` 退出（滞回）、`EXCL=80/1000` 两端排除区、去抖 100ms、**进入“夹住”受排除区限制而退出不受**（否则“被推到近闭合端+力矩掉下来”这个最典型的掉料反而报不出来）、掉料需“`p_des < pos-50`（闭合意图）”才报警（上层命令张开的正常放开不报）；④ 建议新增只读 `0x001C`（抓取判定 0未知/1空/2夹住/3掉料锁存）与 `0x001D`（灯状态码），可写 `0x0037~0x003A`（T_HI/T_LO/EXCL/锁存时间）——**均接在现有连续区间尾部，不破区间，现有脚本一行不用改**；阈值若要掉电保持，NVM 尾部追加+版本号加一，旧版本读出来用默认值（本机 0/1595 不受影响）。
同时核实了硬件：PB6/PB7/PB9 全空且正好是 TIM4_CH1/CH2/CH4(AF2)；`docs/软件方案.md` 与网表仍是旧三颗灯；`PCB/灯板.zip` 只有 Gerber、没有网表（限流阻值未知）。
未完成：方案未获批准、未写码；原理图/网表/`docs/软件方案.md` 的 GPIO 表仍是旧灯（PB12/13/14 高电平）。
下一步：等用户回答三个问题（共阳是否接 5V（接 3.3V 绿/蓝点不亮）／EXCL 取 80 还是 40／是否现在就用 TIM4 上 PWM），然后改 `main.h`+`gpio.c`（**先写高再配输出**，否则共阳硬件上电三色全亮）+`bridge_led_tick()`，并同步 `docs/软件方案.md` 与 `docs/485命令全表.md`。

## 2026-09-13
完成：把品牌 logo `leapgence.svg`（原样：主标题 **LeapGence**，副标题 **UMI TRACE** + 4 条装饰色块，141×47）的副标题整行换成 **GenceGripper**，输出 `leapgence.svg`（工作区根目录，12 055B；主标题路径/色块/viewBox 逐字节未动）。验证：XML 可解析（2 path + 4 rect）；用 WPF 按最终文件重新渲染 PNG 后目视确认（`tmp/svg_logo/verify.png`）。
未完成：本文件处于工作区根目录，未纳入任何 BOM/文档引用；若后续 logo 还要改（换字重、换字体），`tmp/svg_logo/` 里的 `build.ps1`/`finalize.ps1`/`verify.ps1` 可直接复用。
下一步：矢量文字改字形的可复用管线：PowerShell + WPF（`Add-Type PresentationCore,PresentationFramework,WindowsBase`）→ `FormattedText(...).BuildGeometry()` → `[PathGeometry]::CreateFromGeometry()` → `ToString(InvariantCulture)` 得到路径；字号 = 目标 cap 高 / 0.716（Arial）。三个必踩坑：① `Geometry.Parse()` 返回**只读**对象，设 `Transform` 抛 "read-only state"，必须先 `.Clone()`（或干脆在 `<g>` 上写 `transform="translate(x y)"`）；② WPF 输出的路径串带 **`F0`/`F1` 前缀，SVG 不认**，必须剥掉（SVG 默认 nonzero，与 `F1` 等价，字形内孔正常）；③ `[Regex]::Replace(input, evaluator)` 别手抖把第 3 个位置参数写成 `1`，否则只替换一次。

## 2026-09-13
完成：核了用户问的 **立创 C784540 = `TC5050RGBF08-3CJH-AF53A`（天成 TCWIN）** 全参数：**SMD5050-6P（5.0×5.4×1.6mm）/ 独立二极管（非共阳）/ 主波长 620-625、515-520、465-470nm / Vf 红 2~2.2、绿 2.8~3、蓝 3~3.2V / 亮度@20mA 红 600~800、绿 1500~2000、蓝 400~600mcd / ESD 2000V / 0.2W 20mA / −40~+85℃**。与先前短名单 **`E6C1206RGBC3UDA`（C375571，1206 共阳）** 对比：C784540 **亮度高 5~10 倍**、体积大 10 倍、6 脚独立需外部短接阳极；两者都必须 5V 共阳驱动（蓝 Vf 3.0~3.2V 在 3.3V 下点不亮）。限流起点（5V 共阳+GPIO 灌电流）：5mA 档 红560/绿430/蓝390Ω；8mA 档 红360/绿270/蓝240Ω。
**板上现有 LED 焊盘是 `LED0603-RD`（0603，NCD0603Y2 / C89811）—— 5050 与 1206 都贴不上，必须改板或做转接小板**（用户已确认接受做小板）。
未完成：LED 选型仍未定（C784540 vs C375571），未写进 BOM，三颗限流电阻未定值。
下一步：定选型（体积优先→C375571／好焊+醒目→C784540）→ 看 `C784540_sch.png` 核六脚阳/阴极排列 → 写入 BOM 含三颗限流电阻。

## 2026-09-13
完成：为「多色指示灯珠」选型（用户明确要**贴片**），核了三颗，并定出一条**驱动硬约束**：
① **硬约束**：**绿/蓝光的 Vf ≈ 2.9~3.4V，3.3V 驱动点不亮（或极暗）** → 多色/三色 LED **必须从 VCC5V 驱动**；做法：**共阳接 5V，每色阴极经限流电阻由 GPIO 拉低**（3.3V GPIO 拉高时 LED 两端只有 5−3.3=1.7V < Vf，能可靠关断）。板上现有 LED1/4/5 是 `GPIO→1kΩ→阳极、阴极接 GND` 的 **3.3V 源驱动**，只适合红/黄（Vf≤2.2V）。
② 贴片候选（已核）：
   - **`E6C1206RGBC3UDA`（亿晶源）— 立创 C375571**：SMD-4P **1206**（3.2×1.6×0.9），**RGB 真三色 共阳**；Vf 红1.9~2.2 / 绿2.9~3.4 / 蓝2.9~3.4V；亮度 红70~150 / 绿400~800 / 蓝70~150mcd。共阳正好匹配「必须 5V」⭐
   - **`E6C0805RVGC2UDA` — C375548**：SMD-4P **0805**（2×1.2），红/翠绿双色，**4 脚独立**；Vf 红2 / 翠绿3V；亮度 红800 / 翠绿150mcd。
   - **`0603RG0-S9J3YYD0N-E0`（佰强光电）— C55382829**：**0603 无极（2 脚）**红/翠绿，正接红/反接绿、不能同时亮，外置只需 2 线；**立创参数未填，用前看手册**。
   - （直插备选：`3A9RYGWA` C52034639，3mm 红/黄绿 共阴，Vf 都 1.8~2.2V → 3.3V 可直驱；用户已排除非贴片。）
③ 板上可用的 LED 资源：**U5.26/27/28 三路 MCU 直驱**（各经 1kΩ）；LED2/LED3 是 5V/3.3V 电源常亮指示，不占 GPIO —— RGB 三色正好用满这三路，还能 PWM 混色。
未完成：三颗均未定选型、未写入 BOM；限流电阻只是起点（5V 共阳起点：红 1k / 绿 2.2k / 蓝 390Ω）。
下一步：定选型（推荐 E6C1206RGBC3UDA / C375571）→ 核引脚定义（C375571_sch.png）→ 写进 BOM（含三颗电阻）。

## 2026-09-13
完成：应要求又补了一份**物流视角**的对比文件：**`PCB/器件对比_物料视角.md`**（按器件/物料聚合，不看位号；**旧版网表不带型号，只有封装+值**，所以“旧版”列只能到封装/规格这一层）。物资级差异要点：**删除** 470µF/50V 直插电解、SS56 串联二极管、2.54mm 双排针；**换型** 输入保险丝（2920 PTC → 1812 自恢复 BSMD1812-200-30V）、反接 MOS（→PJQ4465AP −60V）、续流管（→SS36）、输入 TVS（→SMF28CA）；**新增** 0805 10µF/35V（补 bulk）+ 6 颗 0402 电阻（R14–R19）；**改值** FB 分压 32k/10k→**47.5k/15k**、RS485 偏置 120Ω→**560Ω（对称）**；**小型化** 电容/电阻 0603→0402（除 1kΩ）、调试口双排针→1.25mm 单座。
未完成：现在 `PCB/` 下有两份互补的对比文件：`网表变更对比_2026-09-07_至_最新.md`（按位号，含位号复用陷阱）与 `器件对比_物料视角.md`（按物料），内容未合并。
下一步：需要时：以最新网表为准核对 BOM 与贴片；细到具体位号看第一份，只看物料看第二份。

## 2026-09-13
完成：应要求把 **09-07 网表 vs 最新（cd378415）** 的完整器件差异做成了文件：**`PCB/网表变更对比_2026-09-07_至_最新.md`**（已写入并 prezent）。核心内容：**新增 10 个位号**（C19、CN1、F2、Q3、R14/R15/R16/R17/R18/R19）；**删除 13 个**（C10、C15、C67、D7、H1、H2、Q1、R12、R13、R22、R23、R24、R26）；**⚠️ 位号复用/功能互换 6 处**（**U2**：保险丝→0805 10µF 电容；**C1**：470µF 电解→100nF；**D4**：SMB 双向 TVS→SS36 续流管；**D5**：SMA 串联管→SMF28CA 输入 TVS；保险丝 U2→F2；反接 MOS Q1→Q3）；以及约 30 条“同位号但规格/封装/型号变了”的全表（含 C17 100nF→10µF、R1 100k→4.7k、R9/R10 120Ω→15k/47.5k 等）。
未完成：文件中列了 6 条遗留待确认（C35 25V、有效容值、CN1 针序、DI/DO 单端、U8 链接笔误），与前几条 SESSIONS 的 open 有重叠，未去重。
下一步：需要继续时：按新图核对 BOM 与贴片，**重点那 6 个位号复用/功能互换的件**。

## 2026-09-13
完成：**更正上一条（我把 24V bulk 耐压问题说重了）**：
① **MLCC 只有一个电压值——额定电压（Rated Voltage）= 允许长期持续施加的最大直流电压**；手册**不给「绝对最大」**。真正的**介质击穿电压通常是额定的 2~3 倍**（厂家内部余量，不标），所以**短时 44.4V 浪涌（1.78 倍、µs 级）通常扛得住** —— **拿「TVS 钳位 44.4V > 25V」去类比 P-MOS 的 VDS 绝对最大是错的**（半导体超绝对最大会立即雪崩；MLCC 的额定是「持续工作/寿命」指标）。
② 真正的两个关注点：**(a) 轨压上限** —— 24V 若 ±5% 就是 25.2V，**超 25V 额定**；**(b) 直流偏置容值衰减（比耐压更影响）** —— X5R 在 24V 偏置下有效容量可能只剩 1/4~1/3，所以「24V bulk ≈101µF」是**标称值**，实际可能仅 **30~40µF**；删掉 470µF/50V 后真正缺的是**有效储能**，不是耐压。
③ 用户随后更新网表（sha cd378415，14787B），**只动了两处**：**C2 从 22µF 25V（三星 CL21A226MAQNNNE，C45783）→ 22µF 35V（HRE芯声 CGA0805X5R226M350MT，C6119901）**；**C67 删除、新增同款 C4（22µF 35V）**。**C35（1206 47µF，华瓷 HGC1206R5476M250NSPJ，C22367835）仍是 25V** —— 它现在是全板唯一的 25V 件（24V 轨上）。总容仍 ≈101µF。
未完成：① **C35（1206 47µF）仍 25V** —— 要整条轨 ≥35V 就得换 1210（1206 的 47µF 基本到 25V 是上限）；若轨压确认稳定 ≤24V，留着可接受；② **确认机械臂工具口 24V 的精度/上限**（会不会超 25V）；③ **按「有效容值」重估 24V bulk 够不够**（标称 101µF ≠ 有效）；④ CN1 针序（1=3V3/2=GND/3=SWCLK/4=SWDIO）与常见 SWD 不一致，且 RS485_TX/RX 不再引出；⑤ DI/DO 仍单端；⑥ U8(THVD1450DR) 的 datasheet 链接指向 thvd1452.pdf。
下一步：确认 24V 轨压上限 → 定 C35 是否升 35V/1210；并按有效容值评估 bulk。

## 2026-09-13
完成：用户给出**带完整参数的网表**（sha c1816ef7，14805B——含型号/厂商/立创C号/耐压），据此**核清了此前所有待核项**：
① **F2 = `BSMD1812-200-30V`（30V/2A/1812，C960026）** ✓ 正是推荐的；
② **D5 = `SMF28CA`（**28V 截止**、双向、SOD-123FL，**C19077517**（R+O宏嘉诚，非志得的 C5161254））** → **Vrwm=24V 的疑虑解除**（28V 对 24V 轨余量正常）；
③ **D4 = `SS36`（60V/3A/SMA，C16015）** ✓；④ **D1 = `BZT52C15`（15V 齐纳）** ✓；
⑤ **U2 = `HGC0805R5106K350NSLJ`（0805 10µF **35V**，C33546007）** ✓；
⑥ **Q3 封装疑虑解除**：立创把 **C6147631 的封装标为 DFN3333-8**，EDA 里叫 POWERPAK-1212-8 —— **同一个立创库条目**，不是真的对不上；
⑦ 所有 0402 10µF（HGC0402R5106M100NTEJ，10V）都在 3.3V/5V 轨上（C3/C8/C17），**24V 轨上没有**；100nF=50V、2.2µF=25V。
**⚠️ 新重点：24V 轨的 bulk 电容耐压偏紧** —— `C2`/`C67` = 22µF **25V**、`C35` = 47µF **25V**，对 24V 标称只剩 1V（96%），且 **低于 TVS 钳位 44.4V**；原设计有颗 **470µF/50V** 电解在扛余量，现已删除，该轨再无 >35V 的电容。→ 建议 bulk 升 **35V/50V 档**（代价是封装变大）。（与 P-MOS −30V vs 45V 钳位同类问题。）
未完成：① **24V bulk（C2/C67/C35）升 35V/50V** ← 优先；② **Q3 的 VGS 仍未确认**（网表/立创参数页都不带 VGS 额定，公开 datasheet 是图片，只能翻 PDF；门极被钳到 −15V，须 ≥±20V，兜底：把 D1 从 15V 换 12V）；③ CN1 针序（1=3V3/2=GND/3=SWCLK/4=SWDIO）与常见 SWD 不一致，且 RS485_TX/RX 不再引出；④ 24V bulk ≈101µF 是否够（电机挂同一条轨）；⑤ DI/DO 仍单端；⑥ 小问题：U8(THVD1450DR) 的 datasheet 链接指向 thvd1452.pdf。
下一步：定 24V bulk 电容的耐压档（35V/50V）与封装。

## 2026-09-13
完成：用户再次更新网表（sha a85a10f5，4023B）：**C4（0402 10µF）删除，新增 `U2` = C0805 10µF**（U2.2→24V_PROT、U2.1→GND）→ **上轮标为「最优先」的「24V 轨 0402 10µF 耐压违规」已修复** ✓；同时**新增 `C2` = C0805 22µF** 也挂在 24V_PROT → 24V bulk 从 ~79µF 升到 **≈101µF**（C35 47 + C67 22 + C2 22 + U2 10）。
现在 24V 轨上的 0402 只剩 C9（100nF，50V 档存在，OK）；C3/C8/C17 在 3.3V/5V 轨上，0402 10µF 可用。
⚠️ **新问题：位号 `U2` 是电容却用了「U」前缀**（U 一般是 IC）—— BOM/贴片/维修极易当成芯片搞错；且 U2 在 09-07 那版里是**保险丝**的位号。建议改成 C 开头。
未完成：① **U2(0805 10µF) 的耐压要 ≥25V（最好 50V，直接用原设计 C440198）**；C2/C67(0805 22µF) 与 C35(1206 47µF) 要 **25V 档** —— **网表不带耐压，必须自己确认**；② F2 型号要选 30V/2A 档（如 BSMD1812-200-30V，C960026）；③ D5 应为 SMF28CA（C5161254）；④ **Q3 的 VGS 未核**（公开源全是图片/被墙，只能翻 PDF）；⑤ Q3 封装 `DFN3333-8L` vs 网表 `POWERPAK-1212-8` —— 最稳是从立创库直接调 **C6147631** 的封装，并逐脚核 S/D；⑥ 24V bulk ≈101µF 是否够（电机挂同一条轨）；⑦ CN1 针序（1=3V3/2=GND/3=SWCLK/4=SWDIO）与常见 SWD 不一致，且 RS485_TX/RX 不再引出；⑧ DI/DO 仍单端。
下一步：确认 U2/C2/C67/C35 的耐压档（网表看不出）；顺手把 `U2` 位号改成 C 开头。

## 2026-09-13
完成：对比用户再次更新后的 **Netlist_Schematic1_2026-09-13.tel**（sha 9f5de8af，3985B）vs 上一版（1ffbdfc0，3914B），改了两处：
① **保险丝改成 `F1812`（F2）** —— 采纳了「30V+2A 最小到 1812」；
② **D3（SOT-23-3，SET23A36L02）删除，新增 `D5`（SOD-123FL，双向）** —— 即 **SMF28CA（28V/钳位 44.4V）**；D5.2→24V_PROT、D5.1→GND 接法正确；
③ 钳位从 33V 变 44.4V 后，对 Q3(−60V) 与 D4(SS36 60V) 仍各 **~15.6V** 余量 ✓。
**⚠️ 新发现（耐压违规，最要紧）**：**24V_PROT 上的 10µF 电容被封成了 `C0402`** —— 但 **0402 做不到 25V/50V 的 10µF（0402 10µF 上限约 6.3~10V）**。原设计是 **0805 10µF **50V**（Murata GRM21BR61H106KE43L，立创 C440198）**。→ **这颗必须回 0805/50V**（C3/C8/C17 在 3.3V/5V 轨上，0402 10µF 可用）。另注：**C4/C9 的值在原理图与网表里是对调的**（图 C4=100nF/C9=10µF；网表 C4=10µF/C9=100nF）——改的时候别改错那颗。
**网表不带耐压/元件值** → C35(1206 47µF)、C67(0805 22µF) 的 25V 档都存在（TDK C3216X5R1E476M / 风华 0805X226M250NT），但得自行确认选的电压档。
未完成：① **24V 轨的 10µF 回 0805/50V（C440198）** ← 优先；② F2 型号要选 30V/2A 档（如 BSMD1812-200-30V，C960026）；③ D5 应为 SMF28CA（C5161254）；④ **Q3 的 VGS 未核**（已确认公开源全是图片/被墙，搜不到，必须翻 PDF）；⑤ **Q3 封装 `DFN3333-8L` vs 网表 `POWERPAK-1212-8`** —— 最稳是从立创库直接调 **C6147631** 的封装，并逐脚核 1/2/3 与 5/6/7/8 谁是 S/D；⑥ 24V bulk 从 470µF 降到 ~79µF 是否够（电机挂同一条轨）；⑦ CN1 针序（1=3V3/2=GND/3=SWCLK/4=SWDIO）与常见 SWD 不一致，且 RS485_TX/RX 不再引出；⑧ DI/DO 仍单端。
下一步：优先修 24V 轨的 0402 10µF（回 0805/50V）。

## 2026-09-13
完成：核了「自恢复保险丝」的选型边界（立创在库）：
① **1206 在 30V 档上限就是 1.1A**（1206L 系列里 1.5A/2A/2.6A/3A 那几档全是 **6~16V**：如 1206L150/6V、1206L200/16V）。
② **1210 也不行**：30V 档只有 ~0.1A（SMD1210-010-30）。
③ → **要「30V + 2A」，最小封装只能到 `1812`**：**`BSMD1812-200-30V`（佰宏，1812，30V，Ihold 2A / Itrip 4A，立创 C960026）**，仍比原 2920 小很多。
**修正（我上轮的错误结论）**：**PTC 保持电流随温度降额**（85°C 约剩 60%）—— `1206L110/30V`（C49318383）的 1.1A 在高温下 ≈0.7A，对实际连续 0.55A（夹爪 <0.5A + Buck ~0.05A）只剩 **1.3 倍**余量；且端口允许的 **2A/<1s 对它是 1.8×、动作时间 300ms → 有误跳风险**。（我先前说「2 倍余量够用」是没算降额，属错误结论。）
未完成：保险丝未定选型、BOM 未改。二选一：① **要按 2A 设计 → BSMD1812-200-30V（C960026）**，焊盘需从 F1206 改成 1812；② 只有实测确认永不出现 2A且连续 <0.5A 时，才可继续用 1206L110/30V（C49318383，保住现有 F1206 焊盘）。
下一步：定保险丝选型（按是否要过 2A）→ 改 BOM 与焊盘。

## 2026-09-13
完成：核了 Q3 所选 **PJQ4465AP(-AU)** 的参数（多源一致）：**DFN3333-8L / −60V / −16A / RDS(on) ≤4848mΩ@−10V（ID −5A）、≤65mΩ@−4.5V / AEC-Q101 / RthJC 6.3°C/W、RthJA 62.5°C/W / −55~150°C**。（立创标 typ 40mΩ/55mΩ、15A —— 与 max/16A 是 typ-max 差异。）
**VGS 未能核实**：公开源要么被墙（DigiKey/TME/element14），要么把参数表做成了**图片**（世强原文表1/表2 全是截图）—— **只能看 PDF 手册的 Absolute Maximum Ratings**，别再花时间搜网页。
**⚠️ 封装名对不上**：PJQ4465AP 是 **DFN3333-8L**，而 09-13 网表里 Q3 的封装写的是 **POWERPAK-1212-8**（L3.1-W3.1-P0.65）。两者**可能只是 EDA 库叫法不同，也可能焊盘真的不同** —— 投板前必须叠焊盘图比对：引脚数、0.65mm 间距、散热焊盘尺寸，以及 **1/2/3 与 5/6/7/8 谁是 S 谁是 D**（决定会不会装反）。
未完成：PJQ4465AP 的 **VGS 额定未确认**（门极被 15V 齐纳钳到 −15V，故须 ≥±20V）。若手册只到 ±16V/±12V → **兜底：把 Zg 从 15V 齐纳换成 12V**（该管 Vth 仅 1.7V，−12V 足够开通）。
下一步：翻 PJQ4465AP 的 PDF 手册第 1 页 Absolute Maximum Ratings，核 VGS 与 ID；并叠 DFN3333-8L vs POWERPAK-1212-8 焊盘图。

## 2026-09-13
完成：用原理图核对 5V Buck 段 + 澄清两处：
① **D4 = SS36（SMA，60V/3A）确认无误**（用户一直用的就是它，不须改）；对 D3 的 33V 钳位余量 27V。
② **SCT2A25 脚位（按原理图符号）：pin1=FB / pin2=NC / pin3=VIN / pin4=BST / pin5=SW / pin6=TM / pin7=EN / pin8=GND / PAD=GND**，且 **EN(pin7) 已接 VIN** —— 所以网表里 U3.2 悬空是正常的，**不要当成“EN 缺失”**（我早先就因此误判过）。分压 R10 47.5k 上 / R9 15k 下 = 5.000V ✓；续流管阴极在 SW ✓。
③ **这份 .tel 网表不带元件值**：所有二极管的 Value 字段都是 `'{Value}'` 占位符 → **值只能看原理图**（我因此把 D4 当成 SS34、让用户改成 SS36，实际用户用的就是 SS36）。另 **C4/C9 的值与原理图对调了**（图 C4=100nF/C9=10uF；网表 C4=10uF/C9=100nF），同在 24V_PROT 上去耦，功能无影响。
④ **D3 = SET23A36L02-ES，钳位 33V**（用户确认）。因 `Vrwm < Vbr < Vcl`，**33V 钳位反推 Vrwm 只能 ≤24V**（立创页两套自相矛盾数据：24V/26.5V/33V 与 36V/40V/50V，必有一错）。
未完成：**D3 的 Vrwm 待核**：若真是 24V，则用在 24V 轨上零余量（应 ≥26.4V，最好 28V），轨压一超 24V 就开始导通/漏电。若确为 24V → 建议改 **SMF28CA（SOD-123FL，28V/钳位 44.4V，立创 C5161254）**；现在 Q3(PJQ4465AP −60V) 与 SS36(60V) 对 44.4V 钳位都有 ~16V 余量，所以不必再追求低钳位。
下一步：翻 D3 手册确认 Vrwm；若 24V 则换 SMF28CA。

## 2026-09-13
完成：逐行对比了用户改后的 **Netlist_Schematic1_2026-09-13.tel**（vs 09-07 旧版）。**之前记录的 5 条隐患已在新网表中修好**：① **CAN 收发器 U6.5(VIO) 已接 VCC3V3**；② RS485 偏置改成**对称 560/560**（R18 上拉A、R19 下拉B）+ 终端 120Ω（R16 120 + R15 0）；③ CAN 终端从 240Ω 修成 **120Ω**（R7 0Ω + R17 120Ω）；④ 反馈分压改成 **R10 47.5k / R9 15k = 5.000V**；⑤ **SS56（串联二极管 D5）已删除**，Buck 输入直接接 24V_PROT。
本版小型化改动：保险丝 U2(F2920 PTC) → **F1(F1206)**；反接 P-MOS Q1(SOIC-8) → **Q3(POWERPAK-1212-8)**；输入 TVS D4(SMB双向) → **D3(SOT-23-3，两路并联接 24V、公共脚 GND)**；续流管 D7(SMB) → **D4(SMA)**；调试口 H1+H2(2×2.54 4P) → **CN1(1.25mm)**（**RS485_TX/RX 不再引出**；针序 1=3V3/2=GND/3=SWCLK/4=SWDIO）；**470µF 输入电解 C1 删除**（C1 改为 0402 100nF 作 NRST 电容），24V_PROT bulk 仅剩 C35(47µF)+C67(22µF)；阻容多改 0402。
未完成：**F1(1206) 载流存疑**：1206 级 PTC 保持电流通常仅 0.5~1.5A，而该轨要过 2A；若是普通 1206 保险丝，额定电压常只 32~63V，低于 TVS 钳位 ~44V。另需核：Q3(PowerPAK-1212-8) 的 VDS ≥−60V / Vgs ≥±20V；D3 的 Vrwm ≥26V 与公共脚定义；D4(SMA 续流管) 的 VRRM ≥60V / IF ≥3A（40V 档不可用）；CN1 针序与常见 SWD 不一致是否为有意；470µF 去掉后 bulk 仅 69µF 是否够。
下一步：拿到 F1 / Q3 / D3 / D4 的具体型号后逐个核参数。

## 2026-09-13
完成：输入保护小型化，两条：
① **续流二极管 SS510B 可降档**（现用 5A/100V/SMB，余量过大）。关键：**VRRM 下限由「输入 TVS 钳位 ~44V」决定，不是 24V 标称** → 必须 ≥60V，**40V 档（SS34）不可用**。电流：D=5/24≈0.21、5V 轨 ~0.25A → 二极管平均 ~0.2A、峰值 ~1.2A（ΔIL≈1.9A）→ 3A 够，**SOD-123FL 的 1A 档不够**。推荐 **SS36（MDD，SMA/DO-214AC，60V/3A，Vf 0.7V@3A，立创 C16015）**，比 SMB 小一档。
② **输入 TVS 可换小**：同 28V 档 → **SMF28CA（志得，SOD-123FL，200W，钳位 44.4V，立创 C5161254）**；想「更小 + ESD 更强」必须**换技术路线**——改用 ESD 阵列 **SET23A36L02-ES（静芯 ElecSuper，SOT-23，标 ±30kV/40pF/500W@8/20µs，立创 C19170912）**，但**其钳位在立创页面自相矛盾（描述 33V vs 参数表 50V）→ 用前必须看手册**。双路器件在单条 24V 轨上的接法：**两路并联接 +24V、公共脚接 GND**（能力翻倍；未用一路不要悬空）。
未完成：以上均未定选型、BOM 未改（SS510B、SMBJ28CA 仍为原型号）。
下一步：若采纳小型化：把 BOM 的 D_catch → SS36（SMA，C16015）、输入 TVS → SMF28CA（SOD-123FL，C5161254）；两者都需改焊盘。

## 2026-09-13
完成：本轮再挖，把「小封装」这条线找齐了，并**更正上一轮的推论**（小封装 ≠ 只能 120mΩ）：
① 【新首选】**PANJIT（强茂）`PJQ4465AP-AU_R2_000A1`** —— **DFN3333-8**（约 3.3×3.3mm，面积约 SOP-8 的一半），**−60V / 15A / Rds(on) 40mΩ@−10V（55mΩ@−4.5V）/ Pd 20W / Vth 1.7V / Qg 22nC / AEC-Q101 车规**，立创 **C6147631**。损耗：0.5A→20mV/10mW；2A→80mV/160mW；4A→160mV/640mW —— 20W Pd 下无压力。
② 备选 **NVTFS5116PL（XBLW）**：DFN3x3-8L，−60V/20A/65mΩ@−10V/Pd 25W，立创 **C22451448**。
③ 又淘汰两颗：Toshiba **SSM3J356R**（SOT-23F，−60V 但 **Rds(on) 300mΩ**、Id 仅 2A）、Nexperia **PSMP033-60YE**（−60V/~33mΩ 但 **LFPAK56**，焊盘不兼容）。
④ **关键更正**：之前「SOT-23 级 120mΩ 是上限」只是「**SOT-23 封装内**的上限」；换到 **DFN3333** 就能同时拿到「小 + 低内阻（40mΩ）」。
⑤ 新注意：**DFN3333-8 是无引脚 + 底部散热焊盘，需回流焊并打过孔散热，手工焊比 SOP-8/SOT-23 难**（当前在手工焊测试，需权衡）。
未完成：选型未定、BOM 未改；PJQ4465AP 的 **Vgs 额定未核**（立创参数页未列；门极被钳到 −15V，须 ≥−20V）；VBA2625 与 AO4407A 的逐脚一致性仍未核。
下一步：定选型（小封装优先→PJQ4465AP/DFN3333-8；最小→NCE60P04Y；不改板→VBA2625），核 Vgs 后改 BOM 输入保护行 + 注明是否需改焊盘/散热过孔。

## 2026-09-13
完成：按「优先小封装」重筛 −60V P-MOS，并定出一条选型硬约束：
① **硬约束（新）**：本板门极网络是 `Rg=100k 下拉 + Zg=15V 齐纳`，稳态把 Vgs 钳到 **−15V**，所以替换管的 **Vgs 额定必须 ≥ ±20V** —— 这一条卡掉了绝大多数比 SOT-23 更小的微封装（SC-70/SOT-323 多为 ±8~±12V）；VDS 仍须 ≥ −60V。
② 小封装候选（已核参数）：**NCE60P04Y**（新洁能，**SOT-23-3L**，−60V/−4A，Rds(on) 120mΩ@−10V / 170mΩ@−4.5V，**Vgs ±20V**，Pd 1.5W，立创 **C102608**）；**NCE60P04SN**（新洁能，SOT-23-6L，−60V/−4A，120mΩ，Pd 1.5W，立创 C7462063，Vgs 待核）。
③ 损耗（实际负载 <0.5A 典型 / 2A 上限 <1s）：120mΩ 时 0.5A→60mV/30mW；2A→240mV/480mW（SOT-23 靠敷铜散热，**别按 2A 连续设计**）。
④ 淘汰：TPZXMP4A57E6TA（台舟，SOT-23-6，**40V**）、NVATS5A107PLZT4G（onsemi，TO-252 更大且 **40V**）；未找到比 SOT-23-3 更小且满足 Vgs≥±20V 的 −60V P-MOS。
⑤ 权衡结论：**小封装 = 必须改焊盘/重画 PCB**（AO4407A 是 SOP-8），不是换料；不改板则仍首选 VBA2625（SOP-8，25mΩ/10A，C416236）。
未完成：选型未定、BOM 未改；未核 NCE60P04SN 的 Vgs 额定；未核 NCE60P04Y 的 SOT-23-3L 引脚定义（G/S/D 次序，改焊盘要用）；VBA2625 与 AO4407A 的逐脚一致性仍未核。
下一步：定选型（改板→NCE60P04Y 或 不改板→VBA2625），然后改 BOM 输入保护行 + 写明备注（用于覆盖 SMBJ28CA 的 ~45V 钳位 / 是否需改焊盘）。

## 2026-09-13
完成：按上轮定下的方向（换 VDS ≥−60V P-MOS）去筛在库型号并核了参数：① 【首选】**VBA2625**（VBsemi/微碧，**SOP-8**，−60V/−10A，Rds(on) **25mΩ@−10V** / 28mΩ@−4.5V，VGS ±20V，Vth −1.7V，立创/LCSC **C416236**）——与 AO4407A 同为 SOP-8，**可免改焊盘**；② 小封装备选 **NCE60P04SN**（新洁能，**SOT-23-6L**，−60V/−4A，Rds(on) 120mΩ@−10V，Pd 1.5W，立创 **C7462063**）；③ SOP-8 兜底 YFW8P06S（佑风微，−60V/−8A，105mΩ）；④ 大厂牌 DMP6110SSD-13（Diodes，SO-8，−60V/3.3A，Rds(on) 未核，3.3A 对 4A 峰值偏紧）。关键判据：本项目实际电流 **<0.5A 典型 / 2A 上限（末端口 <1s）/ 4A 电机峰值** → **内阻可放宽，连 120mΩ 都能用**（0.5A 时仅 60mV/30mW，2A 时 240mV/480mW < Pd 1.5W）。
未完成：选型未定、BOM 未改（AO4407A 仍为 −30V）；**未核 VBA2625 与 AO4407A 的逐脚引脚定义是否一致**（SO-8 P-MOS 有 S=1/2/3·G=4·D=5/6/7/8 与 D=1/2/3·G=4·S=5/6/7/8 两种排法）——这是能否真正免改焊盘的唯一决定项。
下一步：核 AO4407A 与 VBA2625 的引脚排布一致后，把 BOM 输入保护行改成 VBA2625（C416236），备注写明“用于覆盖 SMBJ28CA 的 ~45V 钳位”。

## 2026-09-13
完成：硬件侧咨询给了三条结论并核过参数：① 去 SS56 + 外灌 5V 可与 24V 并存：SS56(D_ser) 是 5V 轨↔24V 轨的唯一通路（用户确认去掉后电机仍得 24V），去掉即两域隔离；5V 回流被续流管 D7 挡住、24V 不会倒灌 5V；前提是共地、5V 极性正确且 ≤5.5V、不把 SS56 装回。② SMBJ28CA 可换同档通用料，但【换 TVS 解决不了「钳位 ~45V > AO4407A 的 −30V」】：24V 轨各档钳位都在 39~53V（24CA 38.9 / 26CA 42.1 / 28CA 45.4 / 30CA 48.4 / 33CA 53.3V；SMBJ 标准表值，以厂家手册为准），且降到 24/26V 档会在 24V 标称下误动作。③ 正解是换 VDS ≥−60V 的 P-MOS，且【必须保持标准 SOP-8 焊盘】：Nexperia BUK6Y33-60P 虽为 −60V/25A/48mΩ、AEC-Q101、就是「反接电池保护」用途，但封装是 LFPAK56(Power-SO8)，【与 SOP-8 不兼容、不能 drop-in】（立创已标停产）。
未完成：未选型、BOM 未改（AO4407A 仍为 −30V）；PCB 仍在分阶段焊接调试。
下一步：按立创在库筛「标准 SOP-8 / −60V / Rds(on) 与 AO4407A 的 13mΩ@−10V 相当」的 P 沟道 MOSFET，替换 BOM 输入保护那一行。

## 2026-09-13
完成：按用户要求产出 **`docs/485命令全表.md`**（485 命令全表 + 操作手册）：① 协议层（115200 8N1、站号默认1、只支持 FC 03/06/10、广播不回应、大端+CRC 低字节在前、回包前静默 0.35ms、单帧读125/写123、MB_BUF_SZ=320→突发上限约40帧）；② 异常码表（0x01 功能码 / 0x02 地址**或值**非法 / 0x03 长度）；③ 标准区 0x0000~0x0088 逐条表（含**写前置条件**：0x0000=1 需非标定中/非自动标定/ID≠0/模式合法/err<2；0x0001 需失能且 1..4；0x0007/0x0008 需失能且值=1）；④ 别名区 0x0100~0x0202（含 0x0105/0x0109 不置 g_p_des_valid 这个不对称）；⑤ **读写白名单与可连续区间**（0x03/0x10 跨过空洞整帧回 0x02；0x000B~0x000C 可读但 0x000D 不可读，所以不能连读 3 个）；⑥ 13 条可直接抄的操作序列；⑦ 故障码表（达妙手册：0失能/1使能正常/3、4、5校准与传感器/8超压/9欠压/A过流/B、C过温/D通讯丢失/E过载 + 板子自加的 0x0F CAN error-passive）；⑧ §7 红线与 14 条坑。
**验证方式**：文档内**所有完整帧的 CRC 用标准向量 `01 03 00 00 00 0A → C5 CD` 校验过的实现逐条自动复核——首轮 43 帧中找出 4 帧手写 CRC 写错（`94 07`/`14 1D`/`8C 18`/`C8 04`，其中 `14 1D` 是数量=4 的 CRC 被当成数量=2），修正后 **43/43 全过**；剩余 9 个“片段”核实为有意占位符（`xx`/`CRCL CRCH`）或 Python `.hex()` 的小写输出注释，非错误。
下一步：接上层 `ag_gripper.py` 真机联调（仍不下发机械臂运动指令）；或把耐久拉长到 300 轮看慢漂移。

## 2026-09-13
完成：**机械臂工具口耐久测试 50 轮全行程通过**（`soak_485.py --cycles 50 --force 30 --speed 3`，走 SSH→192.168.77.11→`--mode dobot --ip 192.168.5.4`）：总 78.7s、均 1.57s/行程；峰值 |力矩| **0.89N·m**（闭合端夹持）；温度 32/37℃ → 最大 34/37℃（线圈仅 +2℃）；跑飞计数 0→0、CAN 失联 0→0、`0x001A`=0。闭合端稳定 +0.0400rad / 回读 29~30 / state 3 / T +0.84~0.86；张开端稳定 +1.6300rad / 回读 1000 / state 1 / T +0.10~0.20 —— 50 轮两端落点完全可复现，无漂移、无累积升温。收工失能（`0x0200=0`）、停在张开端。
跑完复检：`quick_check.py` 三件事全绿 + `test_485_all.py --group D` **9/9**；端点仍 **0/1595**（耐久后标定未漂）、温度 35/37℃。全程未发 `EnableRobot`、未发任何运动指令，只读写 485 寄存器 + 夹爪自身开合。
下一步：接上层 `ag_gripper.py` 真机联调（仍不下发机械臂运动指令）；或把耐久拉长到 300 轮 / 10 分钟看慢漂移。

## 2026-09-13
完成：**机械臂工具口 485 全量测试跑通：57/57（只读 15/15）。**
① 堵点：`leap-os` 开发栈（`start-dev.sh` → `python -m edge_runtime`）独占左右两臂的 29999，首跑报 `Connection refused, IP:Port has been occupied`；`probe_ports.py` 实测 30003 拒连、30004 是只读二进制流 → **29999 是唯一通道**，先问用户停掉才继续。
② 误判与纠正（本轮最大教训）：第一轮只读后我看到 `温度=0/0` 且 `0x0018=0`，判定“电机没应答，去查 24V/CAN”；用户回“电机是有电的”。写 `motor_probe.py` 用 `0x0040=0x0A` 直读一帧电机寄存器 —— **回包了：0x0A=4**，说明 CAN 通、电机在 ID 3、模式 4；再看 SWD 的 FDCAN 计数（CCCR=0x1000 是 PXHD、INIT=0；PSR LEC=0/ACT=Idle/EP=0/EW=1；TEC=125；TXBTO=7）也支持“总线有对端”。两个信号之所以骗人：**温度恒 0 是因为板子失能不发控制帧、电机就不回反馈帧**；**`0x0018` 只反映“开机那次扫描”没找到（那时电机还没上电），不是“现在不在”**。把 `quick_check.py` 的判据改成 `0x0040` 直读，并改正了上一轮记错的那条。
③ 全量 57/57：全部可写寄存器写+回读、`0x0040` 直读电机（0x0A=4 / Gr=7.0 / 0x51）、上层启动流程 165→等3s→165→轮询、状态机 2→1、`0x0103` 0→0.01rad / 1000→**1.59rad 且 T 仅 +0.07N·m**、静默>超时 `0x001A=1` 但不失能且在恢复后清 0、`0x0007` 重设零点（0x51 归零）、端点写+`0x000D` 存 NVM、手动标定模式（含“标定中拒使能”）、**一键自动标定 0x0035=2 / 0x0036=1595 / 7.2s**；收工失能、端点 0/1595、温度 34/37℃。（少的 6 项是原始帧用例，越疆网关不透传。）
④ 新增/更新工作区脚本：`quick_check.py`（5 秒体检，判据已修正）、`motor_probe.py`（0x0040 直读 + 可选扫 ID 1..16）、`probe_ports.py`（端口可用性）。全程**未发 EnableRobot、未发任何运动指令**。
当前：板子经工具口供电、失能、端点 0/1595、电机在 ID 3/模式 4。
未完成（重新整理）：① 机械臂上只跑了 `test_485_all.py`，台架那套专项（链路自检 31/31、耐久 30/30、断 CAN 自愈）仍在本机做；② MB_BUF_SZ 连发上限 40 帧；③ ADC 欠压/过压保护（软硬件唯一缺口）；④ LED 行为需人眼。
下一步：接上层 `ag_gripper.py` 真机联调（仍不下发运动指令）。
下一步：接上层 ag_gripper.py 真机联调；或把 DEBUG_RTT 关掉编一版交付固件。

## 2026-09-13
完成：**机械臂工具口 485 第一次真接触：链路已验通，缺电机侧。**
① `edge_runtime` 独占 29999 挡路：首跑 `test_485_all.py --read-only` 报 `Connection refused, IP:Port has been occupied`。用 `ss -tnp`/`pgrep` 查到是用户手动起的 leap-os 开发栈（`bash scripts/dev/ubuntu/start-dev.sh` → `python -m edge_runtime`，pid 1282903）同时握着 5.4:29999、5.5:29999 和 5.4:30004。探查替代通道：写了个 `probe_ports.py` 逐个试，**30003 拒绝连接、30004 是只读二进制反馈流 → 29999 是发工具口 Modbus 命令的唯一通道**。用户停掉后即可连。（没杀用户进程，先问了他。）
② 只读跑通 **15/15**：`SetToolMode(1,0)`/`SetTool485(115200,N,1)`/`SetToolPower(1)`/`ModbusCreate(127.0.0.1,60000,1,1)` 全部返回 0；读到 `0x0020=3 / 0x0021=19 / 0x0023=1 / 0x0024=4 / 0x000B=0 / 0x000C=1595`，诊断区（含 `0x0062 已处理帧数=102`）与坏帧快照区都能读，`0x0063 ORE=0`、`0x0064 CRC=0` → **机械臂→工具口485→转接板这条路径本身没问题**。
③ **但 CAN 上没有电机**：`0x0013/0x0014 温度=0/0`、`0x0018=0`、`0x0010/0x0012=0` —— 开机自动识别扫遍 1..16 一个都没回，退回了 NVM 的 3/19。判断是**电机 24V 或 CAN 还没接**（用户原话只提到“接好到机械臂的 485”）。
④ 新增 `quick_check.py`（5 秒体检：链路/电机/标定三件事，退出码 0/2/3 区分“链路不通/电机没应答/正常”）；`test_485_all.py`、`probe_ports.py`、`rsh.py`/`rput.py` 都已在工作区。
⑤ 全程没动用户的设备：只调了工具口初始化那 4 条命令，**没发 EnableRobot、没发运动指令、没写任何寄存器**。
当前：板子已由机械臂工具口供电、失能、标定 0/1595；电机侧待接。
下一步：用户把电机 24V/CAN 接好后，先 `python3 quick_check.py`（应报“电机在应答”）→ `test_485_all.py --read-only` → `test_485_all.py` 全量。
未完成：电机侧没接好，全量测试未跑。
下一步：用户接好电机 24V/CAN 后：`python3 quick_check.py` → `test_485_all.py --read-only` → `test_485_all.py`。

## 2026-09-13
完成：按用户要求为"上机械臂接口测试"做准备：写好了新的全命令/全功能测试脚本并先在本机验证完毕，等用户接线。
① 连上 **SSH 192.168.77.11**（paramiko，user leaprobotics002 / 密码 002），确认：主机 `enp1s0 = 192.168.5.100/24`，**机械臂控制柜 192.168.5.4 可达**（ping 0.5ms、29999 开），`/usr/bin/python3 3.12` 带 serial/numpy/requests。**工作区 = `/home/leaprobotics002/projects/jin/gripper_test`**（里面已有 test_gripper.py / dobot_api.py / mb.py / mock_dobot_server.py / README.md）。
② 先读了**上层真正的调用序列**（`projects/leap-os/hardware/src/leap_hardware/robots/dobot/sdk/gripper_base.py`）：`SetToolMode(1,0)` → `SetTool485(115200,parity,1)` → `SetToolPower(1)` → `ModbusClose(0)` → `ModbusCreate(127.0.0.1,60000,slave,1)`，然后 SetHoldRegs/GetHoldRegs。**关键：上层也不调 EnableRobot()** —— 工具口 485 不需要使能机械臂，正好满足用户"绝不要驱动机械臂"。
③ 新写了 **`test_485_all.py`**（33KB，已上传到工作区）：A 链路/协议、B 只读寄存器全扫、C 可写寄存器逐个、D 功能路径（上层启动流程/状态机/两端落点/通信超时保压/清错重使能）、E 破坏性项（0x0007/端点/手动标定模式）、F 收尾自动标定恢复；三后端 `--mode dobot|serial|dryrun`；**只读写 485，不下发任何运动指令**；默认不调 EnableRobot；`--read-only` 只读第一步。
④ 本机验证：`--mode serial` 对真板 **63/63**；`--mode dobot` 对假 dashboard（mock_dobot_server）**57/57**；`--read-only` 21/21。过程中又改了对脚本自己的四个 bug：0x0109 是只写寄存器（读会异常，不是功能缺失）、`wait_still` 返回的是 x100 我按 x1000 比、`x or 999` 把 0 当假值、收尾快照写在 `close()` 之后。
⑤ 其余改动：`mock_dobot_server.py` 补上上层会调的 `ModbusClose`；README 加了§7 描述新脚本与三层注意点（越疆网关一次最多 4 个寄存器、原始帧用例不透传、工具口 485 不需要使能机械臂）。
⑥ 上机环境已验证：py_compile OK、dryrun 退出码 0。**等用户接好线就开始跑。**
当前：机械臂侧未动过任何东西（未使能、未发运动、未改工具口设置）；台架板已失能、全闭位、端点 0/1595。
下一步：用户接好 8 芯航插（485+24V）后，先 `python3 test_485_all.py --read-only`，再跑全量。
下一步：用户接好线后：先 `python3 test_485_all.py --read-only`（确认链路+标定），再 `python3 test_485_all.py` 全量。

## 2026-09-13
完成：按用户要求做了一遍**全命令 / 全功能**测试，并写了可反复跑的套件 `tmp/test_all.py`（**61/61 全过**）。
① `test_all.py` 覆盖：A 协议层（FC03 单/多/大块、FC06、FC10、原子性、三种异常码、坏 CRC、广播、从站地址）；B 只读寄存器全扫（含 0x0060~0x0088 诊断区与坏帧快照区）；C 可写寄存器逐个写+回读（含边界与拒绝路径）；D 功能（安全使能、状态机 0→2→1/3、通信超时保压、清错重使能、电机寄存器直读）；E 破坏性（`0x0007` 设零点、端点 `0x000B/0x000C`、手动标定模式 `0x0030~0x0032`）；F 收尾自动跑一次自动标定把端点恢复到 0/~1595 并复验。
② 首跑 58/61，三个失败查清后**全是测试预期/脚本问题，不是固件问题**：(a) 达妙电机对**任意**寄存器号都会回一帧（未知的返 0），“读未知寄存器应超时”的预期本就错；(b) 状态机用例没先回全闭位、起点已在张开端所以采不到 state=2；(c) **`Serial(timeout=0.5)` 下 `read(256)` 会阻塞满 0.5s** → 每次读 500ms，100ms 量级的通信超时永远清不掉，整个套件也慢百倍、线圈热到 54℃。把 `ser.timeout` 改成 0.02 后，**61/61** 且快很多。
③ 剩下的专项套件也全跑：主机测试 **56 PASS / 0 FAIL**；`rs485_perf` 正常（一问一答 96 次/秒；板子侧 1.65ms/16%）；`travel_soak` **30/30**（26.8s、峰力矩 0.48N·m、温度 36/40℃ 不变）。
④ 链路自检首跑 29/31 —— 背靠背三帧“收 0 字节”但“副作用全部生效”。用 `tmp/burst_probe.py` 复现：**空闲后第一次突发会丢数据**（只收 10/24 字节、丢首个回包头与末个回包、首字节 89ms），紧接的 5 次都是 24/24、3/3 全对（固件 ORE=0）→ 是 **USB-485 适配器(CH343)的冷启动伪影**。把该用例改成“先热身一次 + 最多重试 2 次取最好”后，三连跑 **31/31、31/31、31/31**。
⑤ 记录：`PROJECT.md` 新增「一键全量回归」清单（7 步）与两条坑（CH343 冷启动丢数据、pyserial read(n) 阻塞）；`SESSIONS.md` 本条。
当前：已失能、全闭位、err=0、温度 36/40℃。
未完成（重新整理）：① **断 CAN 自愈需重跑**（要用户把电机 24V 断一下，本轮未验）；② MB_BUF_SZ=320B→连发上限 40 帧（`rs485_perf` 实测 N=45/60 会丢，但真实上层不连发）；③ 自动标定/方案 A 多台件复验；④ ADC 欠压/过压保护（软硬件唯一缺口）；⑤ LED 行为无法远程验。
下一步：请用户断一下电机 24V 把断 CAN 自愈重跑一遍；或接 Dobot 真机联调。
下一步：请用户断电机 24V 重跑断 CAN 自愈；或接 Dobot 真机联调。

## 2026-09-13
完成：按用户要求做“故意写错零点 → 看自动标定能不能找回来”的检验，过程中又提一个真 bug。
① 写了 `tmp/autocal_falsify.py`：安全使能 → `0x0103=500` 走到行程中位 → 失能 → `0x0007`（把中位当零点）→ 验证“坏了”（`0x0103=0` 停在中位）→ 写 `0x0034=1` → 验证“修好了”。
② 结果：自动标定**完全找回来了** —— `0x0035=2`、`0x0036=1595`、端点回到 0/1595，`0x0103=0` 又停在全闭（state 3，T −1.10 = 闭合夹持）、1000 → +1.62（**T 仅 +0.06N·m**）。即“现场换件只需写 0x0034=1”已端到端验证。
③ **测试中逮到真 bug**：写 `0x0007` 后 `pos_abs` 立刻漂了 **−0.79**（不是 0）。根因：`damiao.c` 的回绕补偿用函数静态量 `pos_prev`，零点一变、电机上报的绝对值整体平移，下一帧算出“−旧位置”的假增量灌进累积器，且不会自纠。修为：把 `pos_prev/pos_prev_valid` 提到文件作用域 + 新增 `Damiao_ResetPosAccum()`，并**在 `Damiao_SetZero()` 内部自动调用**（防止以后自己发 0xFE 绕过）。重烧后重跑：`0x0007` 后 `pos_abs=+0.0000`，且测试能正确显示“损坏”（`0x0103=0` 停在中位 0.00）。
④ 回归：485 链路自检 **31/31**；行程耐久——首次跑发现 **`travel_soak.py` 自己的 bug**：它也是“状态一到 1/3 就 break”，而新标定下 `0x0103=0` 落全闭位时状态就是 3 → 20 轮仅 6.4s、实际一步没走（回读停在全闭位 33）。改成“等位置真的停住”后重跑：**20 轮 17.6s、0.81~0.88s/行程、峰力矩 0.50N·m、温度 33/38℃ 不变、无跑飞无故障**。
⑤ 文档：`转接板功能说明.md` §3.2 补 `0x0034~0x0036`、§3.11.1 拆成一键/分步两节（含内部步骤、失败码、两个量测坑），`软件方案.md` §8/§9 同步。固件 text 30584 → 32368。
当前：已失能、全闭位（pos_abs +0.04）、err=0、温度 35/38℃。
未完成：MB_BUF_SZ=320B→连发上限 40 帧、自动标定多台件复验、ADC 欠压/过压保护。
下一步：接 Dobot 真机 + ag_gripper.py 联调；换件后现场只需写 `0x0034=1`。
下一步：接 Dobot 真机 + ag_gripper.py 联调；或把 MB_BUF_SZ 连发上限改大。

## 2026-09-13
完成：按用户要求把“标定”做成了**固件里的一条命令**（之前“一条命令”只是 PC 侧 `tmp/cal_zero.py`，固件里没有）。
① 新增 `main.c` 的非阻塞状态机 `bridge_autocal_tick()` + `bridge_autocal_start()`（跑在主循环，期间 Modbus 照常应答、IWDG 照喂）：新寄存器 **`0x0034` 写1启动/写0清结果、`0x0035` 状态、`0x0036` 结果**；`0x0000/0x0100/0x0030` 加了“标定中拒绝”守卫；text 30584→32356。
② 迭代过程（三次修正，都靠实测打回）：(a) 首版把 `t_rotor` 当温度基线，但启动时电机失能、t_rotor 还是 0 → 一使能真实 32℃ 被当成“升了 32℃”直接失败(0x13)；改为**使能后等 150ms 再取基线** + 补 70℃ 绝对上限。(b) 首版用“位置不动 400ms”当停转判据，两端各被压进硬挡 ≈0.038rad → 行程多算 0.075rad（1693 vs 真实 1610），导致 `0x0103=1000` 顶 1.23N·m；用 `tmp/springback.py` / `springback2.py` / `endpoints_probe.py` 量清楚：**回弹是瞬时的（<52ms）、大小正好等于传动柔性 0.038rad**，于是改为**失能等回弹、用回弹后的 0x51 当自由位**。(c) 但改完仍不准（1687）—— 发现**机构有 ≈0.04rad 迟滞**（设完零点时校验确实是 0，顶几轮后自由全闭位漂到 +0.041），于是加 **`AC_TRIM` 闭环**：先按一次测量写 p_open，再真的走一次 `0x0103=1000` 量稳态力矩，>0.15N·m 就往里收 0.03 重试（最多 4 次）。
③ 实测（`tmp/autocal.py`）：`0x0103=1000` 稳态力矩 **1.23 → 0.08N·m**，`p_open=1.597`；三次重复 **1597/1595/1597**；复验 0→全闭(state3, −1.2N·m 夹持)、500→+0.84、1000→+1.64(T+0.08)。
④ 异常路径实测（`tmp/autocal_fail.py`）：标定中写 `0x0100=165` → **被拒绝**且标定照常跑完；标定中写 `0x0100=0` → `0x0035=0x17`（被外部打断）且 **NVM 端点一字未动**。
⑤ 同步 `docs/转接板功能说明.md`：§3.2 寄存器表补 `0x0034/0x0035/0x0036`，§3.11.1 拆成【3.11.1.1 一键自动标定（推荐，含内部步骤/失败码/保护）】+【3.11.1.2 分步做】两节并写下两个量测坑；变更记录加行。`docs/软件方案.md` §8 标定项与 §9 第 4 项同步。
当前：已失能、全闭位、err=0、温度 36/38℃。
未完成：MB_BUF_SZ=320B→连发上限 40 帧、方案 A / 自动标定多台件复验、ADC 欠压/过压保护。
下一步：接 Dobot 真机联调（`ag_gripper.py`）；换件时现场只需 `0x0034=1`。
下一步：接 Dobot 真机 + ag_gripper.py 联调；换件后现场只需写 0x0034=1 自动标定。

## 2026-09-13
完成：回答“现在标定是啥流程”，并把两份已与代码对不上的文档改齐（项目规矩：文件与代码冲突时以代码为准、同一遍改掉）。
① `docs/转接板功能说明.md`：§3.11 重写成【3.11.1 主流程（方案 A：全闭→`0x0007` 设电机零点→量张开端→写 NVM `0x000B/0x000C`+`0x000D`；换电机/换夹爪必须重做，同台重上电什么都不用做）+ 3.11.2 兜底流程（`0x0030~0x0033` 手推记两个角，不改电机零点，换件后必须重标）】；§5 待办“标定需人工一次”改成“只在装配/换件时一次”；§6 检查清单标定项改成指向 3.11.1；性能表“全行程 4 rad 用时”改成实测 **1.59rad / 0.85s（4rad/s）、1.2s（2rad/s）**；变更记录加 2026-09-13 行。
② `docs/上层对接与上机部署.md`：§7 “代码已就绪，**待烧录验证**” → “已烧录并逐项实测（§9 第 1~7 项全部通过）”；§8 检查清单勾掉已验项（端点方向目视、静默保压 `0x001A=1` 不失能、故障自愈），新增“换件后重标”。
③ 确认一件易踩的事：本轮改的「`0x51` 只在失能时才覆盖 `pos_abs`」**不影响标定模式**（`0x0030` 进标定模式会强制失能，50Hz 读 0x51 照常跟位置）。
④ 重申：上层 `ag_gripper.py` **从不标定**（只写 165，然后用 `0x0103` 0..1000，不做回零/找限位）→ 板子插上机械臂之前必须已经是对的。
当前：固件未改（仍为 CAN 检测那一版）；板子已失能、全闭位、err=0。
未完成：MB_BUF_SZ=320B→连发上限 40 帧、方案 A 多台件复验、ADC 欠压/过压保护（软硬件唯一缺口）。
下一步：接 Dobot 真机联调（`ag_gripper.py`），或补 ADC 保护。
下一步：接 Dobot 真机 + ag_gripper.py 联调；或先补 ADC 欠压/过压保护（需先确认硬件有没有把分压接到 MCU 引脚上）。

## 2026-09-13
完成：**验证并补齐了断 CAN 自愈 —— 过程中发现原验收方式根本做不出来。**
① 用户断开电机 24V 后按计划跑“断 CAN 自愈”：使能后 `en=1 / err=0 / state=3` 一路正常，**没有 Bus-Off**。用 `stm32-dev.py read` 直读地址发现：`PSR=0x77B`（LEC=3 ACK 错误 / ACT=3 发送中 / **EP=1 error-passive** / **BO=0**）、`ECR` TEC=128、TXBRP=7（3 个请求挂着不发）、TXFQS 满、TXBTO=0、`hfdcan1.ErrorCode=512=HAL_FDCAN_ERROR_FIFO_FULL`、`err=0`、`g_dis_err_cnt=0` —— 两次读完全不变。
② 结论（CAN 规范）：接收方不存在 → 每帧 ACK 错误 → TEC +8 涨到 128 进 error-passive；而 **ISO 11898-1 规定 error-passive 的发送方再遇 ACK 错误不再增加 TEC** → **永远到不了 Bus-Off（需 TEC>255）**。所以**“拔 CANH/CANL”这类断路根本触不发 Bus-Off 中断**，固件会自以为健康而实际一帧都发不出去。能触发 Bus-Off 的是总线被拉死（短路/TXD 卡 dominant/收发器故障）。
③ 按用户选择补检测：`main.c` 的 TIM6 节拍里判 `FDCAN1->PSR` 的 **EP 位持续 >500ms** → `err=0x0F` + `Damiao_Disable()`（与 Bus-Off 同一出口），新加计数寄存器 **`0x001B`**；总线恢复后 128 帧内 TEC 排空 EP 自清，不会误判。重编（text 30584）重烧。
④ 实测：使能后 **0.53s** 判出 → `err=0x0F` / `state=31`(0x10+0x0F) / 自动失能 / `0x0200` 返 0 / `0x001B`+1；用户恢复 24V 后写 `0x0100=165` → `err` 被电机反馈帧刷回 `0x01`、`0x0200` 返 1、计数不再涨、`0x0103` 1000↔0 行程正常（1.16/1.21s，T +0.07/−0.25N·m）。**端到端自愈闭环**。脚本 `tmp/can_selfheal.py` + `tmp/can_selfheal2.py`。
⑤ 同步 `docs/软件方案.md`：§5.4 新增 error-passive 检测条目与原理、寄存器表补 `0x001B`、§9 第 7 项改成已验并写明“触发方式要改”、§8 改成第 1~7 项全部实测。技能新增 **坑 #53** 与元教训（读寄存器前先查 CMSIS `_Pos/_Msk` —— 本次把 `CCCR=0x1000` 误读成 INIT=1，实际 bit12 是 PXHD），技能 3.11.0→3.12.0、坑 52→53，selftest PASS。
当前：已失能、全闭位、err=0、温度 31/32℃。
未完成：MB_BUF_SZ=320B→连发上限 40 帧、方案 A 多台件复验、ADC 欠压/过压保护（软硬件唯一缺口）。
下一步：以验收清单 1~7 已全部实测，可进入上层联调（Dobot 真机 + ag_gripper）或补 ADC 保护。

## 2026-09-13
完成：①把改完的固件跑完整回归：**485 链路自检 31/31**（连跑三次都 31/31）、**行程耐久 30/30 轮**（25.2s、峰力矩 0.84N·m、温度 33/38℃ 不变、无跑飞无故障）。
②回归中提两个真 bug 并修好：
 (a) **状态机卡在 2**（`tmp/travel_soak.py` 首轮 6s 未停稳；`tmp/state_probe.py` 测得 pos_abs 稳定 +1.6200 而 p_des=+1.5900，perr=−0.0300 落在 0.02 阈值外，每拍刷新 `g_arrive_ms`）。根因是 **坐标混用**：反馈帧 POS 是 p_m、`0x51` 是 xout，两者差的是传动柔性（实满全开位 p_m+1.5923 / xout+1.6230 = 0.031rad），而 `damiao.c` 收到 0x51 应答就把 `pos_abs` 覆盖成 xout → 带电读一次 0x51 就永久偏一个柔量。已修为**只在失能时**才用 xout 初始化 pos_abs；`main.c` 到位判据改成“**位置真的停住了**（50ms 窗口变化 ≤0.01rad）+ 200ms 去抖”，另加“刚下目标 2s 内误差大且没使上劲→算运动中”的**有上限**豁免。
 (b) **`v_des=0` 导致电机不走**：实测 `0x0103=0` 回读 0、`pos_abs` 一步不动、T 仅 +0.06 —— 是因为复位后 `0x0107/0x0104 = 0`，模式 4 下不给速度就不走。写 `0x0107=2` 后立刻走完整行程（1.59→0.00，1.2s，T 仅 0.04~0.10N·m），状态 2→1→3 全对。已记入文档与坑。
③顺手把三个测试脚本的假故障修了：`rs485_link_check.py` 的 `0x0020` 不再写死旧电机 ID（改成断言 1~16 + 新增 `--motor-id`）、`burst()` 改成“知道该收多少字节就等够再收手”（CH343 分包导致的短读假故障）、`travel_soak.py` 对单次读超时（None）容错（曾把读超时当成“跑飞计数变了”假中止）；并修了 `verify_alias.py` 里写死的期望角 1.65（改成从 0x000B/0x000C 回读算）。
④同步 `docs/软件方案.md` §5.4 与 §6：状态机判据表改成新实现（并写明“判据里刻意不出现绝对位置误差阈值”及原因）、新增“`0x51` 只在失能时初始化 pos_abs”、“复位后 v_des=0 只写位置不走”。
当前：已失能、全闭位（pos_abs +0.0300）、err=0、温度 33/37℃。
未完成：断 CAN 自愈（需拔 CANH/CANL）、MB_BUF_SZ=320B→连发上限 40 帧、方案 A 多台件复验。
下一步：请用户拔 CANH/CANL 验断 CAN 自愈（0x0200 返 0 → 写 165 清错重使能）。

## 2026-09-13
完成：①按用户要求把「向用户提问式排障」写进了 stm32-dev 技能：`SKILL.md` 铁律新增第 6 条「把只有用户的眼睛能定的事实交给人」（五条：先摆出看得见+数字对得上的安全动作、一次只问一件、选项带后果、必须给“看不清”出口、答案连数字落盘；反向纪律：能用探针测的别问用户）；`PITFALLS.md` 新增第 50 条（拿本次「−1.54 是张开 vs −1.11 是闭合位」互相矛盾导致误改代码的真实案例）；版本 3.9.1→3.10.0 + CHANGELOG；同步改了附录表/描述/目录树里的坑数（49→50）。验证：`stm32-dev.py selftest` → **SELFTEST: PASS**（21 项）；坑编号 49→50 连续、①~⑨ 小节标题完好。
②按用户选择给张开端留余量：`tmp/trim_open.py COM21 30` 试了 1.65/1.63/1.61/1.59/1.57 五个候选，测 `0x0103=1000` 停住处的稳态力矩 → **1.24 / 1.07 / 0.40 / 0.02 / 0.00 N·m**；关键是 `0x51` 在 1.65 与 1.59 下分别停 +1.6613/+1.6230，那 0.038rad 是弹性变形而非行程 → 取 **1.59** 行程一点没少、却不再顶（用户目视确认“还是完全张开、看不出区别”）。
③改 `damiao.c` 默认 `p_open=1.59f`（注释里写了余量的来由），重编重烧+reset；写 NVM `0x000B=0 / 0x000C=1590 / 0x000D=1`。
④复验 `tmp/verify_alias.py COM21 30`：0→+0.0000（T−0.51=夹持正常）；500→+0.8000（期望+0.795）；1000→+1.6200 / 0x51 +1.6230 / **T 仅+0.09N·m**；回0→+0.0000。回读 0x0103/0x0202 一致。已失能、温度 33/38℃。
⑤同步 `docs/软件方案.md` §5.3 / §9 第 4 项的端点与复验数字（1.650/1650 → 1.590/1590）。
未完成：断 CAN 自愈（需拔 CANH/CANL）、MB_BUF_SZ=320B→连发上限 40 帧、方案 A 多台件复验。
下一步：请用户拔 CANH/CANL 验断 CAN 自愈（0x0200 返 0 → 写 165 清错重使能）。

## 2026-09-13
完成：同步了挂了很久的文档欠账 `docs/软件方案.md`（read 工具逐节回读验证）：
① §5.1 换成产品板实测：1kHz 组帧+提交 624cyc=3.67µs；**485→CAN 延迟 354~1353µs(均~850)** = 347µs 静默等待 + 0~1000µs TIM6 相位；主机侧 ~10.5ms 中 84% 是 USB-485/主机。
② §5.4「使能不猛冲」补上两个前提：一旦给过位置命令（0x0002/0x0103），再使能会直接跑向旧目标（实测瞬间冲 0.72rad）；复位后 pos_abs=0（失能无反馈帧），照它使能会冲向 0。写了安全顺序：0x0040=0x51 → 写 0x0002 → 再 0x0100=165。
③ §5.3 新增行程端点常数：p_closed=0.000 / p_open=+1.650（方案 A，电机零点=闭合机械基准），NVM 0/1650，编译期在 damiao.c；正=张开与上层同向。
④ §7.1 改掉已不成立的“回第一帧时后两帧已在缓冲区”，写成“回包前先等总线静默（MB_GAP_CYC=59000≈0.35ms@170MHz）”。
⑤ §8/§9 把上机实测项标成已完成第 1~6 项，只注明断 CAN 自愈未做，并新增“方案 A 未在多台件上复验”。
未完成：只剩下断 CAN 自愈（需拔 CANH/CANL）和 MB_BUF_SZ 上限。
下一步：请用户拔 CANH/CANL 验断 CAN 自愈（0x0200 返 0 → 写 165 清错重使能）。

## 2026-09-13
完成：**查清并修好了“标定不对”的真因，方案 A 完成并复验。**
① 零运动探针排除两个怀疑：`tmp/pos_integrity.py`（失能静止下连读 24 个电机寄存器，pos_abs d=+0.0000 ×24）→ 无应答帧污染；`tmp/coord_check.py` k=0.952≈1 → 无坐标混用。`tmp/motor_coord.py` 直读 Gr=7.0（就是当初“7.3×”误判的来源）、PMAX=12.566、CTRL_MODE=4；0x50 p_m 与 0x51 xout 差 0.039rad = 传动柔性。
② 方向目视坐实：`tmp/watch_direction.py COM21 0.15` 正走 0.1155rad（力矩仅 0.03~0.05N·m），用户看到钳口**张开** → **正=张开、负=闭合**；推翻了更早“−1.54 那端是张开”那句话。
③ 两端目视确认无卡点：`tmp/go_open_watch.py` 走到 +0.03（张开硬挡，力50% → 0.26N·m 平台）用户说“完全张开、没有余量”；走到 −1.58（闭合硬挡，力30% → 1.26N·m 平台）用户说“完全闭合、没有余量”。中间段全程 0.02~0.07N·m。行程 = −1.60…+0.04（xout），宽 1.64rad。**结论：机构没有卡点，真因是默认端点跨 4.0rad 而真实只 1.65rad。**
④ 执行方案 A（`tmp/cal_zero.py`）：在闭合硬挡执行 `0x0007=1` 把电机零点定到机械基准（0x51 读回 0.0）；量得张开端 = +1.65；写 `0x000B=0 / 0x000C=1650 / 0x000D=1` 存板子 NVM；改 `damiao.c` 编译期默认 `p_closed=0.0f / p_open=1.65f`，重编（text 30260→30376）重烧并校验通过。
⑤ 复验 `tmp/verify_alias.py COM21 30`：0x0103 0→pos_abs +0.0000/0x51 +0.0000；500→+0.8400（期望+0.825）；1000→+1.6600/0x51 +1.6613；回0→+0.0000；回读 0x0103/0x0202 一致。方向与量程全对。
⑥ 顺手捉到两个新坑：**烧完 flash 后核被留在 halt，485 完全不应答，要再跑一次 `stm32-dev.py reset`**；**复位后 pos_abs 是 0（失能时没有反馈帧），直接使能会冲向 0**。
未完成：①断 CAN 自愈（需拔 CANH/CANL）；②软件方案.md §7.1/§5.1 待同步；③MB_BUF_SZ=320B→连发上限 40 帧。
下一步：让用户拔 CANH/CANL 验断 CAN 自愈（0x0200 返 0 → 写 165 清错重使能）；然后同步软件方案.md。
未完成：断 CAN 自愈未验；软件方案.md §7.1/§5.1 未同步；MB_BUF_SZ 上限未改。
下一步：请用户拔掉 CANH/CANL，跑 485 读 0x0200 观察是否变 0 并在重插后自愈。

## 2026-09-13
完成：①把「可达范围绝对位置在不同会话间漂 ~1.1rad」当成第一嫌疑人来查，写了两个**零运动**探针：(a) `tmp/pos_integrity.py` —— 电机失能静止下连读 24 个电机寄存器（0x0A/0x0B/0x0C/0x0D/0x15/0x16/0x17/0x00），每次之后回读 `0x0010`：**pos_abs 一次都没跳（d=+0.0000 ×24）**，纯读 0x0010 三十次最大偏差 0.0000；使能保持 8s 期间 pos_abs 稳定（-0.98…-0.97），力矩 -0.05~0.02。→ **排除“电机应答帧被当反馈帧解析、污染 pos_abs”这个假设**（原以为 0x33 判据 + 100ms 窗口有洞）。(b) `tmp/coord_check.py COM21 -1.0` —— 读 0x51 建基准 → 命令 p_des=o1−1.0 → 先读 0x0010 再读 0x51：k=(pos_abs增量)/(输出增量)=**0.952≈1**，坐标一致。
②`tmp/motor_coord.py COM21 5` 直读电机参数：CTRL_MODE(0x0A)=**4**、Gr(0x14)=**7.0**、PMAX(0x15)=12.566、KP_APR=54、KI_APR=0、OV_Value=32V；0x50 p_m=−0.9770 vs 0x51 xout=−1.0155（差 0.039rad）→ 反馈帧 POS ≈ p_m、0x51 = xout，两者之差是**传动柔性**不是坐标混用；Gr=7.0 正是之前那次误判“7.3× 坐标混用”的来源，已作废。
③对照手册 `docs/电机资料/_extracted.txt`：反馈帧 D0=ID|ERR<<4、D1/D2=POS、D3..=VEL/T/T_MOS/T_Rotor；读寄存器应答是 MST_ID + CANID_L/H + **0x33** + RID + 4 字节数据；0x50=80 p_m、0x51=81 xout、0x14=20 Gr（寄存器表确认）。
未完成：①方向待肉眼最终确认（历史两条口头结论矛盾：−1.54=张开 / −1.11=闭合，两者只差 0.43rad 而行程 1.65rad，不可能同时成立）；②默认端点方向反了（`damiao.c:59-60` p_closed=−2.0/p_open=+2.0 → 0x0103=0 去张开、1000 去闭合，与上层 0=closed 相反）——待方向确认后再改；③断 CAN 自愈需拔 CANH/CANL；④方案 A 卡在真实行程；⑤软件方案.md §7.1/§5.1 待同步；⑥MB_BUF_SZ=320B→连发上限 40 帧。
下一步：用户看一眼当前失能停在 −1.01rad 的钳口，回答“现在是张开还是闭合/中间”，以及“张开最大到底在负端还是正端”；确认后：按真实方向改 `p_closed/p_open`（负=张开则 p_closed=+0.05、p_open=−1.60，跨度可为负，插值公式 `p_closed + (p_open−p_closed)*v/1000` 本身支持），再用 `0x0103` 0↔1000 在可达范围内复验方向。

## 2026-09-13
完成：①按用户建议把“停车力矩阈值”从 0.8 调到 2.2 N·m、力从 20 调到 60（i_lim 0.6，最多可出 ~2.5 N·m），并给 `sweep_limits.py` 加了参数化阈值 + 线圈温升>5℃ 保护。②结果：**负方向**平滑走到 **−1.600 rad**，力矩从 0.84 **一路爬到 −2.21** 才被阈值停住（渐进负载）；**正方向**同样条件只走到 **+0.050 rad**，而力矩 **稳在 +0.84 不动、位置一步不走**（连续 3 个采样）→ **力矩“平台” = 顶到实体/卡点，不是力不够**。③因此“调大阈值”**并没换来更多行程**（已验证无效），而可达范围确定为 **−1.60…+0.05 rad（1.65rad）**；结合“被人手强推后轴到过 +1.09~+1.16”与用户目视“钳口还没到最大张开”→ 判定**正方向 +0.05~+0.14 有卡点，过了它机构还能走 ~1rad**。④全程线圈温度 33/38℃ 未动（温升保护未触发），结束后已失能、停在 −0.77rad。
未完成：①等用户在“查卡点”与“按可达范围标定收工”之间选；②方向仍未确认（正=闭合/负=张开）；③“手推行程不用”指代不明；④方案 A 被机构卡点阻塞；⑤断 CAN 自愈需拔 CANH/CANL；⑥MB_BUF_SZ=320B→连发上限 40 帧；⑦软件方案.md §7.1/§5.1 待同步。
下一步：用户选“按可达范围标定”→ 开到 +0.05（闭合端？待方向确认）设零点 → p_closed=0 / p_open=1.65 → 存 NVM → 改 damiao.c:59-60 → 在可达范围内复验；用户选“查卡点”→ 先停，不动机构。

## 2026-09-13
完成：①按用户要求用“当前量程”跑 `0x0103` 0↔1000（`tmp/goto_endpoints.py`，力 20/速度 100，力矩突变立即停）：`0x0103=0` 从 0.000 平滑走到 **−1.59 rad** 被挡停（与之前力 80 测到的位置**完全一致**），随后两条腿因为“还在顶着（力矩 0.83）”而立即中止；②又在**安全区**跑了来回演示 `0x0103` 250↔750（`tmp/roundtrip_demo.py`）：→250 能正常到位（−1.03），→750 两次都在 **+0.03 rad** 被挡停（力矩 0.71/0.73）→ 确认**当前可达范围 = −1.59…+0.03 rad（宽 1.62rad）**。③把三次测量的“挡”位置摆在一起：+0.14/−1.59、−0.46/+1.16、+0.03/−1.59 → **宽度稳定（1.62~1.73）但绝对位置漂了 ~1rad**，而同一会话内两次 +0.03 精确复现（不是噪声）→ 推断**电机→夹爪的连接打滑/空程**，已在 PROJECT 里标为当前硬阻塞，并请用户查联轴。④全程温度 41~45/38℃（反复顶着的余温，已回落），结束后已失能。
未完成：①等用户查“电机轴↔夹爪”的连接是否打滑，并回答“哪边是闭合”（方向仍未确认）；②方案 A 被机构阻塞（p_open 常数定不了）；③用户说“手推行程不用”（指代不明，需时再确认）；④断 CAN 自愈需拔 CANH/CANL；⑤MB_BUF_SZ=320B→连发上限 40 帧；⑥软件方案.md §7.1/§5.1 待同步。
下一步：用户回联轴情况后：若确认打滑→先修连接（防松胶/夹紧套/加键）再谈标定；若确认不打滑→当场用两个“挡”的位置一起标（闭合端设零点 + p_closed=0 / p_open=行程角）并用 0x0103 在可达范围内复验。

## 2026-09-13
完成：①按用户要求把标定改成“平滑连续慢走 + 高频监测力矩、突变立即停车并松力”（新脚本 `tmp/sweep_limits.py`：块读 0x0010..0x0012 一次拿 pos/vel/力矩，~50Hz）。②扫出的结果**反而证伪了之前的限位**：起始位置已经是 **+1.09 rad**（在之前认定的“闭合端 +0.14”**之外**）而那里只要 ~0.2 N·m；负向在 **−0.46** 力矩突变到 −0.81、正向在 **+1.16** 突变到 +0.80 就停了 → **阻力点不是固定机械限位，会随相位变**，所以之前所有行程/限位数（−1.54/+0.07/1.61、−1.59/+0.14/1.73）**全部作废**。③补写了上一条声称“已写好”但实际没写的 `tmp/hand_travel.py`（失能状态下手推钳口、只读 0x51，用于拿真实行程 + 判断传动是否打滑），已语法检查。④当前状态：电机失能、位置 +1.17rad、err=0、温度 33/37℃。
未完成：①等用户说“开始”跑 20s 手推行程（`tmp/hand_travel.py`）；②方向仍待确认（正=闭合/负=张开待手推时目视确认）；③方案 A 未实现（p_open 常数要等真实行程）；④断 CAN 自愈需拔 CANH/CANL（换电机后正好可对比）；⑤MB_BUF_SZ=320B→连发上限 40 帧；⑥软件方案.md §7.1/§5.1 待同步。
下一步：用户说“开始”：跑 `tmp/hand_travel.py COM21 20`，他在这 20 秒里手推一个来回；拿 min/max 定真实行程与方向：若 1:1 跟随→按可达行程标定（闭合端设零点 + p_closed=0 / p_open=行程角）；若打滑→先修传动。

## 2026-09-13
完成：①质疑驱动验证：用“命令越过硬挡 + 逐级加大力档”判定 −1.6rad 处是机械到头还是负载顶住 —— 力 0.4→1.0 位置只从 −1.60 挪到 −1.65（0.05rad），而力矩冲到 3.4 N·m、**线圈温度 5 秒 +19℃（32→51℃）、最后 err=10 过流保护停机** → 结论：**马达输出端在这里是硬性受阻**，不是力不够；所以标定算法不是问题。②善后与确认：写 `0x0009=1` 清过流 → err 回 0、温度自行回落到 34/37℃、跑 `tmp/health_check.py` 小范围往返均能跟随（力矩 0.02~0.04）→ **电机无损**。③意外发现一个安全坑：使能瞬间夹爪自己从 −1.56 冲到 −0.84（0.72rad）—— `bridge_on_enable()` 只在 `g_p_des_valid==0` 时才保持当前位置，一旦给过位置命令就会跑向旧目标（已写进坑）。④已把“真实行程 +0.14…−1.65 vs 默认端点±2.0”这个安全边界记进 PROJECT。
未完成：①等用户回答机构问题：夹爪型号、失能后能否用手把钳口掰得更开、离最大张开还差多少 —— 决定是“按可达范围标定收工”还是“机构先排查”；②方案 A 未执行；③断 CAN 自愈需拔 CANH/CANL；④软件方案.md §7.1/§5.1 待同步；⑤MB_BUF_SZ=320B→连发上限 40 帧。
下一步：用户答机构问题后：若确认“已达最大张开”→ 按可达硬挡标定（闭合端 +0.14 设零点 → p_closed=0 / p_open=−1.73 → 存 NVM → 改默认端点）；若确认机构还有行程但马达到不了 → 先查连杆/传动（空程/打滑/别住）。在标定完成前，**不要对这只机构跑任何写 0x0103 的脚本**。

## 2026-09-13
完成：①用户回答“−1.54 那端是**张开**” → **方向确定：负端=张开、正端=闭合**（0x0103=0 应映到 +0.14rad，1000 映到 −1.59rad）。②用户同时质疑“那不是限位的地方” → 查证后确认**我上一版找限位用的力档太小**（0x0101=20/i_lim 0.2，机构被力不够卡在半路），改用上层抓取档 **力 80/i_lim 0.8** 重测：两端都出现**力矩硬顶**（−2.94 / +3.37 N·m）→ **真实限位 −1.59 / +0.14 rad，行程 1.73 rad（≈99°）**；旧数 −1.54/+0.07/1.61rad 作废。验证方式：重写 `tmp/find_limits.py`（可传力档/步进，以力矩硬顶为判据）逐步回读 0x0010/0x0012/0x0013/0x0014。
未完成：①一个小疑点待确认：用户目视觉得 −1.54（≈硬顶 −1.59）时“张开还有余量”，而实测只差 0.05~0.07rad → 是真限位但视觉余量少，还是连杆空程/别住？②方案 A 的第一步（`0x0007` 改电机零点）已向用户征求批准，尚未回答；③断 CAN 自愈需拔 CANH/CANL；④`cal_bench.py` 的两处判据/力档仍是错的（已在 PROJECT 里标注弃用）；⑤MB_BUF_SZ=320B→连发上限 40 帧；⑥软件方案.md §7.1/§5.1 待同步。
下一步：用户批准后：开到闭合端(+0.14) → `0x0007=1` 设零点 → 确认张开位读数≈−1.73 → 写 0x000B=0/0x000C=−1730/0x000D=1 存 NVM → 改 `damiao.c:59-60` 默认端点为 0/−1.73 → `0x0103` 0↔1000 复验方向与行程。

## 2026-09-13
完成：①查清“电机不应答”的真因：用户换的不是夹爪而是**整只电机**（新 ESC_ID=3/MST_ID=19，旧的 1/17），而 `Damiao_AutoDetect()` 只在开机跑一次 → 旧 ID 在说话；**复位后自动识别成功且新 ID 已存 NVM**。判别关键：FDCAN `PSR=0x708`(LEC=0/ACT=1/**BO=0**)、`ECR=0` → 总线零错误、不是 Bus-Off（新电机的收发器会正常 ACK，ACK 与验收滤波无关），但参数读全 timeout、`g_rx_fb_cnt` 3 秒不涨 → **err=9 是旧电机的残留值**（我上一轮据此推的“当前欠压”是错的，已更正到坑）。②新机构找限位：**−1.54 / +0.07 rad，真实行程 1.61 rad（≈92°）**；起始位 −0.74 恰在中间，所以“近端=闭合”判据失效 → 方向待用户肉眼确认；已把夹爪停在 −1.54 等回答。验证方式：can_reg_read.py 读 0x0A(=4)/0x51(=−1.53858) + 485 只读 + FDCAN 寄存器 + 计数器两次采样。
未完成：①等用户回答“−1.54 那端是闭合还是张开”；②方案 A 未实现（流程已排好，见 PROJECT 现状）；③断 CAN 自愈需拔 CANH/CANL（现在换电机后正好可旧电机/新电机对比）；④`cal_bench.py` 的“行程角”与“近端=闭合”两处判据要修；⑤MB_BUF_SZ=320B→连发上限 40 帧；⑥软件方案.md §7.1/§5.1 待同步。
下一步：用户回答方向后：开到闭合端 → `0x0007=1` 设零点 → 写 0x000B/0x000C(±1610)/0x000D=1 存 NVM → 改 `damiao.c:59-60` 默认端点为 0/±1.61 → `0x0103` 0↔1000 复验。

## 2026-09-13
完成：①用户把电机接到夹爪上，并确认**当前位（−1.11 rad）就是闭合位** → 之前“6.25 rad 推不出机械限位”的结论作废（那次机构未装上）；②随即发现**电机报 `err=9` 欠压**：写 `0x0009=1` 清故障后**立刻又变回 9** → 是当前态而非锁存；同时电机寄存器读（0x0040→0x0044）全 timeout、使能被拒（en=0）、但 CAN 反馈帧照来、温度 33/40℃ 正常；查手册确认“电源电压不低于 15V”（`0x00 UV_Value` 范围 (10.0,fmax]）→ 判定 24V 低于门限，已在 `tmp/fault_check.py` 里只清故障不使能（避免强行使能再故障），并请用户查供电。③确认 `pos_abs` 已被故障态反馈帧刷成真实 −1.11rad → 供电恢复后可直接安全使能不会冲。验证方式：485 只读 0x0015/0x0201/0x0010/0x0013/0x0014 + 电机寄存器读 + 手册对照。
未完成：①等用户查 24V（≥15V / J_motor 插头 / 电源限流）——这是当前硬阻塞，方案 A 的“设机械基准 + 量行程角”全部卡在它上；②供电恢复后的顺序已定：确认能读位置 → `cal_bench.py` 找张开限位定行程角 → `0x0007` 设零点(闭合位) → 写 0x000B/0x000C/存 NVM → `0x0103` 0↔1000 复验方向；③方向验收/断 CAN 自愈仍未做；④MB_BUF_SZ=320B→连发上限 40 帧；⑤软件方案.md §7.1/§5.1 待同步。
下一步：用户修好 24V 后：先 `tmp/mb_read.py COM21 0x0015` 确认 err 回到 0/1，再跑 `tmp/cal_bench.py COM21` 定出闭合/张开限位与行程角，然后按方案 A 设零点+写端点+存 NVM 并用 0x0103 0↔1000 复验方向。

## 2026-09-13
完成：①澄清了“标定≠改电机零点”：标定流程 0x0030~0x0032 只把两个角度数写进**板子** NVM（0x0031 只是 `p_closed = pos_abs`），改电机零点的是另一个寄存器 0x0007（`Damiao_SetZero()`→系统命令 0xFE），标定流程从不调它 → 所以“换一只已标定的夹爪就能用”只在零点被拉到同一机械基准时才成立。②用户选定**方案 A**（零点=机械基准，端点变编译期常数）。③为实现 A 去量真实行程：新增 `tmp/find_limits.py`，实测从 +1.99 推到 −4.25 再回 +2.00 rad（**6.25 rad**）**全程没找到机械限位**、力矩仅 0.02~0.07 N·m → 判断**电机当前未啮合夹爪钳口机构**，行程角因此定不了。验证方式：485 逐步回读 0x0010/0x0012/0x0013/0x0014/0x0015。
未完成：①用户确认“电机上到底有没有装夹爪机构”——没装则行程角/方向验收/方案 A 都要等；②方案 A 未实现（框架：p_closed/p_open 改编译期常数 + 一键设基准流程）；③断 CAN 自愈需拔 CANH/CANL；④MB_BUF_SZ=320B→一次连发上限 40 帧；⑤软件方案.md §7.1 已不成立、§5.1 延迟数字待同步。
下一步：用户确认机构状态后：若装了 → 继续往两头推限位（注意位置字段 ±4π≈±12.57rad 回绕）定出行程角，再实现方案 A；若没装 → 先做 A 中不依赖行程角的框架部分。

## 2026-09-13
完成：①修 IWDG 未冻结（main.c 加 `__HAL_DBGMCU_FREEZE_IWDG()`）并验证：故意 `--keep-halted` 停 5s → `g_bb.boot_count` 4→4 不变（修复前会 +1 复位），从此调试器 halt 不再把板子复位、读数可信；②耐久 20 轮全行程（`tmp/travel_soak.py`，31.1s）：峰值力矩 0.11 N·m、温度 33→40℃、跑飞计数 0、err=1、状态机全程 1、结束停在 0x0103=1000；③带电机复跑 485 链路自检 **31/31**（改过 modbus.c 静默判据与 main.c 后 的回归），并修正脚本里过时的 0x0015 断言（原写“期望 15 无电机 Bus-Off”，电机接着时 err=1）。验证方式：上述脚本 + 技能 read/RTT/blackbox。
未完成：①方向验收需肉眼：0x0103=0 时夹爪是否物理闭合（对齐上层 0.0=closed）；②断 CAN 自愈需拔 CANH/CANL（验 0x0200 返 0 → 写 165 清错重使能）；③行程标定从未做过（NVM 空→默认端点 ±2.0rad，两端未顶硬限位，真实行程可能更大）；④MB_BUF_SZ=320B→一次连发上限 40 帧；⑤软件方案.md §7.1 “回第一帧时后两帧已在缓冲区”的说明已不成立，§5.1 延迟数字需同步。
下一步：用户确认物理方向后：若方向反了就按 0x0030/0x0031(记闭合位为端点0)/0x0032 重标定；然后让用户拔 CAN 线验自愈；最后按需同步软件方案.md。

## 2026-09-13
完成：行程与保压实测（本轮新做的）：安全使能（先 0x0040=0x51 读输出轴位置 1.03rad，再写 0x0100=165）后位置 Δ=0 不冲；小步 758→708、0x0103 0↔1000 往返全部到位（回读 0/1000，差 ≤0.01rad，用时 0.49~1.50s）；力20/速度200 下峰值力矩仅 0.09 N·m（TMAX=5）、温度 33/39℃ 全程不变、跑飞计数 0、状态机全程 1。另外验证通信超时：静默 >1s 后 0x001A=1 而 0x0201 仍=1（不失能、位置不变）。验证方式：tmp/travel_sweep.py 逐步回读 0x0202/0x0200/0x0012/0x0013/0x0014/0x0017 + 485 只读。
未完成：①方向验收需肉眼：0x0103=0 时夹爪是否物理闭合（对齐上层 0.0=closed）；②断 CAN 自愈需拔 CANH/CANL（验 0x0200 返 0 → 写 165 清错重使能）；③行程标定从未做过（NVM 空→默认端点 ±2.0rad，两端未顶硬限位，真实行程可能更大）；④IWDG 未冻结（GDB halt >1s 就复位板子）；⑤MB_BUF_SZ=320B→一次连发上限 40 帧。
下一步：用户确认物理方向后：若方向反了就按 0x0030/0x0031(记闭合位为端点0)/0x0032 重新标定；然后让用户拔 CAN 线验自愈。

## 2026-09-13
完成：真板 485+CAN 全链路打通并实测：①修好 TCAN1042 VIO(5脚接3.3V) 后 CAN 立刻可用——自动识别 ESC_ID=1/MST_ID=17、err=0、有反馈帧（此前一直 err=0x0F Bus-Off、故障灯亮、扫不到电机）；②485 链路自检 31/31（rs485_link_check.py，含背靠背三帧/41寄存器大块读/50轮soak）；③模拟真实上层时序 10/10；④延迟实测首字节 10.46ms，分解为 空口1.30 + 固件静默0.35 + USB适配器/主机8.81（84%），一问一答 96 次/秒。验证方式：上述脚本 + 技能 read/RTT/blackbox + FDCAN_TEST/PSR/ECR 寄存器实证。
未完成：①IWDG 未冻结导致调试 halt >1s 就复位板子（可作为 1 行修复：__HAL_DBGMCU_FREEZE_IWDG()）；②nRF 无；②MB_BUF_SZ=320B → 一次连发上限 40 帧，超限会被「超长帧重同步」丢掉大部分（真实上层不连发，暂无影响）；③bridge_read 里那批 (int16_t)(x*100.0f) 也是截断（实测取值域内精确，未动）；④软件方案.md §7.1 关于「回第一帧时后两帧已在缓冲区」的说明已不成立，需同步。
下一步：经用户确认夹爪行程无人/无工件后，跑软件方案 §9 的实机项：写 0x0100=165 使能 → 轮询 0x0200 → 0x0101=20/0x0104=500 → 0x0103 0↔1000 往返 → **方向验收（0=闭合、1000=张开）** → 拔 485 线 10s 保压 → 断 CAN 自愈。注意这些会真的开合夹爪，且 rs485_link_check.py 不能盲跑（内含使能+位置用例）。

## 2026-09-12
Done: 初始化 memory/ 三件套并实测验证工程可跑：.work/host_test 下 run_tests.py = 56 PASS / 0 FAIL（exit 0）；interface-adapter/ 下 make = EXIT 0（text 30260 / data 56 / bss 4488）；确认 CubeCLT 1.22.0 的 arm-none-eabi-gcc、make 与 SEGGER JLink V974 均在位。同会话早先审查了 PCB：勘定 PCB/Netlist_Schematic1_2026-09-07.tel 与 BOM_Board1_PCB1_2026-09-07.xlsx 位号/值不一致，网表不可作准。
Open: 固件审查报告 MH-1/MH-2 未处理；原理图 U6(TCAN1042GV) 5 脚仍标 NC、未接 VIO；未从当前原理图重导网表做一致性核对。
Next: 在原理图把 U6.5 由 NC 改为 VIO 并接 VCC3V3（就近 100nF），随后重导网表与 BOM/PCB 做一致性检查。
