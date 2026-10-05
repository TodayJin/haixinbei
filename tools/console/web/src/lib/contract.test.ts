/**
 * 契约自检：拿真实的假后端（mock/server.mjs）当被测对象，验证前端类型假设与契约一致。
 *
 *   node mock/server.mjs &      # 或 npm run mock
 *   npx vitest run src/lib/contract.test.ts
 *
 * mock 没起来就整体 skip（不让 CI 因为"没开服务"变红）。这层检查的价值：
 *  §3.x 的字段名/单位、§4 的 SSE 事件名、错误体 `{ok:false,error,code}` 的形状 —— 
 *  只要桥实现和 API.md 有一点对不上，这里就会失败。
 *
 * **必须显式给 `CONSOLE_BASE` 才跑**（2026-10-05 修）：默认那个 127.0.0.1:8770 上很可能
 * 正连着真车，而本文件会发**写操作**（连串口、烧任务、运行任务）—— 那天就差点在真车上
 * 把任务跑起来（任务被车以 status=3 拒了才没跑）。要跑：
 *   `PORT=8790 node mock/server.mjs` + `CONSOLE_BASE=http://127.0.0.1:8790 npx vitest run src/lib/contract.test.ts`
 */
import { beforeAll, describe, expect, it } from 'vitest'

const BASE = process.env.CONSOLE_BASE || 'http://127.0.0.1:8770'
/** 只有显式指定 mock 的地址才真的跑；没给就 skip，绝不去碰 8770 上的真车桥 */
const OPT_IN = !!process.env.CONSOLE_BASE
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

describe.skipIf(!OPT_IN)('契约自检（需要 mock 桥：设 CONSOLE_BASE）', () => {
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

  it('connect 只认 sim/serial：模式不对必须明说失败', async () => {
    if (!live) return
    // mode 不对必须明说失败（这条与"上次连过什么"无关，是确定性的）
    const bogus = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'bogus' }),
    })
    expect(bogus.body.ok).toBe(false)
    // 回放模式 2026-10-04 已按用户要求删除：再喊 "replay" 必须是明说的失败，不能连上什么
    const gone = await getJson('/api/link/connect', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ mode: 'replay' }),
    })
    expect(gone.body.ok).toBe(false)
    expect(String(gone.body.error)).toMatch(/sim|serial/)

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

  // 任务页（#/tasks）：假后端也按 API.md §3.6 实现了 /api/tasks*，所以这里能用**真 HTTP** 走一遍
  // 「烧写 → 目录 → 回读逐字节 → 运行 → 状态 → 删掉」——只不过用的是假 Flash。
  // 这条是真 HTTP（十来次往返），跑全量时其它文件在并行编译，5s 默认超时不够 ⇒ 单独给 30s。
  it('任务：烧写/回读/运行/删除整条闭环（走真 HTTP）', async () => {
    if (!live) return
    const { createTaskApi } = await import('./taskapi')
    const { newTaskDef } = await import('./taskedit')
    const { encodeTask, crc16Ccitt } = await import('./task')
    const api = createTaskApi((u, init) => fetch(`${BASE}${u}`, init))

    const prog = newTaskDef(0, '契约自检').prog
    const bytes = encodeTask(prog).bytes
    const crc = crc16Ccitt(bytes.subarray(12))
    const id = 6                       // 车上 8 个槽位：0..7

    const burn = await api.burn(id, bytes, bytes.length, crc, '契约自检', 0x81)  // flags 是保留位
    expect(burn.ok, JSON.stringify(burn)).toBe(true)

    const list = await api.list()
    expect(list.ok).toBe(true)
    if (list.ok) {
      expect(list.data.supported).toBe(true)
      const mine = list.data.items.find((x) => x.id === id)
      expect(mine?.len).toBe(bytes.length)
      expect(mine?.crc).toBe(crc)
      expect(mine?.name).toBe('契约自检')
      expect(mine?.flags).toBe(0x81)   // flags 原样落在目录记录里（曾经被掩成 bit0，m24999 删了那个功能）
    }

    const back = await api.get(id)
    expect(back.ok).toBe(true)
    if (back.ok) {
      expect(Array.from(back.data)).toEqual(Array.from(bytes))   // 逐字节
      expect(crc16Ccitt(back.data.subarray(12))).toBe(crc)       // 车上算的 CRC 也一样
    }

    const run = await api.run(id)
    expect(run.ok, JSON.stringify(run)).toBe(true)
    const st = await api.status()
    expect(st.ok).toBe(true)
    if (st.ok && st.data) {
      expect(st.data.id).toBe(id)
      expect(st.data.running).toBe(true)   // 线上是 u8 1/0，taskapi 归一成布尔（页面按布尔用）
      expect(st.data.state).toBe(0)        // newTaskDef 的主页是 0
      expect(st.data.elapsedMs).toBeGreaterThanOrEqual(0)
    }
    const stop = await api.stop()
    expect(stop.ok).toBe(true)

    // CRC 报错时必须被桥上拦住（烧半个程序的经典事故）：先按 120 B 分片 put 一遍让桥上有暂存，
    // 再用错 CRC 提交 —— 顺带验证分片 offset 与 mock 的 "一帧最多 120 字节" 限制一致。
    const { chunkImage } = await import('./taskapi')
    for (const c of chunkImage(bytes)) {
      const p = await api.put(id, c.offset, c.hex)
      expect(p.ok, JSON.stringify(p)).toBe(true)
    }
    const bad = await api.commit(id, bytes.length, (crc ^ 0x1234) & 0xffff, '坏 CRC')
    expect(bad.ok).toBe(false)
    if (!bad.ok) expect(bad.error).toMatch(/CRC/)

    const del = await api.del(id)
    expect(del.ok).toBe(true)
  }, 30000)
})
