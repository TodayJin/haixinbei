/**
 * 与后端契约 `tools/console/docs/API.md`（冻结版 v1）一一对应的类型。
 * 契约里的字段名/单位不要改；这里只是把它们写成 TS 类型。
 */

/** 链路模式 */
export type LinkMode = 'sim' | 'serial' | 'replay'

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

/** 一个可回放的录制文件（`GET /api/recordings`） */
export interface RecItem {
  name: string
  path: string
  size: number
  mtime_ms: number
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
  active_set: number
  quality: number
  ch: CalChannel[]
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
  telem_hz: number
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

/** NVRAM 状态：0 空闲 / 1 待保存 / 2 保存中 / 3 已保存 / 4 失败 */
export const NVRAM_TEXT: Record<number, string> = {
  0: '空闲',
  1: '待保存',
  2: '保存中',
  3: '已保存',
  4: '失败',
}

/** 驾驶模式 */
export type DriveMode = 'off' | 'wheels' | 'base_turn'

export interface DriveRequest {
  mode: DriveMode
  vl?: number
  vr?: number
  base?: number
  turn?: number
}

/** /api/cmd 支持的命令（`start_a/b/c` 已废弃：固件删了内置任务流程，收到回 `ST_UNSUPPORTED`） */
export type CmdName =
  | 'stop'
  /** @deprecated 用 `#/tasks` 页的「开始」；固件回「不支持」 */
  | 'start_a'
  /** @deprecated 同上 */
  | 'start_b'
  /** @deprecated 同上 */
  | 'start_c'
  | 'clear_fault'
  | 'reset'
  | 'telem_hz'

export interface RecordStatus {
  ok: true
  recording: boolean
  path: string | null
  samples: number
  started_ms: number | null
}
