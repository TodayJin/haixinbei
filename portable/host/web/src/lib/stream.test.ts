/** 钉住「桥侧 SSE 攒批」在前端这一侧的接头。
 *
 * 桥开 `--sse-batch-ms`（默认 20ms）后，连续遥测会走 `telem_batch` 事件一次送来
 * N 帧；前端必须逐帧喂 `onTelem`，否则浏览器一帧遥测都收不到（静默全瞎，而且
 * 界面只是一片空白、不报错）。这里用假 EventSource 把事件名到处理器的映射钉死：
 * 桥发什么名字、前端就必须有对应的 listener。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'

import { openStream } from './api'
import type { Telem } from './types'

class FakeES {
  listeners = new Map<string, ((e: { data?: string }) => void)[]>()
  constructor(public url: string) {}

  addEventListener(name: string, fn: (e: { data?: string }) => void) {
    const arr = this.listeners.get(name) ?? []
    arr.push(fn)
    this.listeners.set(name, arr)
  }

  /** 假装服务器推了一条 `event: <name>`。 */
  push(name: string, payload: unknown) {
    const fns = this.listeners.get(name) ?? []
    expect(fns.length, `前端没有监听 ${name} 事件`).toBeGreaterThan(0)
    for (const fn of fns) fn({ data: JSON.stringify(payload) })
  }

  names() {
    return [...this.listeners.keys()]
  }
}

function mkHandlers() {
  return {
    onState: vi.fn(),
    onTelem: vi.fn(),
    onStats: vi.fn(),
    onLog: vi.fn(),
    onAck: vi.fn(),
    onActions: vi.fn(),
    onParams: vi.fn(),
    onCal: vi.fn(),
    onRoute: vi.fn(),
    onError: vi.fn(),
    onLinkChange: vi.fn(),
  }
}

function mkStream() {
  const h = mkHandlers()
  const es = openStream(h) as unknown as FakeES
  return { h, es }
}

const frame = (i: number) => ({ t: i * 10, yaw: i }) as unknown as Telem

afterEach(() => vi.unstubAllGlobals())

describe('openStream 的事件接线', () => {
  it('telem_batch 一次 N 帧要逐帧喂 onTelem（顺序不变）', () => {
    vi.stubGlobal('EventSource', FakeES)
    const { h, es } = mkStream()

    es.push('telem_batch', [frame(1), frame(2), frame(3)])

    expect(h.onTelem).toHaveBeenCalledTimes(3)
    expect(h.onTelem.mock.calls.map((c) => (c[0] as Telem).yaw)).toEqual([1, 2, 3])
  })

  it('telem 单帧和 telem_batch 走同一条路（不合并时也不瞎）', () => {
    vi.stubGlobal('EventSource', FakeES)
    const { h, es } = mkStream()

    es.push('telem', frame(7))

    expect(h.onTelem).toHaveBeenCalledTimes(1)
    expect((h.onTelem.mock.calls[0][0] as Telem).yaw).toBe(7)
  })

  it('每种事件名都接到自己的处理器（契约 §4）', () => {
    vi.stubGlobal('EventSource', FakeES)
    const { h, es } = mkStream()

    es.push('state', { ok: true })
    es.push('stats', { stats: { rx: 1 } })
    es.push('log', { text: 'hi' })
    es.push('ack', { seq: 1, req: 'cmd' })
    es.push('actions', { ok: true, items: [] })
    es.push('params', { ok: true, items: [] })
    es.push('cal', { ok: true })

    expect(h.onState).toHaveBeenCalledTimes(1)
    expect(h.onStats).toHaveBeenCalledTimes(1)
    expect(h.onLog).toHaveBeenCalledTimes(1)
    expect(h.onAck).toHaveBeenCalledTimes(1)
    expect(h.onActions).toHaveBeenCalledTimes(1)
    expect(h.onParams).toHaveBeenCalledTimes(1)
    expect(h.onCal).toHaveBeenCalledTimes(1)
    expect(es.names()).toContain('telem_batch')
  })

  it('浏览器自身的连接错误（无 data）只算断线；带 data 才当后端报错', () => {
    vi.stubGlobal('EventSource', FakeES)
    const { h, es } = mkStream()

    const errFn = h.onError
    for (const fn of es.listeners.get('error') ?? []) fn({})            // 无 data
    expect(h.onLinkChange).toHaveBeenLastCalledWith('closed')
    expect(errFn).not.toHaveBeenCalled()

    for (const fn of es.listeners.get('error') ?? []) fn({ data: '{"text":"炸了"}' })
    expect(h.onError).toHaveBeenCalledWith('炸了')
  })
})
