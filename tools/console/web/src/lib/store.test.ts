/**
 * 回归测试：在浏览器里改参数必须同步进桥。
 *
 * 2026-10-03 的真 bug：`store.setParam()` 只改本地快照、**从不调 `api.paramSet()`**，
 * 于是桥那边的脏项永远是空的 ⇒「全部下发（N）」按钮空转，浏览器里改的参数根本到不了车。
 * 这里用假 fetch 拦住对桥的请求，把这条路径钉死。
 * （桥侧的语义由 `tools/console/tests/test_api.py` 的
 * `test_param_set_push_reload_defaults` 覆盖：set 只改缓存、push 才下发。）
 */
import { afterEach, describe, expect, it, vi } from 'vitest'
import { store } from './store'
import type { ParamItem, ParamsResponse } from './types'

const LINE_KP: ParamItem = {
  id: 513,
  key: 'line_kp',
  label: '循迹 Kp',
  group: '循迹环',
  unit: '',
  value: 2700,
  default: 2700,
  min: 0,
  max: 20000,
  decimals: 0,
  step: 10,
  dirty: false,
}

type Seen = { url: string; method: string; body: unknown }

function stubFetch(seen: Seen[]): void {
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string, init?: RequestInit) => {
      seen.push({
        url: String(url),
        method: String(init?.method ?? 'GET'),
        body: init?.body ? JSON.parse(String(init.body)) : null,
      })
      const payload: ParamsResponse | { ok: true } = String(url).includes('/api/params/set')
        ? { ok: true }
        : { ok: true, param_crc: 4660, source: 'local', items: [{ ...LINE_KP }] }
      return new Response(JSON.stringify(payload), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      })
    }),
  )
}

describe('参数编辑要同步进桥（不能只改本地快照）', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  it('setParam 本地立刻生效且标脏，并把值 POST 到 /api/params/set', async () => {
    const seen: Seen[] = []
    stubFetch(seen)
    await store.refreshParams()
    expect(store.getSnapshot().params?.items[0].value).toBe(2700)

    store.setParam('line_kp', 1234) // 同步入口：界面立刻变，同步桥发生在后台

    const after = store.getSnapshot().params!.items[0]
    expect(after.value).toBe(1234)
    expect(after.dirty).toBe(true) // 还没下发 ⇒ 脏（等「全部下发」）

    await vi.waitFor(() => expect(seen.length).toBe(2))
    expect(seen[1].url).toContain('/api/params/set')
    expect(seen[1].method).toBe('POST')
    expect(seen[1].body).toEqual({ key: 'line_kp', value: 1234 })
  })

  it('值改回默认就不算脏', async () => {
    const seen: Seen[] = []
    stubFetch(seen)
    await store.refreshParams()

    store.setParam('line_kp', 9999)
    expect(store.getSnapshot().params!.items[0].dirty).toBe(true)
    store.setParam('line_kp', 2700)
    expect(store.getSnapshot().params!.items[0].dirty).toBe(false)

    await vi.waitFor(() => expect(seen.length).toBe(3))
    expect(seen.map((s) => s.url)).toEqual([
      '/api/params',
      '/api/params/set',
      '/api/params/set',
    ])
  })
})
