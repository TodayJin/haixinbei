/**
 * 回归测试：设置页「回放录制」这条路。
 *
 * 2026-10-03 的真问题：契约（`API.md`）和桥里一直有 `mode:"replay"`，但前端没有任何
 * 调用者（`doConnect('replay')` 是死代码），`/api/link/connect` 也不收文件路径
 * ⇒ 浏览器里根本没法回放录下来的 jsonl。这里把两条路径钉死。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { store } from './store'

type Seen = { url: string; method: string; body: Record<string, unknown> | null }

const REC = {
  name: 'rec_20261003_101500.jsonl',
  path: 'D:/car/tools/console/data/logs/rec_20261003_101500.jsonl',
  size: 184320,
  mtime_ms: 1_759_000_000_000,
}

function stubFetch(seen: Seen[]): void {
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string, init?: RequestInit) => {
      const u = String(url)
      seen.push({
        url: u,
        method: String(init?.method ?? 'GET'),
        body: init?.body ? (JSON.parse(String(init.body)) as Record<string, unknown>) : null,
      })
      let payload: unknown = { ok: true }
      if (u.includes('/api/recordings')) payload = { ok: true, dir: 'D:/car/.../logs', items: [REC] }
      else if (u.includes('/api/link/connect')) payload = { ok: true, link: { mode: 'replay', connected: true } }
      else if (u.includes('/api/params')) payload = { ok: true, param_crc: 0, source: 'local', items: [] }
      else if (u.includes('/api/actions')) payload = { ok: true, items: [] }
      else if (u.includes('/api/presets')) payload = { ok: true, items: [] }
      else if (u.includes('/api/record')) payload = { ok: true, record: { ok: true, recording: false } }
      else if (u.includes('/api/link')) payload = { ok: true, mode: 'sim', connected: true }
      return new Response(JSON.stringify(payload), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      })
    }),
  )
}

describe('回放录制（设置页）', () => {
  beforeEach(() => {
    // 这个工程故意不装 jsdom（只允许 react/react-dom/uplot/lucide-react 运行时依赖），
    // 而 `store.toast()` 会调 `window.setTimeout` 来自动消失 ⇒ 给个最小替身。
    vi.stubGlobal('window', { setTimeout: () => 0, clearTimeout: () => {} })
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  it('listRecordings 把 /api/recordings 的 items 写进快照', async () => {
    const seen: Seen[] = []
    stubFetch(seen)
    expect(store.getSnapshot().recordings).toEqual([])

    await store.listRecordings()

    expect(store.getSnapshot().recordings).toEqual([REC])
    expect(seen.map((s) => s.url)).toContain('/api/recordings')
  })

  it('connect("replay") 把文件路径和倍速一起发给桥', async () => {
    const seen: Seen[] = []
    stubFetch(seen)

    await store.connect('replay', undefined, undefined, REC.path, 4)

    const call = seen.find((s) => s.url.includes('/api/link/connect'))
    expect(call, '必须真的请求 /api/link/connect').toBeTruthy()
    expect(call!.method).toBe('POST')
    // JSON.stringify 会把 undefined 的 port 丢掉，桥那边按缺省处理
    expect(call!.body).toEqual({ mode: 'replay', baud: 115200, replay: REC.path, speed: 4 })
  })
})
