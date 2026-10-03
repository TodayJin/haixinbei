# 车上位机控制台 · 前端（Vite + React + TypeScript + Tailwind + uPlot）

这是「本地 Web 控制台」的前端。成品由 Python 桥（纯标准库 + pyserial）在同源下托管，
浏览器只连 `http://127.0.0.1:8770`；**前端所有请求都用相对路径 `/api/...`**，不写死主机端口。

- 后端契约（冻结版）：[`../docs/API.md`](../docs/API.md) —— 页面字段、端点、SSE 事件全按它来。
- 后端实现（Python 桥）：`../bridge/`（由主工程维护，本目录不碰）。
- 本目录只负责**界面**：10 个页面 + 数据层 + 假后端（mock）。

---

## 1. 开发

~~~bash
cd tools/console/web
npm install            # 只装 react/react-dom/uplot/lucide-react + 构建期依赖
npm run mock           # 另开一个终端：零依赖假后端（127.0.0.1:8770）
npm run dev            # 5173，/api 自动代理到 8770
~~~

`npm run dev` 是给改代码用的（HMR）。想看**成品**（跟桥托管的是同一份）：

~~~bash
npm run build          # tsc --noEmit && vite build -> dist/
node mock/server.mjs   # 直接开 http://127.0.0.1:8770 ，托管 dist/
~~~

**真车 / 仿真的成品界面不用 node**：双击 `tools/console/` 下的
`启动上位机.bat`（真车，自动找串口 @230400）、`启动上位机-仿真.bat`（无车）、
`启动上位机-列串口.bat`（只看串口在哪）。桥自己托管 `dist/` 并打开浏览器。

## 2. 命令一览

| 命令 | 作用 |
|---|---|
| `npm run dev` | Vite 开发服务器（5173，`/api` 代理到 8770） |
| `npm run build` | 类型检查 + 生产构建 → `dist/`（`tsc --noEmit` 不过就不出包） |
| `npm run preview` | Vite 自带的静态预览（**不代理 `/api`**，看版式用） |
| `npm run mock` | 启动零依赖假后端（托管 `dist/`，端口 `PORT` 可改） |
| `npm test` / `npx vitest run` | 单元测试 + SSR 冒烟 + 契约自检 |

## 3. 页面（10 个，哈希路由 `#/<id>`）

| 路由 | 页面 | 内容 |
|---|---|---|
| `#/overview` | 总览 | 链路状态/帧率/busy、关键量（偏航/俯仰横滚/轮速/电池电流/模式/主循环/PWM/动作+NVRAM）、三条迷你趋势、8 路循迹阵列、状态标志 |
| `#/drive` | 操控 | **键盘 WASD 驾驶**（W/S 前进后退、A/D 左右转、方向键同义、可组合、A+D 或 W+S 抵消、空格急停）、速度滑块（**速度刻度 0..7200**，不是 mm/s；刻度 1000 以下是静摩擦死区，可开「跳过死区」从可动区起步）+ 转向**角速度**滑块（°/s，0..180）、每 100ms 积分成目标航向偏移（最多 ±540°）后下发 `{mode:"base_turn", base, turn}`（`turn` 单位是度、正 = 左转；**倒车 `base<0` 时角速度先取反**，这样 S+D 才是"车尾往右摆"）、实时推算左右轮与反馈；只在指令变化时下发一次，靠桥的 200ms 保活喂车上的 500ms 看门狗；**顶部常驻安全提示** |
| `#/imu` | IMU | **手写 CSS 3D 车模型**（零依赖、无模型文件：拖动转视角、双击回默认视角，配偏航/俯仰/横滚三个读数与罗盘）+ **偏航/俯仰/横滚三张波形**（各 20s 窗口、纵轴以 0 为中心、卡内 最小/最大/当前）+ **「朝向控制」一个 −180°..+180° 滑块（实时下发、松手保持、回到 0°、交还车上任务）** + 角度环内部量（目标角/误差/期望角速度/修正）。3D 与读数都用**遥测原始帧**（+偏航 = 左转、+俯仰 = 抬头、+横滚 = 向左倾），不套用固件给 OLED 画图时的轴序交换 |
| `#/sensors` | 传感器 | 8 路灰度柱（压线高亮）、重心/偏差/PID 输出、四路趋势、标定表（黑/白/阈值/抖动/极性/质量） |
| `#/servo` | 舵机与动作 | 两路舵机手动角度 + 回中 + 中位偏差；**14 个动作的逐行编辑**（下发 / 播放 / 与出厂值对比 / 复合动作标记）；「固化到车」（写小车 Flash）；预设 |
| `#/params` | 调参 | 按环分组（速度环/角度环/循迹环/串级/任务/车辆/电源）逐项编辑、范围条 + 默认位置刻线、只看改动、全部下发、固化到车、回默认 |
| `#/tasks` | 任务 | **状态机拖拽编程**：左任务列表（新建/复制/删除/导入导出 JSON，浏览器 localStorage 存 `hxb.tasks.v1`）、中状态卡片（每张「进门」+「循环体」两个序列，块拖动排序 / 点开改参数 / 设为主页 / 复制 / 删除）+ 编译与烧写卡（长度·占用%·CRC16·状态块数、红错/黄警列表、**「开机自启」开关（下发 COMMIT 的 flags bit0）**、烧写后**回读逐字节比对**、开始/停车/500ms 运行监视高亮当前状态与块）、右块库（10 种块按 运动/等待/动作/流程 分组，拖进去或点一下加到当前状态）。格式真值是仓根 [任务编程](../../../docs/任务编程.md)；编译器 `src/lib/task.ts`、编辑器 `src/lib/taskedit.ts`、桥客户端 `src/lib/taskapi.ts`（车上能力位 bit7 = `HOST_CAP_TASK` 为 0 时切成「等固件支持」提示） |
| `#/scope` | 波形 | uPlot **每通道独立量程泳道**、5/10/20/30s 窗口、暂停、游标读数、通道勾选、清空 |
| `#/control` | 控制与日志 | 停车（也停车上正在跑的任务）、跳任务编排页、清故障、软复位、遥测频率；日志（可过滤）与 ACK 列表；车辆状态 —— 「启动任务 A/B/C」按钮已删除（车上内置流程已不存在，固件对 `0x02..0x04` 回 `0x08` 不支持），「陀螺校准」按钮同样是删掉的（`gyro_cal` 桥没这个名字、固件 `0x05` 也没实现） |
| `#/settings` | 设置 | 串口/波特率选择、连模拟器/串口/断开、环形缓冲点数、波形窗口、安全提示开关、固件能力位、预设管理与导出、关于 |

全局：侧栏导航 + 顶栏链路徽章；右下角 toast（写操作的成功/失败提示，**失败直接显示后端的 `error` 原文**）；
`Esc` = 急停（停车 + 交还控制权）。

## 4. 数据层

- `src/lib/api.ts`：所有端点（相对路径），失败抛 `ApiError{code,status}`；`openStream()` 用
  `EventSource('/api/stream')`，事件名 `state/telem/stats/log/ack/actions/params/cal`，
  `error` 事件里"有 data 才是后端错误，否则是连接断开"。
- `src/lib/store.ts`：单例发布订阅 + `useSyncExternalStore`。热路径（`telem` 50Hz）走
  `emitHot()` 100ms 节流；冷状态（params/actions/presets/logs）即时刷新。环形缓冲
  `SeriesRing`（默认 30000 点，设置页可改并 `resize` 保留最近数据）。
- `src/lib/channels.ts`：18 个波形通道的定义（键/标签/单位/颜色/取值），新增通道只改这里。
- 设置持久化在 `localStorage['console.settings.v1']`（窗口、缓冲点数、勾选通道、操控页步进、安全提示开关）。
- 任务页的数据层是**另一条线**（不进 `store`，页面自己 `useState` + `localStorage['hxb.tasks.v1']`）：
  `task.ts`（镜像编解码）= 与车上同一份格式；`taskedit.ts` = 拖拽/增删/脏数据修复/编译预览；
  `taskapi.ts` = `/api/tasks*` 的 HTTP 客户端（每文件各带一份 `.test.ts`）。车上存储：**页 58 = 目录**
  （最多 8 条记录，每条 `id/off/len/crc16/flags/name`）、**页 59..61 = 程序区 6 KB**（`0x0801D800` 起，
  单条任务 ≤ 4096 B）；编译卡上的「占用 %」就是按 6144 B 算的。烧写栏的**「开机自启」开关**会把
  COMMIT 帧的 `flags` bit0 置上，车下次上电自己跑这个任务（`TASK_STORE_FLAG_AUTO`）。
  「等固件/桥支持」提示态现在是按**能力位 bit7（`HOST_CAP_TASK`）**切的，不是按端点 404 切的。

## 5. 假后端 mock（零依赖）

`node mock/server.mjs`（只 `node:http`，默认 `127.0.0.1:8770`，`PORT=9000` 可改）：
按契约实现**全部**端点 + SSE（连上先发 `state`，随后 `telem`（按 telem_hz）、1Hz `stats`、`log`/`ack`/`params`/`actions`），
并托管 `../dist`。数据是合成的：yaw 漂移、pitch/roll 抖动、8 路灰度扫过一条"线"、电压缓降、电流随 PWM、
偶发丢线；动作表用固件真实的 14 个动作角度；参数表用 `firmware/App/Svc/host_link.c` 的 `SPECS[]` 真默认值
（另加两个"串级"标定量 `angle_per_unit`/`angle_max`）。

写操作会真的改内存状态并回 `ack`，越界/未知名返回 `4xx + {ok:false,error,code}` —— 前端就把这句话显示出来。

任务端点（`/api/tasks*`，契约见 `docs/API.md` §3.6；真桥见 `tools/console/bridge/server.py`）也实现了，
用的是内存里的**假 Flash**：`put` 收最多 4096 B 分片（页面按 120 B 一片发）、`commit` 查 magic/版本/状态数
并且 **CRC 不对就拒收**（`flags` bit0 存下来、列表里回给前端）、`get` 回整镜像、`run` 之后 `status` 每 400 ms
走一块（走完序列换下一个状态）——所以 `#/tasks` 页不用等固件就能整条演示（烧写 → 目录 → 回读 → 运行 → 停车）。

## 6. 自测

~~~bash
npm test                 # 19 个测试文件
~~~

| 文件 | 覆盖 |
|---|---|
| `src/lib/ring.test.ts` | 环形缓冲：窗口切片、绕回、resize 保留、NaN 归零 |
| `src/lib/format.test.ts` | 单位/格式换算：PWM%、脉宽→角度、角度归一、灰度、时长 |
| `src/lib/render.test.tsx` | **10 个页面 SSR 冒烟**：能渲染、含关键中文、不出现 `NaN`/`undefined` |
| `src/lib/contract.test.ts` | 对 mock 的**契约自检**：字段形状、分组、越界错误体、14 个动作抽查、SSE 先 `state` 后 `telem`、`/api/tasks*` 整条闭环（烧写→目录→回读逐字节→运行→错 CRC 被拦→删掉；mock 没起就 skip） |
| `src/lib/ssrPreview.test.tsx` | **产物生成器**：喂真实结构数据后把 10 个页面渲染成 `out/console/ssr/*.html` |
| `src/lib/task.test.ts` | 任务镜像的编解码/校验/CRC（与 C 侧同格式） |
| `src/lib/task.golden.test.ts` | **跨语言金样本**：`encodeTask` 编出来的字节逐字节等于 C 侧 `task_prog.c` 生成的 `GOLDEN_HEX`（208 B / CRC16 `0x023A`），再解码往返 |
| `src/lib/taskedit.test.ts` | 编辑器纯逻辑：块默认值/描述、拖拽插入与右移补偿、跨状态移动、状态增删克隆与跳转改道、按 UTF-8 字节截名、脏数据修复、存盘往返、编译预览（含红错/黄警） |
| `src/lib/taskapi.test.ts` | 桥客户端：hex 编解码、120 B 分片拼镜像、`/api/tasks*` 各端点载荷、404/`不支持` ⇒ 提示态而不是报错、桥不在/非 JSON 的报错文案 |
| `src/lib/perf.test.ts` | **热路径设计钉死**（用户说「感觉卡」的那次）：200 帧遥测不叫醒 `useMeta()`、rAF 合并、meta 快照同一引用、单通道取数 |
| `src/lib/sparkline.test.ts` | 迷你波形：`rawRange`/`nextRange` 的**纵轴滞回**（放大即时、收缩每 tick 2%、掉到 55% 以下贴合）与 `decimationStep` 死区；对照旧实现证明旧代码每 tick 都在换相位（IMU 曲线"闪"的根源） |
| `src/lib/hotpath.test.tsx` | **量测夹具**：喂 20s×100Hz 真遥测后 SSR 渲染 10 个页面，打印每页 `ms(中位)/折线点/取快照/热tick/元取数/拷贝点/扫描点/角上取数`（不是断言，是给人看的账本） |

契约自检与预览生成需要 mock 在跑：

~~~bash
node mock/server.mjs &
npx vitest run src/lib/contract.test.ts src/lib/ssrPreview.test.tsx
~~~

## 7. 截图（成品回归）

~~~bash
node mock/server.mjs &          # 先起假后端（它托管 dist/）
mkdir -p out/console
"C:\Program Files\Google\Chrome\Application\chrome.exe" --headless=new --disable-gpu \
  --hide-scrollbars --window-size=1680,1050 --virtual-time-budget=8000 \
  --screenshot="$PWD/out/console/overview.png" "http://127.0.0.1:8770/#/overview"
# 其余页面把 #/overview 换成 #/drive #/imu #/sensors #/servo #/params #/scope #/control #/settings
~~~

> ⚠️ **在写这个前端的沙箱里跑不了浏览器**：Chrome 151 `--headless`/`--headless=new`/`--headless=old`
> 一律 `exit=21`；加 `--single-process --no-zygote` 变成 `exit=0` 但**不产出 png**（独立
> `--user-data-dir`、绝对/相对 `--screenshot` 路径、`--log-file` 都试过，无日志无文件）；
> Edge `--headless=new` 直接挂死。所以那一轮**没有像素截图**，替代证据是：
> ①`render.test.tsx` 的 10 页 SSR 冒烟；②`ssrPreview.test.tsx` 生成的
> `out/console/ssr/<页面>.html`（内联真实 CSS + 真实数据结构）——**用一个能跑的浏览器打开这些
> html 就是成品长相**，也可以直接拿来截图。

## 8. 约定 / 边界

- **中文界面**、等宽数字（`.num`）；深色玻璃拟态卡片、青/紫霓虹强调色、150ms 过渡。
- 依赖白名单：运行期 `react`/`react-dom`/`uplot`/`lucide-react`；构建期 `vite`/`@vitejs/plugin-react`/
  `typescript`/`tailwindcss`/`postcss`/`autoprefixer`/`vitest`。**没引任何 UI 库/图表库**（波形是 uPlot，
  其余图形是 SVG/自绘）。
- 所有写操作的失败提示都直接显示后端 `error` **原文**，不做二次翻译。
- 驾驶/舵机/动作这些会真的动车的按钮不额外弹确认框（调试要顺），但操控页常驻安全提示，
  且桥侧有 0.5s 链路看门狗：上位机一断，车自己停。操控页还有一层本地兜底：窗口失焦、切页、
  松开所有键都立刻交还 `{mode:"off"}`，不会"按过 W 就一直跑"。
- 比赛规则不允许运行中的遥控干预，这套只用于调试、标定与赛前检查。
