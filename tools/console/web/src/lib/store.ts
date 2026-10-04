/**
 * 全局数据层：一条 SSE 连接 + 一份环形缓冲 + 所有写操作。
 *
 * 为什么不用 redux/zustand：这个控制台的"热"路径只有一条 SSE。
 * 用一个手写的发布订阅 + `useSyncExternalStore`，热点更新（telem 20Hz 合并）
 * 不影响低频状态（params/actions/presets）：低频页面订阅 `useMeta()`。
 *
 * 契约要点：
 *  - 写操作失败 → 把后端 `error` 原文塞进 toast，不做二次翻译（API.md §1/§5）；
 *  - 环形缓冲默认 30000 点，设置页可改（API.md §5）；
 *  - 断线由浏览器 EventSource 自动重连，期间保留缓冲（API.md §5）。
 */

import { useCallback, useRef, useSyncExternalStore } from 'react'
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
  /** 操控页滑块的步进（速度刻度） */
  driveStep: number
  /** 操控页速度滑块跳过静摩擦死区（刻度低于 DEAD_SCALE 的行程不要） */
  driveSkipDead: boolean
  /** 是否显示"真车模式"安全提示 */
  safetyBanner: boolean
  /**
   * 低特效模式：常开地关掉玻璃模糊与脉冲动效（滚动时本来就会自动关，见 lib/fx.ts）。
   * 给显卡弱/远程桌面的机器用 —— 那些机器上静止时也会卡。
   */
  perfLite: boolean
}

const SETTINGS_KEY = 'console.settings.v1'
const DEFAULT_SETTINGS: Settings = {
  ringPoints: 30000,
  windowSec: 10,
  paused: false,
  scopeChannels: ['yaw', 'pitch', 'roll', 'pwm_l', 'pwm_r'],
  driveStep: 100,
  driveSkipDead: true,
  safetyBanner: true,
  perfLite: false,
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
}

/**
 * 「不含热字段」的快照：`telem` / `ringFill` / `version` 每帧都动。
 * 只显示低频状态（参数表、设置、日志）的页面用 `useMeta()` 订阅它，
 * 就不会被 20Hz 的遥测拖着一起重渲染。
 */
export type MetaSnapshot = Omit<ConsoleSnapshot, 'telem' | 'ringFill' | 'version'>

function snapshotMeta(s: ConsoleSnapshot): MetaSnapshot {
  const { telem, ringFill, version, ...meta } = s
  void telem
  void ringFill
  void version
  return meta
}

const LOG_CAP = 500
const ACK_CAP = 200
const TOAST_CAP = 4
/** 遥测是 100Hz，界面按这个间隔合并刷新（50ms = 20Hz，看得清又不费） */
const HOT_MS = 50

class ConsoleStore {
  private snap: ConsoleSnapshot = {
    version: 0,
    sse: 'connecting',
    link: null,
    params: null,
    actions: null,
    cal: null,
    presets: [],
    telem: null,
    stats: null,
    logs: [],
    acks: [],
    toasts: [],
    settings: loadSettings(),
    ringFill: 0,
    lastError: null,
    ports: [],
  }

  readonly ring = new SeriesRing(loadSettings().ringPoints, CHANNELS.length)

  private listeners = new Set<() => void>()
  private metaListeners = new Set<() => void>()
  private metaSnap: MetaSnapshot = snapshotMeta(this.snap)
  private es: EventSource | null = null
  private toastSeq = 1
  private lastHotBump = 0
  private hotDirty = false
  private hotTimer: ReturnType<typeof setTimeout> | null = null
  private hotRaf: number | null = null

  // ---- 订阅（useSyncExternalStore） ----
  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn)
    return () => {
      this.listeners.delete(fn)
    }
  }

  getSnapshot = (): ConsoleSnapshot => this.snap

  /** 只给窄订阅用：热 tick 号（每次 emit/热更新 +1）。引用恒定，永远不会让 React 重订阅。 */
  getVersion = (): number => this.snap.version

  /** 低频状态（不含 telem），只显示参数/设置/日志的页面用它 */
  subscribeMeta = (fn: () => void): (() => void) => {
    this.metaListeners.add(fn)
    return () => {
      this.metaListeners.delete(fn)
    }
  }

  getMetaSnapshot = (): MetaSnapshot => this.metaSnap

  /** 低频变更（非遥测）：重建 meta 快照，两拨订阅者都通知 */
  private emit(): void {
    this.snap = { ...this.snap, version: this.snap.version + 1 }
    this.metaSnap = snapshotMeta(this.snap)
    this.listeners.forEach((fn) => fn())
    this.metaListeners.forEach((fn) => fn())
  }

  /** 热路径（遥测）：只换 version/telem，不碰 meta —— meta 订阅者不该跟着抖 */
  private emitHotNow(): void {
    this.snap = { ...this.snap, version: this.snap.version + 1 }
    this.listeners.forEach((fn) => fn())
  }

  /** 通知热订阅者：浏览器里挂到**下一次绘制帧**（rAF）—— 同一帧内的多次通知合并成
   *  一次渲染，而且标签页切到后台时 rAF 根本不触发（不白醒、不白算波形），切回来
   *  时下一次 rAF 一次补上最新数据。Node（SSR 预览生成器 / 单测）里没有 window，
   *  直接同步通知，行为与从前一致。 */
  private renderHot(): void {
    const raf = typeof window !== 'undefined' && typeof window.requestAnimationFrame === 'function'
      ? window.requestAnimationFrame
      : null
    if (!raf) {
      this.emitHotNow()
      return
    }
    if (this.hotRaf !== null) return
    this.hotRaf = raf(() => {
      this.hotRaf = null
      this.emitHotNow()
    })
  }

  /** telem 是 100Hz，界面按 HOT_MS(50ms) 合并刷新、渲染挂在 rAF 上（波形自己按窗口
   *  取数，不吃这个节流）。用全局 `setTimeout` 而不是 `window.setTimeout`：SSR 预览
   *  生成器（Node 里跑）喂遥测时也要能走同一条节流路径。 */
  private emitHot(): void {
    this.hotDirty = true
    const now = Date.now()
    if (now - this.lastHotBump < HOT_MS) {
      if (this.hotTimer === null) {
        this.hotTimer = setTimeout(() => {
          this.hotTimer = null
          if (this.hotDirty) {
            this.hotDirty = false
            this.lastHotBump = Date.now()
            this.renderHot()
          }
        }, HOT_MS - (now - this.lastHotBump))
      }
      return
    }
    this.hotDirty = false
    this.lastHotBump = now
    this.renderHot()
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
    const [link, params, actions, presets, cal] = await Promise.allSettled([
      api.link(),
      api.params(),
      api.actions(),
      api.presets(),
      api.cal(),
    ])
    const next = { ...this.snap }
    if (link.status === 'fulfilled' && link.value) next.link = link.value as unknown as LinkInfo
    if (params.status === 'fulfilled') next.params = params.value
    if (actions.status === 'fulfilled') next.actions = actions.value
    if (presets.status === 'fulfilled') next.presets = presets.value.items
    if (cal.status === 'fulfilled') next.cal = cal.value
    this.snap = next
    this.emit()
  }

  // ---- 链路 ----
  async connect(mode: 'serial' | 'sim', port?: string, baud?: number): Promise<void> {
    await this.op(
      '连接',
      () => api.connect(mode, port, baud),
      `已发起连接（${mode}${port ? ' ' + port : ''}）`,
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

  /**
   * 只清日志/ACK（不动波形缓冲）。
   *
   * 本地先清、桥那边也尽力清一遍（`POST /api/log/clear`，桥早就有了）。不通知桥的旧行为
   * 看着像"清不掉"：下一次 `refreshAll()`（GET `/api/log`）会把旧日志原样拉回来。
   * 桥清失败只在本地清空之后报一声，不影响使用。
   */
  clearLogs(): void {
    this.snap = { ...this.snap, logs: [], acks: [] }
    this.emit()
    void api.logClear().catch((e: Error) => {
      this.toast('err', `桥侧日志没清掉（界面已清空）：${e.message}`)
    })
  }
}

export const store = new ConsoleStore()

export function useStore(): ConsoleSnapshot {
  return useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot)
}

/** 只订阅低频状态：拿不到 `telem`，也就不会被 20Hz 的遥测拖着重渲染 */
export function useMeta(): MetaSnapshot {
  return useSyncExternalStore(store.subscribeMeta, store.getMetaSnapshot, store.getMetaSnapshot)
}

/**
 * 只拿"热 tick 号"（一个数字），不搬运整份快照。
 *
 * 吃 telem 的**叶子**组件用它：引用恒定的数字，React 不会重订阅；需要重算的东西
 * （波形、数字）把它挂进 `useMemo` 依赖里。页面壳不要用它 —— 页面壳要的是"我不跟着抖"。
 */
export function useHotVersion(): number {
  return useSyncExternalStore(store.subscribe, store.getVersion, store.getVersion)
}

/** `useHotSelect` 的返回值必须是**标量**：React 用 `Object.is` 比对，新对象会让它一直重渲染 */
type Scalar = string | number | boolean | null | undefined

/**
 * 从一个热快照里挑一个**标量**：挑出来的值没变（`Object.is`）就 bail out，不重渲染。
 *
 * 约束（沿用 `useMeta()` 那条约定）：`select` 必须是纯函数、且**每次调用都不分配**
 * （返回 number/string/bool/null）。返回新对象等于每次都快照不一致，会踩
 * "The result of getSnapshot should be cached" 并陷入无限重渲染。
 * `select` 放进 ref：内联箭头函数每次都换引用，但不能因此重订阅。
 */
export function useHotSelect<T extends Scalar>(select: (s: ConsoleSnapshot) => T): T {
  const fnRef = useRef(select)
  fnRef.current = select
  const get = useCallback(() => fnRef.current(store.getSnapshot()), [])
  return useSyncExternalStore(store.subscribe, get, get)
}
