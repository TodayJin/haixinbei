/**
 * 全局数据层：一条 SSE 连接 + 一份环形缓冲 + 所有写操作。
 *
 * 为什么不用 redux/zustand：这个控制台的"热"路径只有一条 SSE。
 * 用一个手写的发布订阅 + `useSyncExternalStore`，热点更新（telem 10Hz 节流）
 * 不影响低频状态（params/actions/presets）。
 *
 * 契约要点：
 *  - 写操作失败 → 把后端 `error` 原文塞进 toast，不做二次翻译（API.md §1/§5）；
 *  - 环形缓冲默认 30000 点，设置页可改（API.md §5）；
 *  - 断线由浏览器 EventSource 自动重连，期间保留缓冲（API.md §5）。
 */

import { useSyncExternalStore } from 'react'
import { api, normalizePresets, openStream } from './api'
import { CHANNELS } from './channels'
import { SeriesRing } from './ring'
import type {
  AckItem,
  ActionsResponse,
  CalResponse,
  LinkInfo,
  LogItem,
  ParamItem,
  ParamsResponse,
  PresetItem,
  RecItem,
  RecordStatus,
  StatsEvent,
  Telem,
} from './types'

export type ToastKind = 'ok' | 'err' | 'info'

export interface Toast {
  id: number
  kind: ToastKind
  text: string
  at: number
}

export interface Settings {
  /** 环形缓冲点数（设置页可改） */
  ringPoints: number
  /** 波形窗口秒数 */
  windowSec: 5 | 10 | 20 | 30
  /** 波形暂停（暂停时缓冲照收，只是冻住显示窗口） */
  paused: boolean
  /** 波形勾选的通道 key */
  scopeChannels: string[]
  /** 操控页滑块的步进（mm/s） */
  driveStep: number
  /** 是否显示"真车模式"安全提示 */
  safetyBanner: boolean
}

const SETTINGS_KEY = 'console.settings.v1'
const DEFAULT_SETTINGS: Settings = {
  ringPoints: 30000,
  windowSec: 10,
  paused: false,
  scopeChannels: ['yaw', 'pitch', 'roll', 'pwm_l', 'pwm_r'],
  driveStep: 100,
  safetyBanner: true,
}

function loadSettings(): Settings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY)
    if (!raw) return { ...DEFAULT_SETTINGS }
    const parsed = JSON.parse(raw) as Partial<Settings>
    return { ...DEFAULT_SETTINGS, ...parsed }
  } catch {
    return { ...DEFAULT_SETTINGS }
  }
}

function saveSettings(s: Settings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(s))
  } catch {
    /* 隐私模式/无 localStorage：静默 */
  }
}

export interface ConsoleSnapshot {
  version: number
  sse: 'connecting' | 'open' | 'closed'
  link: LinkInfo | null
  params: ParamsResponse | null
  actions: ActionsResponse | null
  cal: CalResponse | null
  presets: PresetItem[]
  record: RecordStatus | null
  telem: Telem | null
  stats: StatsEvent | null
  logs: LogItem[]
  acks: AckItem[]
  toasts: Toast[]
  settings: Settings
  ringFill: number
  lastError: string | null
  /** 桥扫描到的串口（设置页下拉用） */
  ports: string[]
  /** 能回放的录制文件（设置页「回放」卡用） */
  recordings: RecItem[]
}

const LOG_CAP = 500
const ACK_CAP = 200
const TOAST_CAP = 4

class ConsoleStore {
  private snap: ConsoleSnapshot = {
    version: 0,
    sse: 'connecting',
    link: null,
    params: null,
    actions: null,
    cal: null,
    presets: [],
    record: null,
    telem: null,
    stats: null,
    logs: [],
    acks: [],
    toasts: [],
    settings: loadSettings(),
    ringFill: 0,
    lastError: null,
    ports: [],
    recordings: [],
  }

  readonly ring = new SeriesRing(loadSettings().ringPoints, CHANNELS.length)

  private listeners = new Set<() => void>()
  private es: EventSource | null = null
  private toastSeq = 1
  private lastHotBump = 0
  private hotDirty = false
  private hotTimer: ReturnType<typeof setTimeout> | null = null

  // ---- 订阅（useSyncExternalStore） ----
  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn)
    return () => {
      this.listeners.delete(fn)
    }
  }

  getSnapshot = (): ConsoleSnapshot => this.snap

  private emit(): void {
    this.snap = { ...this.snap, version: this.snap.version + 1 }
    this.listeners.forEach((fn) => fn())
  }

  /** telem 是 50Hz，界面按 10Hz 刷（波形自己按窗口取数，不吃这个节流）。
   *  用全局 `setTimeout` 而不是 `window.setTimeout`：SSR 预览生成器（Node 里跑）
   *  喂遥测时也要能走同一条节流路径。 */
  private emitHot(): void {
    this.hotDirty = true
    const now = Date.now()
    if (now - this.lastHotBump < 100) {
      if (this.hotTimer === null) {
        this.hotTimer = setTimeout(() => {
          this.hotTimer = null
          if (this.hotDirty) {
            this.hotDirty = false
            this.lastHotBump = Date.now()
            this.emit()
          }
        }, 100 - (now - this.lastHotBump))
      }
      return
    }
    this.hotDirty = false
    this.lastHotBump = now
    this.emit()
  }

  // ---- toast ----
  toast(kind: ToastKind, text: string): void {
    const t: Toast = { id: this.toastSeq++, kind, text, at: Date.now() }
    this.snap = { ...this.snap, toasts: [...this.snap.toasts, t].slice(-TOAST_CAP) }
    this.emit()
    window.setTimeout(() => this.dismissToast(t.id), kind === 'err' ? 8000 : 3500)
  }

  dismissToast(id: number): void {
    const next = this.snap.toasts.filter((t) => t.id !== id)
    if (next.length !== this.snap.toasts.length) {
      this.snap = { ...this.snap, toasts: next }
      this.emit()
    }
  }

  /** 统一包一层：成功给 ok toast，失败把后端 error 原样抛给用户 */
  private async op<T>(label: string, fn: () => Promise<T>, okText?: string): Promise<T | null> {
    try {
      const r = await fn()
      if (okText) this.toast('ok', okText)
      return r
    } catch (e) {
      const msg = (e as Error).message || '操作失败'
      this.snap = { ...this.snap, lastError: msg }
      this.toast('err', `${label}：${msg}`)
      this.emit()
      return null
    }
  }

  // ---- 连接 ----
  start(): void {
    if (this.es) return
    this.snap = { ...this.snap, sse: 'connecting' }
    this.emit()
    this.es = openStream({
      onLinkChange: (s) => {
        this.snap = { ...this.snap, sse: s }
        this.emit()
        if (s === 'closed') this.toast('err', 'SSE 流断开，浏览器会自动重连…')
      },
      onState: (payload) => {
        const p = payload as {
          link?: LinkInfo
          params?: ParamsResponse
          actions?: ActionsResponse
          presets?: PresetItem[]
          cal?: CalResponse
        }
        this.snap = {
          ...this.snap,
          link: p.link ?? this.snap.link,
          params: p.params ?? this.snap.params,
          actions: p.actions ?? this.snap.actions,
          presets: p.presets ? normalizePresets(p.presets) : this.snap.presets,
          cal: p.cal ?? this.snap.cal,
        }
        this.emit()
      },
      onTelem: (t) => this.onTelem(t),
      onStats: (s) => {
        this.snap = { ...this.snap, stats: s, link: this.snap.link ? { ...this.snap.link, stats: s.stats, telem_hz: s.telem_hz, last_rx_ms: s.last_rx_ms } : this.snap.link }
        this.emit()
      },
      onLog: (item) => {
        this.snap = { ...this.snap, logs: [...this.snap.logs, item].slice(-LOG_CAP) }
        this.emit()
      },
      onAck: (item) => {
        this.snap = { ...this.snap, acks: [...this.snap.acks, item].slice(-ACK_CAP) }
        if (!item.ok) this.toast('err', `车上拒绝：${item.text || item.req}`)
        this.emit()
      },
      onActions: (a) => {
        this.snap = { ...this.snap, actions: a }
        this.emit()
      },
      onParams: (p) => {
        this.snap = { ...this.snap, params: p }
        this.emit()
      },
      onCal: (c) => {
        this.snap = { ...this.snap, cal: c }
        this.emit()
      },
      onError: (msg) => {
        this.snap = { ...this.snap, lastError: msg }
        this.toast('err', msg)
        this.emit()
      },
    })
    void this.refreshAll()
  }

  stop(): void {
    this.es?.close()
    this.es = null
  }

  private onTelem(t: Telem): void {
    this.ring.push(t.t_host || t.t || Date.now(), CHANNELS.map((c) => c.get(t)))
    this.snap = { ...this.snap, telem: t, ringFill: this.ring.size }
    this.emitHot()
  }

  /**
   * 把一帧遥测塞进来（走的是和 SSE 完全相同的那条路径）。
   *
   * 只给一种场合用：离线预览/回归 —— `ssrPreview.test.tsx` 先用 mock 的数据喂进来，再渲染页面。
   * 真机数据一律走 `openStream()`，不要在这里造假数据。
   */
  feedTelem(t: Telem): void {
    this.onTelem(t)
  }

  // ---- 首屏/手动刷新 ----
  async refreshAll(): Promise<void> {
    const [link, params, actions, presets, cal, rec] = await Promise.allSettled([
      api.link(),
      api.params(),
      api.actions(),
      api.presets(),
      api.cal(),
      api.recordStatus(),
    ])
    const next = { ...this.snap }
    if (link.status === 'fulfilled' && link.value) next.link = link.value as unknown as LinkInfo
    if (params.status === 'fulfilled') next.params = params.value
    if (actions.status === 'fulfilled') next.actions = actions.value
    if (presets.status === 'fulfilled') next.presets = presets.value.items
    if (cal.status === 'fulfilled') next.cal = cal.value
    if (rec.status === 'fulfilled') next.record = rec.value.record
    this.snap = next
    this.emit()
  }

  // ---- 链路 ----
  /**
   * 连链路。`replay` 是回放要用的 jsonl 路径（只有回放模式需要），
   * `speed` 是回放倍速（1.0 = 原速）。
   */
  async connect(
    mode: 'serial' | 'sim' | 'replay',
    port?: string,
    baud?: number,
    replay?: string,
    speed?: number,
  ): Promise<void> {
    await this.op(
      '连接',
      () => api.connect(mode, port, baud, replay, speed),
      `已发起连接（${mode}${mode === 'replay' && replay ? '：' + replay : port ? ' ' + port : ''}）`,
    )
    await this.refreshAll()
  }

  async disconnect(): Promise<void> {
    await this.op('断开', () => api.disconnect(), '已断开')
    await this.refreshAll()
  }

  /** 让桥重新扫一遍串口（设置页下拉用） */
  async listPorts(): Promise<void> {
    try {
      const r = await api.ports()
      const list = (r.ports ?? []).map((p) => p.port)
      this.snap = { ...this.snap, ports: list }
      this.emit()
    } catch (e) {
      this.toast('err', `扫描串口失败：${(e as Error).message}`)
    }
  }

  /** 列出 `data/logs/*.jsonl`（设置页回放下拉用） */
  async listRecordings(): Promise<void> {
    try {
      const r = await api.recordings()
      this.snap = { ...this.snap, recordings: r.items ?? [] }
      this.emit()
    } catch (e) {
      this.toast('err', `读录制文件列表失败：${(e as Error).message}`)
    }
  }

  /** 急停：停车 + 关闭上位机驾驶（Esc 快捷键也走这里） */
  async emergencyStop(): Promise<void> {
    await this.op('急停', async () => {
      await api.drive({ mode: 'off' })
      await api.cmd('stop')
    }, '已急停：停车 + 交还控制权')
  }

  // ---- 参数 ----
  /** 本地改一个参数（id 或 key 都收）：先本地立刻生效，再把值同步进桥的缓存。
   *  桥不会自动下发 —— 点「全部下发」才把脏的那批 push 到车（契约 §2）。 */
  setParam(idOrKey: number | string, value: number): void {
    const p = this.snap.params
    if (!p) return
    const hit = p.items.find((it) =>
      typeof idOrKey === 'number' ? it.id === idOrKey : it.key === idOrKey,
    )
    if (!hit) return
    const items = p.items.map((it) =>
      it === hit ? { ...it, value, dirty: value !== it.default } : it,
    )
    this.snap = { ...this.snap, params: { ...p, items } }
    this.emit()
    // 脏标记的权威在桥那边（脏 = 与车上值不同）；桥改完会走 SSE `params` 回灌
    void api.paramSet(hit.key, value).catch((e) => {
      this.toast('err', `参数没同步进桥：${(e as Error).message}`)
    })
  }

  async pushParams(keys?: string[]): Promise<void> {
    const r = await this.op('下发参数', () => api.paramsPush(keys), '参数已下发到小车')
    if (r) await this.refreshParams()
  }

  async saveParams(): Promise<void> {
    const r = await this.op('固化参数', () => api.paramsSave(), '已请求小车写 Flash')
    if (r) await this.refreshParams()
  }

  async reloadParams(): Promise<void> {
    const r = await this.op('读回参数', () => api.paramsReload(), '已从小车读回参数')
    if (r) await this.refreshParams()
  }

  async paramsDefaults(keys?: string[]): Promise<void> {
    const r = await this.op('恢复默认', () => api.paramsDefaults(keys), '已恢复默认值（记得下发）')
    if (r) await this.refreshParams()
  }

  async refreshParams(): Promise<void> {
    try {
      this.snap = { ...this.snap, params: await api.params() }
      this.emit()
    } catch (e) {
      this.toast('err', `读参数失败：${(e as Error).message}`)
    }
  }

  paramByKey(key: string): ParamItem | undefined {
    return this.snap.params?.items.find((p) => p.key === key)
  }

  // ---- 动作 ----
  setAction(n: number, a1: number, a2: number): void {
    const a = this.snap.actions
    if (!a) return
    this.snap = {
      ...this.snap,
      actions: { ...a, items: a.items.map((it) => (it.n === n ? { ...it, a1, a2, dirty: true } : it)) },
    }
    this.emit()
  }

  async playAction(n: number): Promise<void> {
    await this.op('播放动作', () => api.actionPlay(n), `${n} 号动作已下发`)
  }

  async saveActions(): Promise<void> {
    const r = await this.op('固化动作表', () => api.actionSave(), '已请求小车把动作表写 Flash')
    if (r) await this.refreshActions()
  }

  async refreshActions(): Promise<void> {
    try {
      this.snap = { ...this.snap, actions: await api.actions() }
      this.emit()
    } catch (e) {
      this.toast('err', `读动作表失败：${(e as Error).message}`)
    }
  }

  // ---- 舵机 ----
  async setServo(ch: 1 | 2, angle: number): Promise<void> {
    await this.op('舵机', () => api.servo(ch, angle))
  }

  async centerServo(): Promise<void> {
    await this.op('舵机回中', () => api.servoCenter(), '两路舵机已回中')
  }

  async setDeviation(ch: 1 | 2, dev_us: number): Promise<void> {
    await this.op('中位偏差', () => api.servoDeviation(ch, dev_us), `舵机${ch} 中位偏差 ${dev_us}µs`)
  }

  // ---- 驾驶 / 命令 ----
  /** 驾驶指令：三种模式（off/wheels/base_turn），只改目标速度，闭环还是车上的三环 */
  async drive(req: { mode: 'off' | 'wheels' | 'base_turn'; vl?: number; vr?: number; base?: number; turn?: number }): Promise<void> {
    await this.op('驾驶', () => api.drive(req), req.mode === 'off' ? '已停车（host_drive 关闭）' : '驾驶指令已下发')
  }

  async cmd(cmd: string, arg?: number): Promise<void> {
    await this.op('命令', () => api.cmd(cmd, arg), `命令 ${cmd} 已下发`)
  }

  async toggleRecord(): Promise<void> {
    const on = this.snap.record?.recording ?? false
    const r = await this.op('记录', () => api.record(on ? 'stop' : 'start'), on ? '已停止记录' : '开始记录 JSONL')
    if (r) {
      this.snap = { ...this.snap, record: r.record }
      this.emit()
    }
  }

  // ---- 预设 ----
  async savePreset(name: string, targets: string[], note?: string): Promise<void> {
    const r = await this.op('保存预设', () => api.presetSave(name, targets, note), `预设「${name}」已保存`)
    if (r) await this.refreshPresets()
  }

  async applyPreset(name: string, targets: string[]): Promise<void> {
    const r = await this.op('套用预设', () => api.presetApply(name, targets), `已套用「${name}」`)
    if (r) {
      await this.refreshAll()
    }
  }

  async deletePreset(name: string): Promise<void> {
    const r = await this.op('删除预设', () => api.presetDelete(name), `已删除「${name}」`)
    if (r) await this.refreshPresets()
  }

  async refreshPresets(): Promise<void> {
    try {
      const r = await api.presets()
      this.snap = { ...this.snap, presets: r.items }
      this.emit()
    } catch (e) {
      this.toast('err', `读预设失败：${(e as Error).message}`)
    }
  }

  // ---- 设置 ----
  patchSettings(patch: Partial<Settings>): void {
    const settings = { ...this.snap.settings, ...patch }
    if (patch.ringPoints !== undefined) this.ring.resize(patch.ringPoints)
    saveSettings(settings)
    this.snap = { ...this.snap, settings, ringFill: this.ring.size }
    this.emit()
  }

  clearBuffers(): void {
    this.ring.clear()
    this.snap = { ...this.snap, ringFill: 0 }
    this.emit()
  }

  /** 只清日志/ACK（不动波形缓冲） */
  clearLogs(): void {
    this.snap = { ...this.snap, logs: [], acks: [] }
    this.emit()
  }
}

export const store = new ConsoleStore()

export function useStore(): ConsoleSnapshot {
  return useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot)
}
