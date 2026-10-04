/**
 * `#/tasks` 页访问桥的 `/api/tasks*` 端点。
 *
 * **现状（诚实说明）**：桥里还没有这些端点（`bridge/server.py` 只到参数/动作/标定/回放），
 * 固件的 0x60–0x69 帧也还没实现 —— 这是 `docs/任务编程.md` §7 的第 3、4 步。
 * 所以这一层做两件事：
 *  1. 把"端点不存在"与"真出错"分开：前者回 `{ ok:false, supported:false }`，页面显示
 *     「等固件/桥支持」而不是弹一个红错误；
 *  2. 把烧写流程（分片 → 提交 → 回读校验）写成**可以用假 fetch 测**的纯循环，
 *     端点一落地就能直接用，不用等真车。
 */
import { TASK_HDR_LEN } from './task'

/** 一帧 `T_TASK_PUT` 能带的字节数（协议帧载荷 ≤120 B，含 id+off 4 B） */
export const CHUNK_BYTES = 120

export class TaskApiError extends Error {}

export interface RemoteTaskInfo {
  id: number
  name: string
  len: number
  crc: number
  flags: number
}

export interface TaskStatus {
  id: number
  state: number
  running: boolean
  block: number
  elapsedMs: number
  flags: number
}

export type Result<T> = { ok: true; data: T } | { ok: false; error: string; supported: boolean }

/**
 * 车/桥两边的字段口径不完全一样：固件推的是 `elapsed_ms`（蛇形），页面统一读 `elapsedMs`。
 * 这里把两种拼写归一，省得某一层少写一个键就在界面上显示 `undefined ms`。
 */
export function normalizeStatus(s: Partial<TaskStatus> & { elapsed_ms?: number }): TaskStatus {
  return {
    id: Number(s.id ?? 0xffff),
    state: Number(s.state ?? 0xff),
    running: !!s.running,
    block: Number(s.block ?? 0),
    elapsedMs: Number(s.elapsedMs ?? s.elapsed_ms ?? 0),
    flags: Number(s.flags ?? 0),
  }
}

export function bytesToHex(b: Uint8Array): string {
  let s = ''
  for (const x of b) s += x.toString(16).padStart(2, '0')
  return s
}

export function hexToBytes(hex: string): Uint8Array {
  const clean = hex.replace(/[^0-9a-fA-F]/g, '')
  if (clean.length % 2) throw new TaskApiError('十六进制长度是奇数')
  const out = new Uint8Array(clean.length / 2)
  for (let i = 0; i < out.length; i += 1) out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16)
  return out
}

/** 把镜像切成 ≤CHUNK_BYTES 的分片：`{offset, hex}` */
export function chunkImage(bytes: Uint8Array, size = CHUNK_BYTES): { offset: number; hex: string }[] {
  const out: { offset: number; hex: string }[] = []
  for (let off = 0; off < bytes.length; off += size) {
    out.push({ offset: off, hex: bytesToHex(bytes.subarray(off, Math.min(off + size, bytes.length))) })
  }
  return out
}

export function decodeImageHex(hex: string): Uint8Array {
  const b = hexToBytes(hex)
  if (b.length < TASK_HDR_LEN) throw new TaskApiError(`镜像太短：${b.length} B`)
  return b
}

type Fetcher = (url: string, init?: { method?: string; headers?: Record<string, string>; body?: string }) => Promise<{
  ok: boolean
  status: number
  json: () => Promise<unknown>
  text: () => Promise<string>
}>

interface Envelope {
  ok?: boolean
  error?: string
  [k: string]: unknown
}

export function createTaskApi(fetchImpl: Fetcher) {
  async function call<T>(url: string, init?: { method?: string; body?: unknown }): Promise<Result<T>> {
    let res: Awaited<ReturnType<Fetcher>>
    try {
      res = await fetchImpl(url, {
        method: init?.method ?? 'GET',
        headers: { 'content-type': 'application/json' },
        body: init?.body === undefined ? undefined : JSON.stringify(init.body),
      })
    } catch (e) {
      return { ok: false, error: `连不上桥：${(e as Error).message}`, supported: true }
    }
    if (res.status === 404 || res.status === 501) {
      return { ok: false, error: '桥还没有实现 /api/tasks* 端点（docs/任务编程.md §7 第 3 步）', supported: false }
    }
    let env: Envelope = {}
    try {
      env = (await res.json()) as Envelope
    } catch {
      return { ok: false, error: `桥回的不是 JSON（HTTP ${res.status}）`, supported: true }
    }
    if (!res.ok || env.ok === false) {
      const msg = typeof env.error === 'string' ? env.error : `HTTP ${res.status}`
      // 桥在信封里给了 supported 就用它（比抠 error 文案准）；没有才退回文案判据。
      // 固件没置 HOST_CAP_TASK 时桥会明确回 supported:false。
      const supported = typeof env.supported === 'boolean'
        ? env.supported
        : !/不支持|no cap|unsupported|任务功能/i.test(msg)
      return { ok: false, error: msg, supported }
    }
    return { ok: true, data: env as T }
  }

  const del = (id: number) => call<unknown>('/api/tasks/delete', { method: 'POST', body: { id } })
  const stop = () => call<unknown>('/api/tasks/stop', { method: 'POST', body: {} })

  /** 删车上的任务：跑着的任务固件对 DELETE 一律回「忙」（firmware/App/Svc/host_link.c:999），
   *  所以第一次失败就停车再删一次。`stopped` 告诉调用方中间停过车（好写提示文案）。
   *  单独放这里而不是塞进页面，是为了让 taskapi.test.ts 能把这段顺序钉死。 */
  const delWithStop = async (id: number): Promise<{ result: Result<unknown>; stopped: boolean }> => {
    const first = await del(id)
    if (first.ok) return { result: first, stopped: false }
    const s = await stop()
    if (!s.ok) return { result: first, stopped: false }
    return { result: await del(id), stopped: true }
  }

  return {
    /** 车上现在有哪些任务（顺带回能力位） */
    list: async (): Promise<Result<{ supported: boolean; items: RemoteTaskInfo[] }>> => {
      const r = await call<{ supported?: boolean; items?: RemoteTaskInfo[] }>('/api/tasks')
      if (!r.ok) return r
      return { ok: true, data: { supported: r.data.supported !== false, items: r.data.items ?? [] } }
    },

    get: async (id: number): Promise<Result<Uint8Array>> => {
      const r = await call<{ hex?: string; bytes?: string }>(`/api/tasks/get?id=${id}`)
      if (!r.ok) return r
      try {
        return { ok: true, data: decodeImageHex(r.data.hex ?? r.data.bytes ?? '') }
      } catch (e) {
        return { ok: false, error: (e as Error).message, supported: true }
      }
    },

    put: (id: number, offset: number, hex: string) => call<unknown>('/api/tasks/put', { method: 'POST', body: { id, offset, hex } }),

    /** 提交：桥上把 len/crc 与收到的字节核对一遍，不一致就拒（避免烧半个程序）。
     *  `flags` bit0 = 开机自启（车上自己跑这个任务，不用电脑点「开始」）。 */
    commit: (id: number, len: number, crc: number, name?: string, flags?: number) =>
      call<unknown>('/api/tasks/commit', { method: 'POST', body: { id, len, crc, name, flags } }),

    del,

    run: (id: number) => call<unknown>('/api/tasks/run', { method: 'POST', body: { id } }),

    stop,

    delWithStop,

    status: async (): Promise<Result<TaskStatus | null>> => {
      type Raw = Partial<TaskStatus> & { elapsed_ms?: number }
      const r = await call<{ status?: Raw } & Raw>('/api/tasks/status')
      if (!r.ok) return r
      const raw = r.data.status ?? (typeof r.data.state === 'number' ? r.data : null)
      return { ok: true, data: raw ? normalizeStatus(raw) : null }
    },

    /** 烧一个任务：分片 → 提交（回读校验由页面调 get 比对） */
    async burn(id: number, bytes: Uint8Array, len: number, crc: number, name?: string,
               flags?: number): Promise<Result<{ chunks: number }>> {
      const chunks = chunkImage(bytes)
      for (const c of chunks) {
        const r = await this.put(id, c.offset, c.hex)
        if (!r.ok) return r
      }
      const c = await this.commit(id, len, crc, name, flags)
      if (!c.ok) return c
      return { ok: true, data: { chunks: chunks.length } }
    },
  }
}

export type TaskApi = ReturnType<typeof createTaskApi>

export const taskApi = createTaskApi(((url: string, init?: unknown) =>
  fetch(url, init as RequestInit)) as unknown as Fetcher)
