/**
 * 与后端契约 `tools/console/docs/API.md`（冻结版 v1）一一对应的类型。
 * 契约里的字段名/单位不要改；这里只是把它们写成 TS 类型。
 */

/** 链路模式（`replay` 已在 2026-10-04 按用户要求删除，只剩真串口与模拟器） */
export type LinkMode = 'sim' | 'serial'

/** 固件握手信息（未握手时为 null） */
export interface FwInfo {
  name: string
  version: string
  proto: number
  param_crc: number
  telem_hz: number
  caps: number
}

export interface LinkStats {
  rx: number
  tx: number
  crc: number
  resync: number
  drops: number
}

export interface LinkInfo {
  ok: true
  mode: LinkMode
  port: string | null
  baud: number
  connected: boolean
  stats: LinkStats
  telem_hz: number
  last_rx_ms: number | null
  rtt_ms: number | null
  fw: FwInfo | null
}

export interface HealthInfo {
  ok: true
  version: string
  uptime_ms: number
  sim: boolean
}

/** 参数分组：契约固定这几个取值 */
export type ParamGroup = '速度环' | '角度环' | '循迹环' | '串级' | '机械/车辆' | '电源' | '任务'

export interface ParamItem {
  id: number
  key: string
  label: string
  group: ParamGroup
  unit: string
  value: number
  default: number
  min: number
  max: number
  decimals: number
  step: number
  dirty: boolean
  note?: string
}

export interface ParamsResponse {
  ok: true
  param_crc: number
  source: 'fw' | 'local'
  items: ParamItem[]
}

export interface ActionItem {
  n: number
  name: string
  a1: number
  a2: number
  preset_a1: number | null
  preset_a2: number | null
  dirty: boolean
  compound: boolean
  steps?: string
}

export interface ActionsResponse {
  ok: true
  items: ActionItem[]
}

export interface CalChannel {
  i: number
  blk: number
  wht: number
  thr: number
  jitter: number
  pol: number
  ok: boolean
  reason: number
  level: number
  thr_level: number
}

export interface CalResponse {
  ok: true
  /** 当前在用第几套标定（0 = 普通 / 1 = 亚克力）。
   *  来源：固件的第二帧 `T_CAL_REPORT_B`（★2026-10-11 加）。**老固件不回这一帧 ⇒ 是 null**，
   *  界面要显示成「当前套 ?」而不是当成 0。 */
  active_set: number | null
  quality: number
  /** **当前那套**的 8 路标定。 */
  ch: CalChannel[]
  /** ★**另一套**的 8 路标定（T_CAL_REPORT_B 带来）。老固件 / 没拿到时是空数组。
   *  传感器页把两套并排画图，就是为了在两种地面之间对比标定是否真的分开。 */
  other?: CalChannel[]
}

/** ★2026-10-11：航位推算轨迹（编码器 mm/s + IMU yaw 积分出来的俯视路线）。
 *  桥按 5 Hz 通过 SSE 的 route 事件推**增量**（from = 这批点从第几个开始），
 *  reset: true 那一次带**全量**（新打开的页面靠它一次拿全）。
 *  坐标单位 mm，原点 = 清零那一刻车的位置，x 右 / y 前（数学系，画图时 y 要翻）。 */
export interface RouteMark {
  x: number
  y: number
  /** state = 状态切换，lost = 丢线 */
  k: 'state' | 'lost'
  /** state 时是状态号 */
  v: number
  i: number
}

export interface RouteEvent {
  from: number
  pts: [number, number][]
  marks: RouteMark[]
  dist_mm: number
  n: number
  head: number
  /** 本次清零后第一帧的航向（画板拿它当"正右"） */
  yaw0: number
  cur: [number, number]
  box?: [number, number, number, number]
  reset?: boolean
}

export interface PresetItem {
  name: string
  kind: 'actions' | 'params' | 'servo' | 'mixed'
  updated_ms: number
  keys: string[]
  note?: string
}

export interface PresetsResponse {
  ok: true
  items: PresetItem[]
}

export interface TelemFlags {
  lost: boolean
  slope: boolean
  gyro_ok: boolean
  motor: boolean
  host_drive: boolean
  logging: boolean
  flash: boolean
  fault: boolean
}

/** §4 的 telem 事件 */
export interface Telem {
  t: number
  t_host: number
  yaw: number
  /** ★2026-10-15：**连续域航向**（把 ±180 取模的 yaw 逐帧解卷绕累加），由 store.onTelem 填。
   *  仅供显示/绘图：波形不再在 ±180 处瞬跳，页面读数也不打架。与固件 s_yaw_cont 同口径，不参与控制。 */
  yawCont?: number
  pitch: number
  roll: number
  vl: number
  vr: number
  pwm_l: number
  pwm_r: number
  vbat_mv: number
  ibat_ma: number
  servo1_us: number
  servo2_us: number
  servo1_deg: number
  servo2_deg: number
  line: number[]
  gray: number[]
  /** 解释器当前状态号（原「renwu 步号」）；0xFF = 没载入 / 没在跑 */
  state_id: number
  loop_us: number
  mode: number
  /** 当前任务号（TELEM2[51]）；0xFF = 没在跑 */
  task_id: number
  action: number
  trace: { pos: number; pid: number }
  angle: { err: number; rate_ref: number; corr: number }
  flags: TelemFlags
  nvram: number
}

export interface StatsEvent {
  stats: LinkStats
  /** **实测**遥测帧率（桥按 1s 窗口数出来的）—— 显示帧率用这个。
   *  区分见 `telem_hz_set`：那是车上固件报的**设定值**（`/api/link` 的 `telem_hz` 也是它，
   *  「控制 → 遥测频率」滑块的初值取设定值）。2026-10-05 修：桥原来只回设定值 ⇒
   *  用户把频率调到 200Hz，界面还显示 100Hz，永远看不出链路真实吞吐。 */
  telem_hz: number
  /** 车上固件报的设定帧率（`CMD_SET_TELEM_HZ` 的那个目标值） */
  telem_hz_set: number
  /** 实测的**下行**帧率（桥每秒往车上发的命令/轮询帧数） */
  telem_tx_hz: number
  /** 车上主循环里界面刷新耗时，单位 us（固件遥测 loop_us，不是控制中断耗时） */
  loop_us: number
  last_rx_ms: number | null
  busy_pct: number
}

export type LogLevel = 'rx' | 'tx' | 'info' | 'warn' | 'error'

export interface LogItem {
  ts_ms: number
  level: LogLevel
  text: string
}

export interface AckItem {
  seq: number
  req: string
  status: number
  ok: boolean
  text: string
}

export interface StateEvent {
  link: LinkInfo
  params: ParamsResponse
  actions: ActionsResponse
  presets: PresetItem[]
  cal: CalResponse
}

/** 整车模式：0 空闲 / 1 陀螺 / 2 循迹 / 3 串级 */
export const MODE_TEXT: Record<number, string> = {
  0: '空闲',
  1: '陀螺',
  2: '循迹',
  3: '串级',
}

/** 任务号直出：0xFF（没在跑）与越界值都显示成 `--`，其余 `任务 #N` */
export function taskText(id: number | null | undefined): string {
  if (id === null || id === undefined || id === 0xff || id > 0xfe) return '--'
  return `任务 #${id}`
}

/** 解释器状态号：0xFF = 没载入 / 没在跑 */
export function stateText(id: number | null | undefined): string {
  if (id === null || id === undefined || id === 0xff || id > 0xfe) return '--'
  return `#${id}`
}

/** NVRAM 状态（★2026-10-14 审计 #11：以**固件**为准的 3 态）。
 *
 *  真值来源 `firmware/App/Svc/nvram.h:49-51`：
 *    0 IDLE  没在保存
 *    1 BUSY  到点要写了：提示上屏中 / 正在擦写
 *    2 DONE  刚写成功，"保存成功"还挂着
 *  固件**没有失败态**（写失败只悄悄回 IDLE）⇒ 界面不许承诺一个到不了的"失败"徽章。
 *
 *  这里原来按 5 态解释（1 待保存 / 2 保存中 / 3 已保存 / 4 失败），而固件发的是 2=DONE
 *  ⇒ 写完那一瞬间界面显示的是"保存中"、徽章永远不变绿。
 */
export const NVRAM_TEXT: Record<number, string> = {
  0: '空闲',
  1: '正在写',
  2: '已保存',
}

/** 驾驶模式 */
export type DriveMode = 'off' | 'wheels' | 'base_turn' | 'trace' | 'trace2'

export interface DriveRequest {
  mode: DriveMode
  /** `wheels`：左轮速度刻度（0..7200，负 = 反转）；其他模式忽略 */
  vl?: number
  /** `wheels`：右轮速度刻度（0..7200） */
  vr?: number
  /** `base_turn`：基准速度刻度（0..7200，负 = 倒车）；其他模式忽略 */
  base?: number
  /** `base_turn`：目标航向偏移（度，正 = 左转），相对**进入本模式时**的航向；其他模式忽略 */
  turn?: number
}

/** /api/cmd 支持的命令（`start_a/b/c` 已废弃：固件删了内置任务流程，收到回 `ST_UNSUPPORTED`） */
/**
 * `/api/cmd` 的**规范命令名** —— 与桥上唯一那张表 `bridge/state.py:CMD_NAMES` 逐字一致
 * （2026-10-04 修：网页原来发的是 `telem_hz`，桥上叫 `set_telem_hz` ⇒ 每次「下发频率」
 *  都被回「没有这个命令: telem_hz」。`bridge/protocol.py` 里那张同义表 `CMD_BY_NAME`
 *  是没人引用的死代码，已删除。）
 */
export type CmdName =
  | 'stop'
  /** @deprecated 用 `#/tasks` 页的「开始」；固件回「不支持」 */
  | 'start_a'
  /** @deprecated 同上 */
  | 'start_b'
  /** @deprecated 同上 */
  | 'start_c'
  | 'calib_gyro'
  | 'save_params'
  | 'clear_fault'
  | 'reset'
  | 'set_telem_hz'
  | 'set_logging'
  | 'set_telem_mode'
