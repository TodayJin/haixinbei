/**
 * 回归测试：「界面卡不卡」的关键设计不能再退化。
 *
 * 背景：用户 m20705 说上位机「感觉卡卡的」，于是 2026-10-03 做了三件事：
 *   1. `useMeta()`：只显示参数/设置/日志的页面订阅"不含 telem"的快照，
 *      不再被 100Hz 遥测拖着整页重渲染；
 *   2. 热路径合并到 HOT_MS(50ms) = 20Hz；
 *   3. `SeriesRing.snapshotField()`：波形一条泳道只拷**一个**通道，
 *      不再每次 `snapshot()` 把 21 个通道全拷一遍（21 倍的无用拷贝）。
 *
 * 这里把这三条钉死：谁把 telem 又塞回 meta 快照、或把节流拆掉、或让泳道退回
 * 全通道拷贝，测试立刻红。真机上的帧率/CRC/延迟另有 `tools/console/tests/` 的
 * 字节级对拍与真板实测（见 memory/SESSIONS.md）。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'
import { store } from './store'
import { CHANNELS } from './channels'
import type { Telem } from './types'

const T0 = 1_700_000_000_000

/** 造一帧结构完整的遥测（字段名必须和 types.ts 的 Telem 一致） */
function mkTelem(i: number, t = T0 + i * 10): Telem {
  const f = i % 2 === 0
  return {
    t,
    t_host: t,
    yaw: 1 + i * 0.1,
    pitch: 2,
    roll: 3,
    vl: 100 + i,
    vr: 200 - i,
    pwm_l: 10,
    pwm_r: -10,
    vbat_mv: 7400,
    ibat_ma: 300,
    servo1_us: 1500,
    servo2_us: 1500,
    servo1_deg: 0,
    servo2_deg: 0,
    line: [0, 1, 1, 0, 1, 1, 0, 1],
    gray: [1, 2, 3, 4, 5, 6, 7, 8],
    state_id: 0,
    loop_us: 800 + i,
    mode: 1,
    task_id: 0,
    action: 0,
    trace: { pos: 0, pid: 0 },
    angle: { err: 0, rate_ref: 0, corr: 0 },
    flags: {
      lost: false,
      slope: false,
      gyro_ok: true,
      motor: f,
      host_drive: f,
      logging: false,
      flash: false,
      fault: false,
    },
    nvram: 0,
  }
}

describe('遥测热路径的刷新成本（用户 m20705「感觉卡卡的」）', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  it('200 帧遥测不叫醒 useMeta() 订阅者，整快照订阅者也只被叫起 1~2 次', () => {
    vi.useFakeTimers()
    let full = 0
    let meta = 0
    const offFull = store.subscribe(() => {
      full += 1
    })
    const offMeta = store.subscribeMeta(() => {
      meta += 1
    })
    try {
      for (let i = 0; i < 200; i++) store.feedTelem(mkTelem(i))
      // 关键断言：低频页面完全不吃热路径
      expect(meta).toBe(0)
      // 200 帧挤在 50ms 窗口里 ⇒ 合并成极少数次通知（绝不是 200 次）
      expect(full).toBeGreaterThanOrEqual(1)
      expect(full).toBeLessThanOrEqual(3)

      vi.advanceTimersByTime(60)
      expect(meta).toBe(0)
      expect(full).toBeLessThanOrEqual(4)
      // 但最后一帧确实进去了（合并只是少刷，不是丢数据）
      expect(store.getSnapshot().telem?.t).toBe(T0 + 199 * 10)
    } finally {
      offFull()
      offMeta()
    }
  })

  it('浏览器里热渲染挂在 rAF 上：同一帧内的遥测合并成一次，帧没到就不渲染', () => {
    const frames: FrameRequestCallback[] = []
    vi.stubGlobal('window', {
      requestAnimationFrame: (fn: FrameRequestCallback) => {
        frames.push(fn)
        return frames.length
      },
    })
    vi.useFakeTimers()
    vi.advanceTimersByTime(500) // 冲掉上一个测试遗留的节流窗口与定时器
    frames.length = 0
    let full = 0
    const off = store.subscribe(() => {
      full += 1
    })
    try {
      store.feedTelem(mkTelem(300))
      store.feedTelem(mkTelem(301))
      expect(frames.length).toBe(1) // 两帧遥测只排一次绘制
      expect(full).toBe(0) // 绘制帧还没到：一次渲染都没有
      frames.shift()!(0)
      expect(full).toBe(1) // 一帧到了，合并成一次渲染

      // 窗口内的尾帧：定时器到点后也走 rAF，而不是同步渲染
      store.feedTelem(mkTelem(302))
      vi.advanceTimersByTime(60)
      expect(full).toBe(1)
      expect(frames.length).toBe(1)
      frames.shift()!(0)
      expect(full).toBe(2)
      // 合并只是少刷，最后一帧不能丢
      expect(store.getSnapshot().telem?.t).toBe(T0 + 302 * 10)
    } finally {
      off()
      vi.unstubAllGlobals()
    }
  })

  it('meta 快照在热路径上保持同一引用（React 直接 bail out，不重渲染）', () => {
    vi.useFakeTimers()
    const before = store.getMetaSnapshot()
    store.feedTelem(mkTelem(1))
    vi.advanceTimersByTime(60)
    expect(store.getMetaSnapshot()).toBe(before)
    // 而且它里面本来就没有 telem / ringFill / version
    const keys = Object.keys(before)
    expect(keys).not.toContain('telem')
    expect(keys).not.toContain('ringFill')
    expect(keys).not.toContain('version')
  })

  it('波形单通道快照只拷一个通道，值与全量快照的同一通道一致', () => {
    const ring = store.ring
    ring.clear()
    expect(CHANNELS.length).toBeGreaterThan(1)
    for (let i = 0; i < 40; i++) {
      ring.push(T0 + i * 10, Array.from({ length: CHANNELS.length }, (_, f) => i * 100 + f))
    }
    const now = T0 + 39 * 10
    const all = ring.snapshot(10_000, now)
    const one = ring.snapshotField(10_000, 3, now)

    expect(all.count).toBe(40)
    expect(all.v.length).toBe(CHANNELS.length) // 全量：21 个通道
    expect(one.count).toBe(40)
    expect(Array.from(one.v)).toEqual(Array.from(all.v[3])) // 单通道：只拷 1 个
    expect(Array.from(one.t)).toEqual(Array.from(all.t))
    expect(Array.from(one.v).slice(0, 3)).toEqual([3, 103, 203])

    // 越界要钳位，不能崩（波形页允许改通道索引）
    expect(ring.snapshotField(10_000, 999, now).count).toBe(40)
    expect(ring.snapshotField(10_000, -5, now).v[0]).toBe(all.v[0][0])
  })
})
