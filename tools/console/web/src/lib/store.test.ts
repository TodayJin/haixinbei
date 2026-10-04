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

/**
 * 用户 m24215：「每次操控就会弹出一个后端报错，什么 HTTP002。200。每次都是」。
 *
 * 两个毛病叠在一起：①桥对语义性失败回 HTTP 200 且**不带 error**，前端兜底就把状态码
 * 当错误信息 ⇒「请求失败（HTTP 200）」；②操控是 10Hz 连续下发（转向每 100ms 一帧），
 * 车一拒绝就是 10 条/秒的弹幕。这里把文案和去重都钉住。
 */
describe('操控报错的文案与弹幕', () => {
  /** 桥回 200 + {ok:false}（语义性失败的正常形状）。 */
  function stubReply(payload: Record<string, unknown>): void {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => new Response(JSON.stringify(payload), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      })),
    )
  }

  const clearToasts = (): void => {
    for (const t of store.getSnapshot().toasts) store.dismissToast(t.id)
  }

  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
    clearToasts()
  })

  it('车拒绝但后端没给 error ⇒ 说「车上拒绝了：未就绪」，不许出现 HTTP 200', async () => {
    stubReply({ ok: false, sent: true, status: 5, status_name: '未就绪', answered: true })
    await store.drive({ mode: 'base_turn', base: 0, turn: 0 })

    const t = store.getSnapshot().toasts
    expect(t.length).toBe(1)
    expect(t[0].kind).toBe('err')
    expect(t[0].text).toContain('车上拒绝了：未就绪')
    expect(t[0].text).not.toContain('HTTP')
  })

  it('链路没应答时也给人话，不说 HTTP 200', async () => {
    stubReply({ ok: false, sent: true, status: null, status_name: '无应答', answered: false })
    await store.drive({ mode: 'base_turn', base: 0, turn: 0 })

    const t = store.getSnapshot().toasts
    expect(t.length).toBe(1)
    expect(t[0].text).toContain('车上没应答')
    expect(t[0].text).not.toContain('HTTP')
  })

  it('连续下发时同一句话只弹一条（10Hz 不刷屏）', async () => {
    stubReply({ ok: false, sent: true, status: 7, status_name: '忙', answered: true })
    for (let i = 0; i < 5; i++) await store.drive({ mode: 'base_turn', base: 0, turn: 0 })

    const errs = store.getSnapshot().toasts.filter((t) => t.kind === 'err')
    expect(errs.length).toBe(1)
    expect(errs[0].text).toContain('车上拒绝了：忙')
  })

  it('成功不再每帧弹提示，只有停车给一条确认', async () => {
    stubReply({ ok: true, seq: 1, sent: true, status: 0, status_name: 'OK', answered: true })
    for (let i = 0; i < 3; i++) await store.drive({ mode: 'base_turn', base: 0, turn: 0 })
    expect(store.getSnapshot().toasts).toEqual([])

    await store.drive({ mode: 'off' })
    const toasts = store.getSnapshot().toasts
    expect(toasts.length).toBe(1)
    expect(toasts[0].kind).toBe('ok')
    expect(toasts[0].text).toContain('已停车')
  })
})
