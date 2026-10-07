# 上位机（tools/console）后端 API 契约 —— 冻结版 v1

> 前端（`tools/console/web/`）与 Python 桥（`tools/console/bridge/`）之间**唯一的约定**。
> 双方都不得单方面改动；要改先改本文件并在提交信息里写明。
> 数据方向：小车 ──(LPUART1 921600, 帧协议)──> 桥 ──(HTTP + SSE, localhost)──> 浏览器。

## 0. 运行形态

- 一键启动（双击，不用手敲命令）：`tools/console/启动上位机.bat`（真车：自动找串口，921600）、
  `启动上位机-仿真.bat`（内置仿真车，无车也能开）、`启动上位机-列串口.bat`（只看车在哪个 COM）；
  逻辑都在 `tools/console/launch.ps1`（手工用：`pwsh -File tools\console\launch.ps1 -Sim -NoBrowser`）。
- 桥只依赖 **Python 3.11+ 标准库 + pyserial**；手工跑：`cd tools/console && python -m bridge.main --serial COM26`
  （或 `--sim`）。注意 `--port` 是**桥的 HTTP 端口**（默认 8770），串口设备用 `--serial`。
- 静态前端：桥直接托管 `tools/console/web/dist/`（`GET /` → `index.html`，其余按文件返回）。
  开发期可用 `npm run dev`（Vite 5173）并把 `/api` 代理到桥的 8770 端口。
- 默认端口 **8770**，只监听 `127.0.0.1`。
- 两种链路模式：`serial`（真车，串口）/ `sim`（内置仿真信号源，无车可用）。

## 1. 通用

- 所有请求/响应 `Content-Type: application/json; charset=utf-8`（SSE 除外）。
- 错误统一：HTTP 4xx/5xx + `{"ok":false,"error":"人话错误","code":"bad_param"}`。
- 成功写操作统一：`{"ok":true, ...}`，通常回带受影响对象的新状态。
- 数值单位一律显式：`_mv` 毫伏、`_ma` 毫安、`_us` 微秒、`_mmps` mm/s、`_ms` 毫秒、角度 `_deg`（度）、`pwm` 为固件 PWM 刻度（±7200）。

## 2. 端点一览

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/api/health` | 版本/运行时长/是否仿真 |
| GET | `/api/link` | 链路状态 + 固件握手信息 |
| POST | `/api/link/connect` | `{mode,port?,baud?}` 连接（切换模式也用它；`mode` 只有 `serial`（要 `port`）/ `sim`） |
| POST | `/api/link/disconnect` | 断开 |
| GET | `/api/ports` | 可用串口列表 |
| GET | `/api/params` | 全部参数（本地值 + 默认/范围 + 脏标记） |
| POST | `/api/params/set` | `{id\|key, value}` 改一条（本地缓存，不自动下发） |
| POST | `/api/params/push` | `{ids?, keys?}` 下发到小车（省略 = 全部脏的） |
| POST | `/api/params/save` | 让小车把当前参数写进 Flash（掉电保存） |
| POST | `/api/params/reload` | 从车上全表读回（覆盖本地） |
| POST | `/api/params/defaults` | `{ids?}` 恢复默认（只改本地，等 push） |
| GET | `/api/actions` | 20 个舵机动作姿态（前 14 项是出厂角度，15..20 是空位） |
| POST | `/api/actions/set` | `{n, a1, a2}` 改一个动作（下发到小车 RAM，舵机立刻到位） |
| POST | `/api/actions/save` | 让小车把动作表写进 Flash |
| POST | `/api/actions/play` | `{n}` 完整播放一个动作（含复合动作的中间步骤与延时） |
| POST | `/api/servo` | `{ch:1\|2, angle:0..180}` 直接摆一路舵机 |
| POST | `/api/servo/center` | 两路回 0°（收回位） |
| POST | `/api/servo/deviation` | `{ch, dev_us}` 中位偏差微调（±100µs） |
| POST | `/api/drive` | `{mode:"off"\|"wheels"\|"base_turn", vl?,vr?,base?,turn?}`：`wheels` 的 `vl`/`vr` = 左右轮**速度刻度**（−7200..+7200，负 = 反转）；`base_turn` 的 **`base` = 基准速度刻度（−7200..+7200，负 = 倒车）、`turn` = 目标航向偏移（度，正 = 左转 = 偏航角增大，±540）**，基准航向在**进入**该模式时锁存（= 固件 `s_base_yaw`），之后重复帧只改目标；`base=0` 即"原地用车上的角度环转到并保持这个朝向"。**注意：这里的速度是固件速度环的目标刻度、不是 mm/s**（mm/s 只有遥测的 `vl/vr` 用；刻度 1000 以下落在静摩擦死区） |
| POST | `/api/cmd` | `{cmd, arg?}`：命令名只有桥上 `bridge/state.py:CMD_NAMES` 那张表 —— `stop` / `clear_fault` / `reset` / `set_telem_hz` / `save_params` / `calib_gyro`(固件回未就绪) / `set_logging`(同上) / `set_telem_mode` / `start_a`·`start_b`·`start_c`（这三个仍收，但固件删了内置任务流程、回 **`0x08` 不支持** ⇒ 桥回 `200 + ok:false`；任务走 §3.6）。**没有 `telem_hz` 这个短名**（2026-10-04 用户报的「没有这个命令: telem_hz」就是网页发了短名）。**`arg` 是 32 位**：桥发 `cmd`(u8) + 4 字节小端 i32，少于 5 字节的载荷固件会当成 `arg=0`（2026-10-04 修的真 bug：老代码只发 1 字节 ⇒ `set_telem_hz` 被钳成 1Hz）。`set_telem_hz` 固件钳 **1..200**，桥的 `/api/link` 报同一个钳过的值 |
| GET | `/api/stream` | **SSE**：实时数据流（见 §4） |
| GET | `/api/log?limit=200` | 最近的文本日志 |
| GET | `/api/cal` | 灰度标定数据（8 路黑白阈值/抖动/有效性） |
| GET | `/api/presets` | 本机预设列表 |
| POST | `/api/presets` | `{name, targets?:["params","actions","servo"], keys?, values?, note?}` 保存（`values` 省略 = 抓当前；`kind` 由 targets 推） |
| POST | `/api/presets/apply` | `{name, targets:["actions","params","servo"]}` 应用 |
| DELETE | `/api/presets?name=x` | 删除 |

## 3. 数据模型

### 3.1 `/api/link`
```json
{
  "ok": true,
  "mode": "sim",                  // sim | serial
  "port": "COM27", "baud": 921600,    // 代码默认 921600 = 板上固件 board.c 的 DL_BAUD（2026-10-05 定档）
  "connected": true,
  "stats": {"rx": 0, "tx": 0, "crc": 0, "resync": 0, "drops": 0},
  "telem_hz": 100,
  "last_rx_ms": 1234,             // 距上次收到小车帧的毫秒数
  "rtt_ms": 1.2,                  // 测得的往返时延（无则 null）
  "fw": {                          // 未握手时 null
    "name": "quandi-car", "version": "0.0.2", "proto": 2,
    "param_crc": 41321, "telem_hz": 50, "caps": 127
  }
}
```
`caps` 位（与固件一致）：bit0 TELEM2、bit1 动作表、bit2 舵机、bit3 驾驶、bit4 标定、bit5 Flash 保存、bit6 看门狗、**bit7 任务程序**（`HOST_CAP_TASK`）。

### 3.2 `/api/params`
```json
{"ok":true,"param_crc":41321,"source":"fw",   // fw=车上读过 | local=未读，用内置默认
 "items":[
  {"id":513,"key":"line_kp","label":"循迹 Kp","group":"循迹环","unit":"",
   "value":2700,"default":2700,"min":0,"max":20000,"decimals":1,"step":10,
   "dirty":true,"note":"循迹外环比例"}
 ]}
```
`group` 取值固定为：`速度环` `角度环` `循迹环` `串级` `机械/车辆` `电源` `任务`。

### 3.3 `/api/actions`
```json
{"ok":true,"items":[
  {"n":1,"name":"收回","a1":0,"a2":0,"preset_a1":0,"preset_a2":0,"dirty":false,"compound":false},
  {"n":5,"name":"台阶2收回","a1":80,"a2":170,"preset_a1":80,"preset_a2":170,"dirty":false,"compound":true,"steps":"135/170 →0.4s→ 90 →0.1s→ 80"}
]}
```
`a1/a2` = 小车上当前 RAM 值；`preset_*` = 本机预设文件里的值（没有则 null）；`dirty` = 两者不同；`compound=true` 的动作 `play` 会走完整过程。

### 3.4 `/api/cal`
```json
{"ok":true,"active_set":0,"quality":8,
 "ch":[{"i":0,"blk":3820,"wht":910,"thr":2365,"jitter":12,"pol":0,"ok":true,"reason":0,
        "level":18,"thr_level":128}, ...8 项]}
```

### 3.5 预设
```json
{"ok":true,"items":[{"name":"默认调试","kind":"mixed","updated_ms":1728000000000,
                     "keys":["actions","params"],"note":""}]}
```
`kind`：`actions` | `params` | `servo` | `mixed`。文件落在 `tools\console/data/presets/*.json`。

### 3.6 任务（`#/tasks` 页用）
格式与语义的真值来源是仓根 `docs/任务编程.md`（二进制 v1、帧 `0x60`–`0x69`、能力位 `HOST_CAP_TASK=bit7`）。
前端 `web/src/lib/taskapi.ts`、Python 桥（`bridge/server.py` 的 `_route_get`/`_route_post`）与假后端都按下面的端点实现。
桥在**能力位缺失**时不回 404/501，而是回 `200 + {"ok":false,"supported":false,"error":"…"}`，前端据此切成「等固件支持」态：

| 方法 | 路径 | 请求体 | 回 |
|---|---|---|---|
| GET | `/api/tasks` | — | `{"ok":true,"items":[{"id","name","len","crc","flags"}]}` |
| GET | `/api/tasks/get?id=N` | — | `{"ok":true,"hex":"…"}`（整镜像，前端自己切分块） |
| POST | `/api/tasks/put` | `{"id","offset","hex"}` | `{"ok":true}`（一帧 `T_TASK_PUT`；**协议上限 124 B = `MAX_PAYLOAD-4`，页面按 120 B 分片**） |
| POST | `/api/tasks/commit` | `{"id","len","crc","name","flags"}` | `{"ok":true}`（`T_TASK_COMMIT`，车上擦写 Flash；`flags` 是**保留位**，车上原样存进目录页、不解释 —— bit0 曾经是「开机自启」，用户 m24999 删了那个功能：上电只扫描不启动） |
| POST | `/api/tasks/delete` | `{"id"}` | `{"ok":true}` |
| POST | `/api/tasks/run` | `{"id"}` | `{"ok":true}` |
| POST | `/api/tasks/stop` | `{}` | `{"ok":true}` |
| GET | `/api/tasks/status` | — | `{"ok":true,"status":{"id","state","running","block","elapsed_ms","flags"}}` |

> **★写车前先跑 `python -m bridge.taskbackup`**（workdir `tools/console`）：**只读**走
> `GET /api/tasks` + 逐槽 `GET /api/tasks/get`，把目录与**镜像字节**抄到仓根 `out/taskbackup/`
> （`<槽>_<名字>_<CRC>.bin` + `index.json`），并逐条校验「长度 / 头 `crc[6..7]` / magic /
> 头 `len[8..9]` / 按 `[12:]` 重算的 body CRC」—— 任一条不合就明确报错、退出码 1。
> 2026-10-07 有两次**参数填错**的 `commit`（`crc` 写成 8..9、`len` 写成 4..5）把车上三条任务
> **从目录里抹掉了** ⇒ **先备份，再写车**。

- 失败体一律 `{"ok":false,"error":"…"}`；**语义性失败（车拒收/没人应答）也是 HTTP 200 + `ok:false`**，只有参数不合法才回 400。`error` 必须是人话，**不许只丢一个状态码**：2026-10-04 用户 m24215 报的「每次操控弹一个后端报错，什么 HTTP002。200」就是 `send_wait` 失败时不补 `error`、前端兜底把 HTTP 状态码当成了错误信息。现在 `bridge/state.py` 的 `send_wait` 会按情形补「车上没应答（等 Ns 超时）/ 车上没回确认帧（收到 N 帧都不是 ACK）/ 车上拒绝了：<name>」并记一条 warn 日志，驾驶被拒还说明下一步（「任务正在跑…先去任务页停掉」）。任务这几个端点额外过一遍 `TASK_STATUS_HINT`
（`bridge/state.py`）：车回 `忙` ⇒ 「先在任务页按停车」、`参数错` ⇒ 「多半是任务名超长或长度/校验对不上」、
`超范围` ⇒ 「换一个 0~7 里的空槽」…… 2026-10-07 之前这里在"链路正常、只是这一帧没回"时写
「多半是固件不带任务功能（HOST_CAP_TASK 未置位）」，被当成结论查错了方向（其实只是丢帧，能力位一直是好的）。
- `crc` 是 `T_TASK_COMMIT` 里那个 CRC16-CCITT（覆盖镜像 `[12,len)`），前端算好一起发；车上回读后前端会再比一次字节（这一层在页面里，桥只发一次 COMMIT）。
- `status` 允许平铺（`{"ok":true,"id":…}`）也允许嵌在 `status` 里，两种都认；`running` 线上是 `u8` 1/0，前端 `taskapi.normalizeStatus()` 归一成布尔，`elapsed_ms` 与 `elapsedMs` 两种拼写都接受。
- **假后端 `web/mock/server.mjs` 已经按本表实现了一遍**（内存里的假 Flash：`put` 收分片、`commit` 查 magic/版本/状态数/**CRC 必须对**、`get` 回整镜像、
  `run`/`stop`/`status` 让监视画面真的动起来），所以 `#/tasks` 页不用等固件就能整条演示。
  `web/src/lib/contract.test.ts` 里有一条用例专门走这条真 HTTP 闭环（烧写 → 目录 → 回读逐字节 → 运行 → 状态 → 错 CRC 必须被拦 → 删掉）；
  Python 侧对应用例在 `tools/console/tests/test_task.py`（帧层 / 仿真车 / 桥端点三层，跑真 HTTP）。
  **注意**：`contract.test.ts` 直接调 `createTaskApi().burn(id,…)` 并自己指定 `id`，不走下面那三条选槽规则
  （规则在页面/store 里，属于"谁来定 `id`"这一层）。

#### 3.6.1 上传时怎么选槽号（`id` 是谁定的）

车上按**槽**存任务（固件 `task_store.h` 的 `TASK_STORE_SLOTS = 8`，槽 0..7）；`TaskStore_Commit(slot,…)`
就是"写进那个槽"—— 往一个占用的槽再提交 = **覆盖它**，目录里不会多出一条副本。所以"再烧一次同一个任务
会不会留副本"，完全看客户端发给 `put`/`commit` 的那个 `id`。桥不做选择，`id` 由页面按下面三条算
（纯函数 `web/src/lib/store.ts` 的 `planTaskSlot()`，输入 = `/api/tasks` 的目录 + 编辑器里的任务名）：

| 情形 | 烧到哪 | 界面提示 |
|---|---|---|
| 车上有**同名**任务 | **它那个槽**（覆盖） | 「车上已经有同名的 #N「名字」—— 这次是覆盖它，不会多出一条副本」 |
| 没有同名 | **第一个空槽**（0..7 里最小的空闲） | 「车上没有同名任务 —— 会烧到第一个空槽 #N」 |
| 8 个槽全满且没有同名 | **不烧**（按钮禁用 + 红字），绝不悄悄覆盖别人的任务 | 「车上 8 个槽（#0~#7）都占着，也没有叫「X」的任务可以覆盖 —— 先在左边「车上现有」里删掉一个不用的，再烧写」 |

- 页面点「烧写到车」时会**先重读一次 `/api/tasks`** 再定槽位（不拿可能过期的目录做决定）；**目录读不到就不烧**
  —— 宁可让人重按一次，也不拿编辑器里的本地编号去覆盖车上别人的任务（那是 2026-10-07 之前的老行为：
  `doBurn()` 直接 `taskApi.burn(task.id,…)`，`task.id` 只是编辑器列表 0..7 的编号）。
- 「开始」跑的是**车上那个槽**（按名字定位），不是编辑器里的编号：名字对不上就直说"车上没有这条任务，先烧写"。
- **任务名在车上最长 30 字节**（固件 `TASK_STORE_NAME_MAX`；桥 `payload_task_commit()` 也按 30 字节切，
  切点可能落在半个汉字中间）。页面因此发送前按**字符边界**截到 30 字节（`fitCarName()`），比同名时按
  "车上真正存下来的那 30 字节"比（`carNameKey()`）—— 否则名字长的任务再烧一次会认不出车上那条是自己、又占一个空槽。
- `GET /api/tasks` 与 `GET /api/tasks/get` 都是**多帧回包**，收尾帧丢一次就整条失败：桥对两者都**整条重发**
  （目录 3 次、回读 3 次，回读还额外有"块不连续就整条重读"2 次），重发幂等（车会重头再 dump 一遍）。
- 页面上任务失败的红字都过一遍 `humanizeTaskError()`，落点是**下一步动作**
  （"再按一次烧写" / "先去停车" / "去设置页连串口" / "先在车上现有里删一个"），不出现纯底层术语；
  原文另存一份 `console.warn`，排查时看浏览器控制台。

## 4. SSE 事件（`GET /api/stream`）

连接后**第一帧**发 `state`（完整快照，见下），之后按事件推进；服务端 15s 没数据发 `: ping`。
每帧格式：`event: <名字>\ndata: <JSON>\n\n`。

| event | 频率 | data |
|---|---|---|
| `state` | 连接时 1 次 | `{"link":…,"params":…,"actions":…,"presets":[…],"cal":…}` |
| `telem` | 小车遥测频率（**车上固件默认 200Hz**：`firmware/App/Svc/host_link.c:1087`；`--sim` 仿真车默认 50Hz） | 见下 |
| `telem_batch` | 同 `telem`，但连续帧按 `--sse-batch-ms` 攒成一条 | `[<telem>, <telem>, …]`（2 帧起） |
| `stats` | 1 Hz | `{"stats":…,"telem_hz":42,"telem_hz_set":137,"telem_tx_hz":7,"loop_us":834,"last_rx_ms":3,"busy_pct":12.3}`。<br>**`telem_hz` 是实测**（1s 窗口数出来的帧率）、`telem_hz_set` 才是车上报的设定值 —— 界面显示帧率一律用实测那个（2026-10-05 修：原来两个都回设定值，"设 200Hz 还显示 100Hz"）。`loop_us`/`busy_pct` 是车上**主循环里界面刷新**的耗时与占 5ms 控制拍的比例，**不是**控制中断占用（控制中断实测 ~9µs） |
| `log` | 事件驱动 | `{"ts_ms":…,"level":"rx"\|"tx"\|"info"\|"warn"\|"error","text":"…"}` |
| `ack` | 事件驱动 | `{"seq":12,"req":"params.set","status":0,"ok":true,"text":"…"}` |
| `actions` | 改动/读回时 | `{"items":[…同 §3.3…]}` |
| `params` | 读回/保存后 | 同 §3.2 |
| `cal` | 读回时 | 同 §3.4 |
| `error` | 事件驱动 | `{"text":"串口被拔出"}` |

**攒批（`--sse-batch-ms`，默认 20ms）**：桥把连续遥测攒够一个窗口再发同一条
`telem_batch`；`0` = 关，退回"一条事件一帧"的老行为。这是**拿延迟换唤醒数**：100/200 Hz 定频源实测 ——
帧率不变（100.4→100.2、199.3→199.8 帧/s），遥测报文数 100Hz 402→135、200Hz 803→134（约 1/3 ~ 1/6），
单条报文 740B→2132B/4260B，字节率基本不变（73.2→72.1 KB/s）；代价是单帧最坏晚
一个窗口（20ms 档实测报文到达间隔 p95 31.9ms / max 32.4ms）。前端 `openStream`
对 `telem_batch` 逐帧喂同一个 `onTelem`，上层无感。

`telem` 字段（**未做的量一律给真实默认值、不假装**；注意 `pitch`/`roll` 是**传感器原始量** —— 车上那颗 JY61P 装成横的，**车身横滚 = `pitch`、车身俯仰 = `roll`**，见下面 IMU 页那节的轴对调）：
```json
{"t":1728000000123,"t_host":1728000000130,
 "yaw":-64.7,"pitch":5.8,"roll":4.7,
 "vl":0,"vr":0,"pwm_l":1200,"pwm_r":-1200,
 "vbat_mv":12010,"ibat_ma":330,
 "servo1_us":1500,"servo2_us":1500,"servo1_deg":90,"servo2_deg":90,
 "line":[0,0,1,1,1,0,0,0],"gray":[12,34,180,220,190,30,10,8],
 "state_id":23,"loop_us":412,"mode":0,"task_id":255,"action":3,
 "trace":{"pos":0.52,"pid":300},
 "angle":{"err":0.12,"rate_ref":0.0,"corr":0.0},
 "flags":{"lost":false,"slope":false,"gyro_ok":true,"motor":false,
          "host_drive":false,"logging":false,"flash":false,"fault":false},
 "nvram":0}
```
- `mode`：0 空闲 / 1 陀螺 / 2 循迹 / 3 串级。
- `state_id`：**任务解释器在跑时的当前状态号**；没在跑（含没载入）= `0xFF`（255，界面上显示为 `--`）。
- `task_id`：**正在跑的任务号**（车上目录页里的槽号）；没在跑 = `0xFF`（255）。判据是解释器**在跑**
  （固件 `TaskRun_IsRunning()`）：程序跑完 `END`、被 `stop` 停掉或出故障之后立刻回 `0xFF`，
  不会因为 RAM 里还留着一份程序就继续报旧号；`0x69` 状态帧另有 `running` 字段可以区分"载入但没跑"。
  键名 2026-10-03 从 `renwu` 改来 ——
  旧语义是"内置比赛流程 0 停 / 1 任务A / 2 任务B / 3 任务C"，那套流程连同 `renwu.c` 已从固件删除，
  这三个命令（`start_a/b/c`）现在回 `0x08` 不支持。
- `nvram`：0 空闲 / 1 待保存 / 2 保存中 / 3 已保存 / 4 失败。
- `gray` 0..255（255=最黑）；`line` 为 0/1。

## 5. 前端要求（给 web/ 的实现约束）

- 技术栈：**Vite + React 18 + TypeScript + Tailwind CSS 3.4 + uPlot**（图标用 `lucide-react`，其余不引入 UI 库）。禁止 Qt/tkinter/electron。
- 语言：界面中文；数字用等宽字体（`tabular-nums`）。
- 设计：深色优先（`#0b1020` 级背景）、**默认低特效主题**（`html.perf-lite`，设置页开关「低特效模式（默认开）」）：**面板一律不给底色** —— 侧栏与卡片 `background-color: transparent`，文字直接落在页面渐变上（页面底 = `#0b1020` + `body` 那三团固定辉光），卡片只留 `ring-1 ring-white/10` 的 1px 亮边分组；顶栏因为 sticky 压在滚动内容上**必须不透明**，但它画的是**同一份 `background-image` + `background-attachment: fixed`**，与 `body` 的辉光逐像素对齐（`out/looklite.cjs` 用"把 header 藏起来再截同一块比像素"钉死：最大通道差 ≤ 2）。**不许把面板刷成任何纯色**：先刷成纯黑被用户 m24215 否掉，再刷成 `#0d1428/#121a33/#141d3d` 三层被用户 m25581 否掉（「跟背景还是融合不到一起啊。你为什么非要有这个文字背景呢？你就单纯一个文字不行吗？」）；关掉开关回到玻璃拟态卡片（`bg-white/5 + backdrop-blur + ring-1 ring-white/10`）+ 状态点脉冲。滚动期间（`html.is-scrolling`，见 `web/src/lib/fx.ts`，松手 0.14s 摘掉）**无论开关如何都临时停掉模糊与脉冲**（每帧重糊 / 每帧全文档样式重算是实测的卡顿根因）。圆角 16px、青/紫霓虹强调色、切换 150ms 过渡；桌面三栏自适应，窄屏折叠侧栏。
- 页面：**总览 / 操控 / IMU / 传感器 / 舵机与动作 / 调参 / 任务 / 波形 / 控制与日志 / 设置**（10 个）。
- 操控页：键盘 **W/S = 前进/后退、A/D = 左/右转**（方向键同义），可组合（W+A = 边走边左转），A+D 或 W+S 相互抵消；空格的优先级最高 = 急停交还车上任务流程。所有键松开 ⇒ `{mode:"off"}`。前端只在指令变化时 POST 一次 `/api/drive`，之后靠桥的 200 ms 驾驶保活喂车上的 500 ms 看门狗。
- 操控页的速度滑块给的是**速度刻度 0..`PWM_MAX`(7200)**（不是 mm/s —— 固件 `SpeedControl_SetTarget()` 吃的就是这个刻度；7200 = 满量程 ≈ 516 mm/s）。刻度 `DEAD_SCALE`(1000) 以下轮上 PWM 不到 3000，实测是静摩擦死区（速度环靠积分顶要好几秒），所以设置里默认开「跳过静摩擦死区」（滑块从 1000 起步，`driveSkipDead`）。
- 键盘给的转向是**角速度**（°/s，滑块 0..`TURN_RATE_LIMIT`=180），前端每 100 ms 把角速度积分成**目标航向偏移**（累计最多 `HOLD_LIMIT`=540°）再下发；松键 = 保持当前朝向（不发 off），A/D 的符号与固件一致：**按 A（左转）发正 `turn`**。**倒车（`base < 0`）时积分前先把角速度取反**：`turn` 是目标航向，而行进方向 = 航向 + 180°，取反后按 D 才是"车尾往右摆"（用户 m20267 报的方向反了就是漏了这一步）。
- IMU 页「朝向控制」：一个 −180°..+180° 滑块（左侧 = 右转、右侧 = 左转），拖动即接管角度环（`{mode:"base_turn", base:0, turn:offset}`，节流 100 ms + 尾部补一帧），松手保持；「回到 0°」归零，「交还车上任务」发 `{mode:"off"}`。读数：基准朝向 / 目标朝向 / 当前朝向 / 角度环误差。
- IMU 页 3D 车模型：**纯 CSS 3D**（`preserve-3d` 叠长方体，不引 three.js，仓库里也没有模型文件），符号约定走**固件 OLED 方框那一套（默认档 M3）**：模型吃**遥测原始帧**、`rotateY(+yaw) rotateX(−roll) rotateZ(+pitch)`（车的左侧 = 模型 +x，车头朝 +z；`lib/attitude3d.ts` 文件头有推导）；读数与波形页同名通道按**车体轴**显示 —— `俯仰 ← 遥测 roll`、`横滚 ← 遥测 pitch`（`web/src/lib/channels.ts`），即 +横滚 = 车向左倾、+俯仰 = 低头。用户 m30822 报的「波形界面的横滚和俯仰是相反的」就是原来这里直接取了同名字段。相机是 `rotateY(az) rotateX(-el)`（默认 az 26° / el 30°，拖动改视角、双击复位）。三张波形 = 偏航/俯仰/横滚各一张（20 s 窗口，纵轴以 0 为中心，用 `Sparkline` 的 `zeroScale`）。
- 不要用 `gyro_cal` / `gyro_zero` / `gyro_turn` 这三个命令名：桥的 `CMD_NAMES` 里没有（只有 `calib_gyro`），固件 `CMD_CALIB_GYRO 0x05` 也没实现（明确回 `ST_NOT_READY`）；只有开发用 mock 认这三个名字。朝向控制的正确做法就是上面的 `/api/drive` `base_turn`。
- 数据：`EventSource('/api/stream')` + 本地环形缓冲（默认 30 000 点，可在设置里改）。断开自动重连并保留缓冲。
- 波形页：每通道**独立量程泳道**（节约纵向空间）、可选 5/10/20/30 s 窗口、暂停/继续、游标读数。
- 每个写操作后给出 1 行反馈（成功/失败原因），失败要用后端返回的 `error` 原文。
- 安全提示常驻：操控页顶部一行“真车模式下会真的让车轮转动 —— 先架空空转”。
- 任务页（`#/tasks`）：三栏 = 左任务列表（新建/复制/删除/导入导出 JSON，localStorage `hxb.tasks.v1`）/ 中状态卡片（每张
  两个序列「进门」「循环体」，块可拖动排序、点开改参数）+ 编译与烧写卡 / 右块库（按 运动/等待/动作/流程 分组，
  **拖进去或点一下**加到当前状态）。**烧写 = 分块 `put` → `commit` → 立刻 `get` 回来逐字节比对**；运行监视 500 ms 轮询
  `status` 并高亮正在跑的状态与块。诊断信息（`elapsed_ms`）是给界面看的经验值，**不要**用它做逻辑判断（固定 5 ms 一拍、1 Hz 心跳）。
  拖拽用手写 HTML5 DnD（`dataTransfer` 里放 `application/x-hxb-task`），不做嵌套拖拽。
- 任务页的编译器是 `web/src/lib/task.ts`（与车上 `firmware/App/Svc/task_prog.c` 同格式，有跨语言金样本对拍：`tests/c/taskgen.c` + `tests/fixtures/task.golden.json` + `web/src/lib/task.golden.ts`）；编辑操作全在 `web/src/lib/taskedit.ts`，**改格式要同时改这两处 + `docs/任务编程.md` + 重新生成金样本（`tools/console/tests/c/make_golden.ps1`）**。
- 还有一条**反向**对拍（`tools/console/tests/test_task_firmware_e2e.py`）：esbuild 打包 `tests/c/taskentry.ts` → node 用**上位机的编码器**编出 7 个镜像 → gcc 把 `tests/c/taskcheck.c` 链上**真** `task_prog.c` 读一遍，断言车上校验 `TASK_OK`、车上算出的 len/CRC/状态名/块字段与上位机一致，并且"缺收尾块"时两边都报错（上位机 `最后一块` / 车上 `TASK_E_TAIL=9`）。
  查这个链路时抓到过一个**格式陷阱**：名字区没有长度也没有 `'\0'`，读名字只能用"下一个更大的偏移"定界（`%s` 会把下一个名字一起读出来）—— 已写进 `docs/任务编程.md` §4 的警告块。

## 6. 后端开发/测试要求

- `python -m pytest tools/console/tests -q` 全绿；协议编解码、参数表、CRC、SSE 分帧、动作表合并、预设读写都要有测试。
- 无车测试：`--sim`（内置信号源，产生逼真的 yaw/pitch/roll/灰度/电压/电流/舵机波形 + 偶发丢线）。
- 有车测试（J-Link 供电、轮子悬空）：**只读**（握手/读参数/读动作表/收遥测），不下发任何让电机或舵机动作的命令。
- 桥必须能在**串口被拔掉/热插拔**时自愈（重连 + `error` 事件），不崩。
