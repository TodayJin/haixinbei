/**
 * @file  fx.test.ts
 * @brief 滚动状态机 + 低特效类：纯逻辑单测（时间源注入，不需要 DOM）
 *
 * 这里锁的是两件容易改坏的事：
 *  1. 连着滚的时候**只挂一次类**（否则每个 scroll 事件都去碰 classList）；
 *  2. 松手 `SCROLL_IDLE_MS` 之后**一定要摘掉**（摘不掉就等于低特效模式常开，玻璃拟态白丢了）。
 * 真机上的收益数字记在 lib/perf.test.ts 的账本里。
 */
import { describe, expect, it } from 'vitest'

import {
  applyLiteFx,
  createScrollFx,
  LITE_CLASS,
  SCROLLING_CLASS,
  SCROLL_IDLE_MS,
} from './fx'

/** 假定时器：手动 fire，不依赖真实时钟 */
function fakeTimers() {
  let seq = 0
  const timers = new Map<number, { fn: () => void; ms: number }>()
  return {
    setTimer: (fn: () => void, ms: number): unknown => {
      const id = ++seq
      timers.set(id, { fn, ms })
      return id
    },
    clearTimer: (h: unknown): void => {
      timers.delete(h as number)
    },
    pending: (): number => timers.size,
    /** 跑掉当前所有挂着的定时器（返回跑掉的个数） */
    runAll: (): number => {
      const all = [...timers.entries()]
      timers.clear()
      for (const [, t] of all) t.fn()
      return all.length
    },
    lastMs: (): number | null => {
      const last = [...timers.values()].pop()
      return last ? last.ms : null
    },
  }
}

function fakeTarget() {
  const set = new Set<string>()
  let adds = 0
  return {
    set,
    adds: () => adds,
    add: (n: string): void => {
      adds += 1
      set.add(n)
    },
    remove: (n: string): void => {
      set.delete(n)
    },
  }
}

describe('滚动时降特效（<html>.is-scrolling）', () => {
  it('第一次 tick 挂类，之后连着 tick 不再重复挂', () => {
    const t = fakeTimers()
    const el = fakeTarget()
    const fx = createScrollFx(el, t)

    expect(el.set.has(SCROLLING_CLASS)).toBe(false)
    fx.tick()
    fx.tick()
    fx.tick()
    expect(el.set.has(SCROLLING_CLASS)).toBe(true)
    expect(el.adds()).toBe(1) // 只碰了一次 classList
    expect(fx.scrolling).toBe(true)
  })

  it('松手 SCROLL_IDLE_MS 之后摘类，只留一个待触发的定时器', () => {
    const t = fakeTimers()
    const el = fakeTarget()
    const fx = createScrollFx(el, t)

    fx.tick()
    fx.tick()
    expect(t.pending()).toBe(1) // 后一次 tick 把前一个定时器清掉了
    expect(t.lastMs()).toBe(SCROLL_IDLE_MS)
    expect(SCROLL_IDLE_MS).toBeGreaterThan(0)

    expect(t.runAll()).toBe(1)
    expect(el.set.has(SCROLLING_CLASS)).toBe(false)
    expect(fx.scrolling).toBe(false)
  })

  it('摘掉之后还能再滚一次（状态能来回切）', () => {
    const t = fakeTimers()
    const el = fakeTarget()
    const fx = createScrollFx(el, t)

    fx.tick()
    t.runAll()
    fx.tick()
    expect(el.set.has(SCROLLING_CLASS)).toBe(true)
    expect(el.adds()).toBe(2)
    t.runAll()
    expect(el.set.has(SCROLLING_CLASS)).toBe(false)
  })

  it('dispose 会清定时器并摘类，卸载后类不会留在 <html> 上', () => {
    const t = fakeTimers()
    const el = fakeTarget()
    const fx = createScrollFx(el, t)

    fx.tick()
    expect(t.pending()).toBe(1)
    fx.dispose()
    expect(t.pending()).toBe(0)
    expect(el.set.has(SCROLLING_CLASS)).toBe(false)
    // 再 dispose 一次不该抛（组件双挂载/严格模式会走到）
    expect(() => fx.dispose()).not.toThrow()
  })
})

describe('低特效模式（<html>.perf-lite）', () => {
  it('按开关挂/摘，且开关本身不碰滚动那个类', () => {
    const el = fakeTarget()
    applyLiteFx(el, true)
    expect(el.set.has(LITE_CLASS)).toBe(true)
    expect(el.set.has(SCROLLING_CLASS)).toBe(false)
    applyLiteFx(el, false)
    expect(el.set.has(LITE_CLASS)).toBe(false)
  })

  it('两个类名不同、且都不是空串（CSS 里按名字选中）', () => {
    expect(LITE_CLASS).toBe('perf-lite')
    expect(SCROLLING_CLASS).toBe('is-scrolling')
    expect(LITE_CLASS).not.toBe(SCROLLING_CLASS)
  })
})
