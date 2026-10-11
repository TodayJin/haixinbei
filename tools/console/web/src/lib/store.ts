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
   * 低特效模式（**默认开**）：常开地关掉玻璃模糊与脉冲动效 —— 静止时也不花那笔钱。
   * 用户 m24215 实测"开了确实非常流畅"，所以把它做成默认；关掉才回到玻璃拟态。
   * 视觉上不是"少特效"，是另一套不透明分层主题（见 index.css 末尾）。
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
  perfLite: true,
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
/** 同一句话的去重窗口：错误留长一点（连续下发的拒绝对用户只算一件事） */
const TOAST_DEDUP_ERR_MS = 8000
const TOAST_DEDUP_OK_MS = 1500
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
  /**
   * 同一句话 N 毫秒内只弹一次。
   *
   * 为什么必须有：操控是**连续下发**（`pages/Drive.tsx` 转向时每 `TURN_TICK_MS=100ms`
   * 一帧 `store.drive`），车一旦拒绝（最典型的是任务在跑 ⇒ ST_NOT_READY），
   * 每一帧都走一遍 `op()` ⇒ 屏幕上是 10 条/秒的弹幕，把画面糊住、还关不掉。
   * 用户 m24215："每次操控就会弹出一个后端报错…每次都是"。
   */
  private toastAt = new Map<string, number>()

  toast(kind: ToastKind, text: string): void {
    const key = `${kind}:${text}`
    const now = Date.now()
    const win = kind === 'err' ? TOAST_DEDUP_ERR_MS : TOAST_DEDUP_OK_MS
    if (now - (this.toastAt.get(key) ?? 0) < win) return
    this.toastAt.set(key, now)
    const t: Toast = { id: this.toastSeq++, kind, text, at: now }
    this.snap = { ...this.snap, toasts: [...this.snap.toasts, t].slice(-TOAST_CAP) }
    this.emit()
    // 全局 setTimeout（不是 window.setTimeout）：同 emitHot 的理由 —— 测试/SSR 在 Node 里跑，
    // 没有 window；自动关闭只是个定时器，不需要 DOM。
    setTimeout(() => this.dismissToast(t.id), kind === 'err' ? 8000 : 3500)
  }

  dismissToast(id: number): void {
    const next = this.snap.toasts.filter((t) => t.id !== id)
    if (next.length !== this.snap.toasts.length) {
      this.snap = { ...this.snap, toasts: next }
      this.emit()
    }
  }

  /** 写操作串行队列。
   *
   *  ★为什么必须有（用户 2026-10-10 报的 bug）：串口一次只能跑一条命令，而这个界面可能
   *  连着发两条 —— 最典型的是**失焦自动下发**紧跟**固化**：用户在角度那一格敲完数字直接点
   *  「固化到车」，blur 触发的 `setAction` 和点击触发的 `saveActions` 会抢串口。
   *  固化先跑到的话，它固化的是**车里还没更新的旧值**，紧接着 `refreshActions()` 从车重读
   *  ⇒ 界面**弹回原值**，用户看到的就是"改了保存不了"。
   *  排队之后：后一条一定等前一条的 ACK 回来才发。 */
  private q: Promise<unknown> = Promise.resolve()

  /** 统一包一层：成功给 ok toast，失败把后端 error 原样抛给用户 */
  private async op<T>(label: string, fn: () => Promise<T>, okText?: string): Promise<T | null> {
    /* 挂到队尾：不管前一条成功还是失败都要继续跑下一条（hence 两个 then 分支） */
    const run = this.q.then(() => fn(), () => fn())
    this.q = run.then(
      () => undefined,
      () => undefined,
    )
    try {
      const r = await run
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
        // s.telem_hz 是**实测**帧率、link.telem_hz 是车上报的**设定**值：别把实测写回 link，
        // 否则「控制 → 遥测频率」滑块的初值会跟着实测抖（2026-10-05 用户 m30228）。
        this.snap = { ...this.snap, stats: s, link: this.snap.link ? { ...this.snap.link, stats: s.stats, telem_hz: s.telem_hz_set ?? this.snap.link.telem_hz, last_rx_ms: s.last_rx_ms } : this.snap.link }
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
    // 只有"停车"给成功提示：操控过程中每一帧都吐一条 "驾驶指令已下发" 就是刷屏
    // （转向 10 帧/秒）。失败仍然会弹，由 toast 的去重窗口挡成一条。
    await this.op('驾驶', () => api.drive(req), req.mode === 'off' ? '已停车（host_drive 关闭）' : undefined)
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

/* ------------------------------------------------- 任务上传：到底烧到哪个槽
 *
 * 车上的任务**按槽存**：固件 `firmware/App/Svc/task_store.c` 最多 8 条（槽 0..7），
 * `TaskStore_Commit(slot,…)` 就是"写进那个槽"—— 往一个占用的槽再提交 = **覆盖它**，
 * 目录里不会多出一条（目录解析遇到重复 id 还会跳过）。
 *
 * 上位机原来的做法（`pages/Tasks.tsx` 的 `doBurn()` 直接 `taskApi.burn(task.id, …)`）：
 * 拿**编辑器里那条任务自己的编号**（`taskedit.ts` 的 0..7，`nextTaskId()` 给的是编辑器
 * 列表里的空位）当槽号烧，**从不看车上目录** ⇒ 编辑器里新加一条任务再烧，就会悄悄盖掉
 * 车上同槽位的**别人的**任务（用户 m 问的"同一个任务都是同一个地方的，会覆盖吗？"
 * 就是这个：同一个任务会覆盖，但换一条任务也可能覆盖，且界面上一句提示都没有）。
 *
 * 现在定的规则（用户 m 2026-10-07 要求）：
 *   ① 车上有**同名**任务 ⇒ 用**它那个槽**（覆盖它，界面上明确写"将覆盖车上同名的 #N"）；
 *   ② 没有同名 ⇒ 用**第一个空槽**（0..7 里最小的空闲）；
 *   ③ 8 个槽全满又没有同名 ⇒ **不烧**，给人话提示（绝不悄悄覆盖别人的任务）。
 *
 * 纯函数放这里（不在页面里）是为了让规则可以被单独复核：输入 = 车上目录 + 编辑器里的名字。
 */

/** 车上目录最多 8 条（`firmware/App/Svc/task_store.h` 的 `TASK_STORE_SLOTS`） */
export const TASK_SLOT_COUNT = 8

/**
 * 车上目录页里名字的字节上限（固件 `TASK_STORE_NAME_MAX = 30`）。
 *
 * 超出部分**两端都会砍**：桥 `bridge/protocol.py` 的 `payload_task_commit()` 按 30 字节切，
 * 固件 `task_store.c` 的 `store_build()` 也按 30 字节收。所以发 COMMIT 之前先在
 * **字符边界**上截好（`fitCarName()`），免得车上存半个汉字。
 */
export const CAR_NAME_MAX = 30

/** 按 UTF-8 字节截断、不切坏字符（超出返回前若干个完整字符）。发给车的任务名用它。 */
export function fitCarName(s: string, max: number = CAR_NAME_MAX): string {
  const t = (s ?? '').trim()
  const enc = new TextEncoder()
  if (enc.encode(t).length <= max) return t
  let out = ''
  let used = 0
  for (const ch of t) {
    const n = enc.encode(ch).length
    if (used + n > max) break
    out += ch
    used += n
  }
  return out
}

/**
 * "同名"的比较键：车上只存得下前 30 字节 ⇒ 比同名必须拿**车上真正存下来的那 30 字节**去比，
 * 否则名字长的任务再烧一次就认不出车上那条是自己，会当成新任务又占一个空槽（凭空多出副本）。
 *
 * 半个汉字被砍掉的尾巴在两端都会解码成 U+FFFD（Python `errors="replace"` / JS `TextDecoder`），
 * 这里一律削掉，好让"老代码按字节切出来的名字"也认得出。
 */
export function carNameKey(s: string): string {
  const t = (s ?? '').trim()
  if (!t) return ''
  const bytes = new TextEncoder().encode(t)
  const cut = bytes.length <= CAR_NAME_MAX ? bytes : bytes.subarray(0, CAR_NAME_MAX)
  return new TextDecoder().decode(cut).replace(/\uFFFD+$/, '')
}

/** 车上目录里的一条（只用到这三个字段；形状与 `taskapi.ts` 的 `RemoteTaskInfo` 兼容） */
export interface CarTaskItem {
  id: number
  name?: string
  len?: number
}

export type SlotPlan =
  /** 车上有同名 ⇒ 烧进它那个槽（覆盖） */
  | { kind: 'same-name'; slot: number; carName: string; carLen: number }
  /** 车上没同名 ⇒ 烧进第一个空槽 */
  | { kind: 'free'; slot: number }
  /** 8 个槽全满且没同名 ⇒ 不烧，`error` 就是给用户看的那句话 */
  | { kind: 'full'; error: string }

/** 规则本体：车上目录 + 编辑器里的任务名 ⇒ 烧到哪个槽（或为什么不烧）。 */
export function planTaskSlot(car: CarTaskItem[], name: string): SlotPlan {
  const items = Array.isArray(car) ? car : []
  const want = carNameKey(name)
  if (want) {
    const hits = items
      .filter((it) => carNameKey(it.name ?? '') === want)
      .sort((a, b) => Number(a.id) - Number(b.id))
    if (hits.length) {
      return {
        kind: 'same-name',
        slot: Number(hits[0].id),
        carName: (hits[0].name ?? '').trim(),
        carLen: Number(hits[0].len ?? 0),
      }
    }
  }
  const used = new Set(items.map((it) => Number(it.id)))
  for (let i = 0; i < TASK_SLOT_COUNT; i += 1) {
    if (!used.has(i)) return { kind: 'free', slot: i }
  }
  const slots = `#0~#${TASK_SLOT_COUNT - 1}`
  return {
    kind: 'full',
    error: want
      ? `车上 ${TASK_SLOT_COUNT} 个槽（${slots}）都占着，也没有叫「${want}」的任务可以覆盖 —— 先在左边「车上现有」里删掉一个不用的，再烧写。`
      : `车上 ${TASK_SLOT_COUNT} 个槽（${slots}）都占着，这条任务又没名字（认不出哪个是它）—— 先给它起个名字、或删掉一个不用的，再烧写。`,
  }
}

/** 计划 → 界面上那句人话（烧写前提示、烧写后回执共用同一份文案，省得两处说法不一样）。 */
export function describeSlotPlan(plan: SlotPlan, name: string): string {
  if (plan.kind === 'same-name') {
    return `车上已经有同名的 #${plan.slot}「${plan.carName || carNameKey(name)}」—— 这次是覆盖它，不会多出一条副本。`
  }
  if (plan.kind === 'free') return `车上没有同名任务 —— 会烧到第一个空槽 #${plan.slot}。`
  return plan.error
}

/**
 * 底层报错 → 指向下一步动作的人话（用户 m 2026-10-07 要求：界面上的红字要能告诉人
 * "再试一次 / 先删一个任务 / 链路没连上"，不许只丢一堆术语）。
 *
 * 认不出来的原样返回：那种情况宁可让用户把原文发过来，也不要把线索藏掉
 * （调用方另外 `console.warn` 一份原文，方便查）。
 */
export function humanizeTaskError(raw: string): string {
  const s = (raw ?? '').trim()
  if (!s) return '这次没成功，桥也没说原因 —— 再按一次；反复失败就重启一下桥。'
  if (/连不上桥/.test(s)) {
    return '连不上上位机后端 —— 确认启动窗口里那个 python 还在跑，再刷新这个页面。'
  }
  if (/HOST_CAP_TASK|不支持任务功能/.test(s)) {
    return '车上这份固件不带任务功能 —— 先给它烧一份带任务功能的新固件。'
  }
  if (/链路没打开|没连上|车没上电/.test(s)) {
    return '链路没连上（串口没插 / 车没上电）—— 去「设置」页连上串口，再按一次「烧写到车」。'
  }
  if (/没收到车的帧|车没在发/.test(s)) {
    return '连上了但收不到车的帧 —— 车可能没上电、或没跑这份固件；去「概览」页看链路那一栏，正常了再烧。'
  }
  if (/忙|正在跑/.test(s)) {
    return '车上正在跑任务（跑着的时候车上不收烧写和删除）—— 先按「停车」，再烧写。'
  }
  if (/数据块?不连续|收尾帧|没等到数据/.test(s)) {
    return '读目录 / 回读的时候丢了一片 —— 再按一次「读目录」或「烧写到车」（桥自己会重读几遍，还不行就重启一下桥）。'
  }
  if (/没人应答|没应答|没等到 ACK|超时/.test(s)) {
    return '这一帧没等到车的回话 —— 多半只是丢了，直接再按一次「烧写到车」；连着失败就重启一下桥。'
  }
  if (/槽号是空的|车上没有任务/.test(s)) {
    return '车上这个槽是空的（这条任务还没写进去）—— 先按「烧写到车」。'
  }
  if (/一片最多|十六进制|不是 JSON|HTTP \d/.test(s)) {
    return `上位机内部没对上（${s}）—— 把这句话原样发我。`
  }
  return s
}
