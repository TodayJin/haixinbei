/**
 * 契约自检：拿真实的假后端（mock/server.mjs）当被测对象，验证前端类型假设与契约一致。
 *
 *   node mock/server.mjs &      # 或 npm run mock
 *   npx vitest run src/lib/contract.test.ts
 *
 * mock 没起来就整体 skip（不让 CI 因为"没开服务"变红）。这层检查的价值：
 *  §3.x 的字段名/单位、§4 的 SSE 事件名、错误体 `{ok:false,error,code}` 的形状 —— 
 *  只要桥实现和 API.md 有一点对不上，这里就会失败。
 */
import { beforeAll, describe, expect, it } from 'vitest'

const BASE = process.env.CONSOLE_BASE || 'http://127.0.0.1:8770'
let live = false

async function getJson(path: string, init?: RequestInit) {
  const res = await fetch(`${BASE}${path}`, init)
  return { status: res.status, body: await res.json() }
}

beforeAll(async () => {
  try {
    const res = await fetch(`${BASE}/api/health`, { signal: AbortSignal.timeout(1500) })
    live = res.ok
  } catch {
    live = false
  }
  if (!live) console.warn(`[contract] ${BASE} 没起来，跳过契约自检（先跑 node mock/server.mjs）`)
})

describe('契约自检（需要 mock 或桥在 127.0.0.1:8770）', () => {
  it('health/link 字段形状', async () => {
    if (!live) return
    const h = await getJson('/api/health')
    expect(h.body.ok).toBe(true)
    expect(typeof h.body.version).toBe('string')
    expect(typeof h.body.uptime_ms).toBe('number')

    const l = await getJson('/api/link')
    for (const k of ['mode', 'connected', 'baud', 'stats', 'telem_hz', 'fw']) expect(l.body).toHaveProperty(k)
    for (const k of ['rx', 'tx', 'crc', 'resync', 'drops']) expect(l.body.stats).toHaveProperty(k)
    expect(l.body.fw).toHaveProperty('caps')
  })

  it('params 覆盖三个环 + 串级，且字段齐全', async () => {
    if (!live) return
    const { body } = await getJson('/api/params')
    expect(body.ok).toBe(true)
    expect(body.items.length).toBeGreaterThanOrEqual(30)
    const groups = new Set(body.items.map((p: { group: string }) => p.group))
    for (const g of ['速度环', '角度环', '循迹环', '串级']) expect([...groups]).toContain(g)
    for (const p of body.items) {
      for (const k of ['id', 'key', 'label', 'group', 'unit', 'value', 'default', 'min', 'max', 'decimals', 'step', 'dirty']) {
        expect(p).toHaveProperty(k)
      }
      expect(p.value).toBeGreaterThanOrEqual(p.min)
      expect(p.value).toBeLessThanOrEqual(p.max)
    }
  })

  it('写操作越界时返回 {ok:false,error,code}', async () => {
    if (!live) return
    const { status, body } = await getJson('/api/params/set', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ key: 'line_kp', value: 999999 }),
    })
    expect(status).toBeGreaterThanOrEqual(400)
    expect(body.ok).toBe(false)
    expect(typeof body.error).toBe('string')
    expect(body.error.length).toBeGreaterThan(0)
    expect(typeof body.code).toBe('string')
  })

  it('14 个动作的名字/角度与固件一致（抽查 1/5/9/14）', async () => {
    if (!live) return
    const { body } = await getJson('/api/actions')
    expect(body.items.length).toBe(14)
    const by = Object.fromEntries(body.items.map((a: { n: number }) => [a.n, a]))
    expect([by[1].name, by[1].a1, by[1].a2]).toEqual(['收回', 0, 0])
    expect([by[5].name, by[5].a1, by[5].a2]).toEqual(['台阶2收回', 80, 170])
    expect([by[9].name, by[9].a1, by[9].a2]).toEqual(['三级3打', 100, 80])
    expect([by[14].name, by[14].a1, by[14].a2]).toEqual(['打下准备(另一路)', 30, 60])
    expect(by[5].compound).toBe(true)
    expect(by[9].compound).toBe(true)
  })

  it('/api/recordings 列回放文件，connect replay 收文件和倍速', async () => {
    if (!live) return
    const r = await getJson('/api/recordings')
    expect(r.body.ok).toBe(true)
    expect(typeof r.body.dir).toBe('string')
    expect(r.body.items.length).toBeGreaterThan(0)
    for (const k of ['name', 'path', 'size', 'mtime_ms']) expect(r.body.items[0]).toHaveProperty(k)

    // mode 不对必须明说失败（这条与"上次连过什么"无关，是确定性的）
    const bogus = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'bogus' }),
    })
    expect(bogus.body.ok).toBe(false)

    // 回放不带文件：mock/桥会复用"上次那个文件"（`lastReplay`），所以**不能**断言一定失败 ——
    // 长跑的 mock 进程在上一次用例里刚连过回放时这里就是成功的。
    // 要么明说是哪出了问题，要么真的连上回放，二者必居其一。
    const bad = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'replay' }),
    })
    if (bad.body.ok) expect(bad.body.link.mode).toBe('replay')
    else expect(String(bad.body.error)).toMatch(/回放|文件/)

    const ok = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'replay', replay: r.body.items[0].path, speed: 2 }),
    })
    if (ok.body.ok) {
      expect(ok.body.link.mode).toBe('replay')
    } else {
      // 真桥上 mock 的假路径确实不存在：失败也必须是明说的失败
      expect(String(ok.body.error)).toMatch(/回放|不存在/)
    }

    // 收尾切回模拟器：后面的用例还指望链路是 sim
    const back = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'sim' }),
    })
    expect(back.body.link.mode).toBe('sim')
  })

  it('SSE 先发 state，并能在 3s 内收到 telem', async () => {
    if (!live) return
    const res = await fetch(`${BASE}/api/stream`, { signal: AbortSignal.timeout(3000) })
    expect(res.status).toBe(200)
    const reader = res.body?.getReader()
    expect(reader).toBeTruthy()
    const dec = new TextDecoder()
    let text = ''
    const deadline = Date.now() + 3000
    while (Date.now() < deadline && !(text.includes('event: state') && text.includes('event: telem'))) {
      const { value, done } = await reader!.read()
      if (done) break
      text += dec.decode(value, { stream: true })
    }
    expect(text).toContain('event: state')
    expect(text).toContain('event: telem')
    // state 里的四块都要在
    for (const k of ['"link"', '"params"', '"actions"', '"presets"', '"cal"']) expect(text).toContain(k)
    await reader!.cancel()
  })
})
