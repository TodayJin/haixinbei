/**
 * ★航位推算轨迹在前端的**拼接**规则（store.onRoute）。
 *
 * 为什么要单独测：桥按 5 Hz 推**增量**（from = 这批点从第几个开始），而 SSE
 * **断线重连时桥会把 reset 事件重发一遍**。拼接写错的后果是"轨迹被画了两遍/少一段"，
 * 而看图的人完全看不出是画错了，会以为车真的那么跑。
 */
import { beforeEach, describe, expect, it } from 'vitest'
import { store } from './store'
import type { RouteEvent } from './types'

const ev = (o: Partial<RouteEvent>): RouteEvent => ({
  from: 0, pts: [], marks: [], dist_mm: 0, n: 0, head: 0, yaw0: 0, cur: [0, 0],
  ...o,
})

const pts = (s: number, e: number): [number, number][] =>
  Array.from({ length: e - s }, (_, i) => [s + i, 0] as [number, number])

describe('路线增量拼接', () => {
  beforeEach(() => {
    // 回到干净状态：一条 reset 的空轨迹
    store.feedRoute(ev({ reset: true, pts: [], marks: [], n: 0 }))
  })

  it('reset 带全量 ⇒ 整条替换（新打开的页面就是这样一次拿全的）', () => {
    store.feedRoute(ev({ reset: true, from: 0, pts: pts(0, 5), n: 5, dist_mm: 40 }))
    expect(store.getSnapshot().route?.pts.length).toBe(5)
    expect(store.getSnapshot().route?.n).toBe(5)
  })

  it('后续增量按 from 接着拼，点数对', () => {
    store.feedRoute(ev({ reset: true, from: 0, pts: pts(0, 5), n: 5 }))
    store.feedRoute(ev({ from: 5, pts: pts(5, 9), n: 9, dist_mm: 80 }))
    const r = store.getSnapshot().route
    expect(r?.pts.length).toBe(9)
    expect(r?.n).toBe(9)
    expect(r?.dist_mm).toBe(80)
  })

  it('★重连重放同一批（n 没变大）整个忽略 —— 不会把点画两遍', () => {
    store.feedRoute(ev({ reset: true, from: 0, pts: pts(0, 5), n: 5 }))
    store.feedRoute(ev({ from: 0, pts: pts(0, 5), n: 5 }))   // 重放
    expect(store.getSnapshot().route?.pts.length).toBe(5)
  })

  it('★from 往回指也安全：截断到 from 再拼，不会重复', () => {
    store.feedRoute(ev({ reset: true, from: 0, pts: pts(0, 5), n: 5 }))
    // 桥重启后重新从 from=3 推了 4 个点（n 变大）——前 3 个要保持一致
    store.feedRoute(ev({ from: 3, pts: pts(3, 7), n: 7 }))
    const p = store.getSnapshot().route?.pts ?? []
    expect(p.length).toBe(7)
    expect(p.map((q) => q[0])).toEqual([0, 1, 2, 3, 4, 5, 6])
  })

  it('打点按 i 去重（重放时不会多出一个点）', () => {
    const mk = { x: 0, y: 0, k: 'state' as const, v: 1, i: 3 }
    store.feedRoute(ev({ reset: true, from: 0, pts: pts(0, 4), marks: [mk], n: 4 }))
    store.feedRoute(ev({ from: 4, pts: pts(4, 6), marks: [mk], n: 6 }))
    expect(store.getSnapshot().route?.marks.length).toBe(1)
  })
})
