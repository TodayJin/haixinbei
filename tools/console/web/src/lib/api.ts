/**
 * 后端 API 客户端（契约见 `tools/console/docs/API.md`）。
 * 只用相对路径 `/api/...` —— 成品由桥托管在同源下。
 *
 * 错误处理约定（契约 §1）：失败时后端给 4xx/5xx + `{"ok":false,"error":"人话错误","code":"..."}`。
 * 这里把 `error` 原样抛出去，界面直接显示，不做二次翻译。
 */

import type {
  AckItem,
  ActionsResponse,
  CalResponse,
  HealthInfo,
  LinkInfo,
  LogItem,
  ParamsResponse,
  PresetItem,
  RecItem,
  RecordStatus,
  StatsEvent,
  Telem,
} from './types'

export class ApiError extends Error {
  readonly code: string
  readonly status: number
  constructor(message: string, code = 'http_error', status = 0) {
    super(message)
    this.name = 'ApiError'
    this.code = code
    this.status = status
  }
}

function qs(params: Record<string, string | number | boolean | undefined>): string {
  const parts: string[] = []
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined) continue
    parts.push(`${encodeURIComponent(k)}=${encodeURIComponent(String(v))}`)
  }
  return parts.length ? `?${parts.join('&')}` : ''
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response
  try {
    res = await fetch(path, {
      ...init,
      headers: { 'Content-Type': 'application/json', ...(init?.headers ?? {}) },
    })
  } catch (e) {
    // 网络层失败（桥没起来 / 端口不通）
    throw new ApiError(`连不上后端（${path}）：${(e as Error).message || '网络错误'}`, 'offline')
  }

  const text = await res.text()
  let body: unknown = null
  if (text) {
    try {
      body = JSON.parse(text)
    } catch {
      body = null
    }
  }
  const obj = (body ?? {}) as Record<string, unknown>

  if (!res.ok || obj.ok === false) {
    const msg = typeof obj.error === 'string' ? obj.error : `请求失败（HTTP ${res.status}）`
    const code = typeof obj.code === 'string' ? obj.code : 'http_error'
    throw new ApiError(msg, code, res.status)
  }
  return obj as T
}

function post<T>(path: string, payload?: unknown): Promise<T> {
  return request<T>(path, { method: 'POST', body: JSON.stringify(payload ?? {}) })
}

/**
 * 把后端给的预设列表归一成 `PresetItem`。
 *
 * 预设目录里不只住着 `preset_save()` 写的预设，还有动作表存档 `actions.json`
 * （形状 `{"actions":[[a1,a2]…]}`，**没有 kind/keys**）。缺 `keys` 时页面读
 * `p.keys.length` 会抛 TypeError，React 会把整棵树卸掉 ⇒ 浏览器里就是一片空白。
 * 桥侧已按契约归一（`bridge/state.py` 的 `_normalize_preset`），这里再兜一层：
 * 手写的 / 旧版本的 json 也不该能把某个页面整个打黑。
 */
export function normalizePresets(items: unknown): PresetItem[] {
  if (!Array.isArray(items)) return []
  return items.map((raw, i) => {
    const o = (raw ?? {}) as Partial<PresetItem>
    const kind = o.kind
    return {
      name: typeof o.name === 'string' && o.name ? o.name : `preset-${i + 1}`,
      kind: kind === 'actions' || kind === 'params' || kind === 'servo' || kind === 'mixed' ? kind : 'mixed',
      updated_ms: typeof o.updated_ms === 'number' ? o.updated_ms : 0,
      keys: Array.isArray(o.keys) ? o.keys.filter((k): k is string => typeof k === 'string') : [],
      note: typeof o.note === 'string' ? o.note : '',
    }
  })
}

export const api = {
  health: () => request<HealthInfo>('/api/health'),
  link: () => request<LinkInfo>('/api/link'),
  ports: () => request<{ ok: true; ports: { port: string; desc?: string }[] }>('/api/ports'),
  /** 波特率默认 921600（固件 2026-10 起 LPUART1 就是这个）；sim/replay 用不到但照样带上 */
  connect: (mode: 'serial' | 'sim' | 'replay', port?: string, baud = 921600, replay?: string, speed?: number) =>
    post<{ ok: true; link: LinkInfo }>('/api/link/connect', { mode, port, baud, replay, speed }),
  disconnect: () => post<{ ok: true }>('/api/link/disconnect'),

  params: () => request<ParamsResponse>('/api/params'),
  paramSet: (key: string, value: number) => post<{ ok: true }>('/api/params/set', { key, value }),
  paramsPush: (keys?: string[]) => post<{ ok: true }>('/api/params/push', { keys }),
  paramsSave: () => post<{ ok: true }>('/api/params/save'),
  paramsReload: () => post<{ ok: true }>('/api/params/reload'),
  paramsDefaults: (keys?: string[]) => post<{ ok: true }>('/api/params/defaults', { keys }),

  actions: () => request<ActionsResponse>('/api/actions'),
  actionSet: (n: number, a1: number, a2: number) =>
    post<{ ok: true }>('/api/actions/set', { n, a1, a2 }),
  actionSave: () => post<{ ok: true }>('/api/actions/save'),
  actionPlay: (n: number) => post<{ ok: true }>('/api/actions/play', { n }),

  servo: (ch: 1 | 2, angle: number) => post<{ ok: true }>('/api/servo', { ch, angle }),
  servoCenter: () => post<{ ok: true }>('/api/servo/center'),
  servoDeviation: (ch: 1 | 2, dev_us: number) =>
    post<{ ok: true }>('/api/servo/deviation', { ch, dev_us }),

  drive: (req: { mode: 'off' | 'wheels' | 'base_turn'; vl?: number; vr?: number; base?: number; turn?: number }) =>
    post<{ ok: true }>('/api/drive', req),
  cmd: (cmd: string, arg?: number) => post<{ ok: true }>('/api/cmd', { cmd, arg }),

  log: (limit = 200) => request<{ ok: true; items: LogItem[] }>(`/api/log${qs({ limit })}`),
  cal: () => request<CalResponse>('/api/cal'),

  presets: async () => {
    const r = await request<{ ok: true; items: PresetItem[] }>('/api/presets')
    return { ...r, items: normalizePresets(r.items) }
  },
  presetSave: (name: string, targets: string[], note?: string) =>
    post<{ ok: true }>('/api/presets', { name, targets, note }),
  presetApply: (name: string, targets: string[]) => post<{ ok: true }>('/api/presets/apply', { name, targets }),
  presetDelete: (name: string) =>
    request<{ ok: true }>(`/api/presets${qs({ name })}`, { method: 'DELETE' }),

  record: (action: 'start' | 'stop') => post<{ ok: true; record: RecordStatus }>('/api/record', { action }),
  recordStatus: () => request<{ ok: true; record: RecordStatus }>('/api/record'),
  /** 能拿来回放的录制文件（`data/logs/rec_*.jsonl`） */
  recordings: () => request<{ ok: true; dir: string; items: RecItem[] }>('/api/recordings'),
}

/** 打开 SSE 流（断线自动重连由浏览器 EventSource 自己做） */
export function openStream(handlers: {
  onState: (payload: unknown) => void
  onTelem: (t: Telem) => void
  onStats: (s: StatsEvent) => void
  onLog: (item: LogItem) => void
  onAck: (item: AckItem) => void
  onActions: (a: ActionsResponse) => void
  onParams: (p: ParamsResponse) => void
  onCal: (c: CalResponse) => void
  onError: (msg: string) => void
  onLinkChange: (state: 'connecting' | 'open' | 'closed') => void
}): EventSource {
  const es = new EventSource('/api/stream')
  es.addEventListener('open', () => handlers.onLinkChange('open'))
  // 注意：浏览器自己的连接错误也叫 `error`，和后端的 `event: error` 同名 ——
  // 有 data 的才是后端发来的（契约 §4 最后一行）。
  es.addEventListener('error', (e) => {
    const data = (e as MessageEvent).data
    if (typeof data === 'string' && data.length > 0) {
      try {
        handlers.onError((JSON.parse(data) as { text?: string }).text ?? '后端报错')
      } catch {
        handlers.onError('后端报错')
      }
    } else {
      handlers.onLinkChange('closed')
    }
  })
  es.addEventListener('state', (e) => handlers.onState(JSON.parse((e as MessageEvent).data)))
  es.addEventListener('telem', (e) => handlers.onTelem(JSON.parse((e as MessageEvent).data) as Telem))
  // 桥按 sse_batch_ms 攒批：连续遥测会一次发来 N 帧（契约 §4 的 `telem_batch`），
  // 逐帧喂同一个 onTelem —— 上层（ring 缓冲 + 50ms 合并渲染）对此完全无感。
  es.addEventListener('telem_batch', (e) => {
    const arr = JSON.parse((e as MessageEvent).data) as Telem[]
    for (const t of arr) handlers.onTelem(t)
  })
  es.addEventListener('stats', (e) => handlers.onStats(JSON.parse((e as MessageEvent).data) as StatsEvent))
  es.addEventListener('log', (e) => handlers.onLog(JSON.parse((e as MessageEvent).data) as LogItem))
  es.addEventListener('ack', (e) => handlers.onAck(JSON.parse((e as MessageEvent).data) as AckItem))
  es.addEventListener('actions', (e) => handlers.onActions(JSON.parse((e as MessageEvent).data) as ActionsResponse))
  es.addEventListener('params', (e) => handlers.onParams(JSON.parse((e as MessageEvent).data) as ParamsResponse))
  es.addEventListener('cal', (e) => handlers.onCal(JSON.parse((e as MessageEvent).data) as CalResponse))
  return es
}
