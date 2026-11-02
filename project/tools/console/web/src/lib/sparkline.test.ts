/**
 * 波形"闪来闪去"的两个根因，各有一组回归测试把它钉住（用户 m22973）：
 *
 *  1. **抽稀相位**（`SeriesRing.view()`）：原来从窗口数组的 `i=0` 起步抽稀，窗口滑一格
 *     ⇒ 选中的样本整批换掉 ⇒ 折线整条抖。现在相位锚在**环形缓冲的绝对下标**上
 *     （`absIdx % step === 0`），重叠部分一个样本都不动，只有两端进出。
 *  2. **纵轴量程**（`nextRange()`）：原来每帧按当前窗口极值重算量程，极值变一点点就
 *     整条曲线重新缩放 ⇒ 上下"呼吸"。现在放大即时、收缩缓慢（峰值包络）。
 */
import { describe, expect, it } from 'vitest'
import { SeriesRing, decimationStep } from './ring'
import { extent, nextRange, rawRange } from './sparkline'

describe('sparkline：纵轴量程滞回', () => {
  it('extent/rawRange：空数组不炸，也不给 NaN 量程', () => {
    expect(extent([]).min).toBeNaN()
    expect(extent([3, -1, 2])).toEqual({ min: -1, max: 3 })
    expect(rawRange([], false)).toEqual({ lo: -1, hi: 1 })
    // zeroScale：以 0 为中心对称，且不为 0 跨度的全零数据
    expect(rawRange([1, 3], true)).toEqual({ lo: -3, hi: 3 })
    expect(rawRange([0, 0], true)).toEqual({ lo: -1e-6, hi: 1e-6 })
  })

  it('nextRange：第一帧用原始量程', () => {
    expect(nextRange(null, { lo: -1, hi: 1 })).toEqual({ lo: -1, hi: 1 })
    expect(nextRange({ lo: 5, hi: 5 }, { lo: 0, hi: 1 })).toEqual({ lo: 0, hi: 1 })
  })

  it('nextRange：往外扩立刻扩（绝不裁掉数据）', () => {
    expect(nextRange({ lo: -1, hi: 1 }, { lo: -3, hi: 2 })).toEqual({ lo: -3, hi: 2 })
  })

  it('nextRange：往里收每 tick 只挪 decay（默认 2%）', () => {
    const r = nextRange({ lo: -10, hi: 10 }, { lo: -9, hi: 9 })
    expect(r.lo).toBeCloseTo(-9.98, 6)
    expect(r.hi).toBeCloseTo(9.98, 6)
    // 收缩过程中量程始终 ≥ 原始量程（不裁数据）
    expect(r.lo).toBeLessThanOrEqual(-9)
    expect(r.hi).toBeGreaterThanOrEqual(9)
  })

  it('nextRange：幅度掉到 55% 以下直接贴合，别把曲线压扁', () => {
    expect(nextRange({ lo: -10, hi: 10 }, { lo: -2, hi: 2 })).toEqual({ lo: -2, hi: 2 })
    // 恰好 55% 还不贴合（< 才贴）
    const r = nextRange({ lo: -10, hi: 10 }, { lo: -5.5, hi: 5.5 })
    expect(r.lo).toBeCloseTo(-9.91, 6)
  })
})

describe('ring：稳定相位抽稀', () => {
  it('decimationStep：带死区，容量边界不来回跳档', () => {
    expect(decimationStep(0, 200)).toBe(1)
    expect(decimationStep(150, 200)).toBe(1)
    expect(decimationStep(2000, 200)).toBe(10)
    expect(decimationStep(2000, 200, 10)).toBe(10)
    // 1800 > 200×9×0.98 = 1764 ⇒ 还留在 10 档
    expect(decimationStep(1800, 200, 10)).toBe(10)
    // 1764 ≤ 1764 ⇒ 才降到 9 档
    expect(decimationStep(1764, 200, 10)).toBe(9)
    // 死区不能反过来挡住升档
    expect(decimationStep(2000, 200, 1)).toBe(10)
  })

  it('view()：整窗极值/末值不受抽稀影响，相位锚在绝对下标', () => {
    const ring = new SeriesRing(4000, 1)
    for (let i = 0; i < 2000; i++) ring.push(i * 10, [i - 1000])

    const a = ring.view(20000, 0, 200, 1, 19990)
    expect(a.step).toBe(10)
    expect(a.count).toBe(200)
    expect(a.start % a.step).toBe(0)
    // 抽稀只留 200 个点，但极值/末值来自 2000 个点
    expect(a.windowCount).toBe(2000)
    expect(a.min).toBe(-1000)
    expect(a.max).toBe(999)
    expect(a.last).toBe(999)

    // 窗口滑一格（nowMs +10ms）：step 不变 ⇒ 重叠样本一个不换，只有两端进出
    const b = ring.view(20000, 0, 200, a.step, 20010)
    expect(b.step).toBe(10)
    expect(b.start % b.step).toBe(0)
    const bTs = new Set(Array.from(b.t))
    const same = Array.from(a.t).filter((t) => bTs.has(t)).length
    expect(same / a.count).toBeGreaterThan(0.98)
  })

  it('view()：对照旧实现（窗口相对下标起步）——同一场景下样本几乎全换', () => {
    const ring = new SeriesRing(4000, 1)
    for (let i = 0; i < 2000; i++) ring.push(i * 10, [i])

    /** 旧实现：snapshotField 整窗拷贝 + 从窗口下标 0 起步抽稀 */
    const oldSamples = (nowMs: number): number[] => {
      const s = ring.snapshotField(20000, 0, nowMs)
      const step = Math.max(1, Math.floor(s.count / 200))
      const out: number[] = []
      for (let i = 0; i < s.count; i += step) out.push(s.t[i])
      return out
    }
    const first = new Set(oldSamples(20000))
    const keep = oldSamples(20010).filter((t) => first.has(t)).length
    expect(keep / first.size).toBeLessThan(0.2)
  })

  it('view()：空缓冲给空视图，不抛异常', () => {
    const ring = new SeriesRing(100, 1)
    const v = ring.view(5000, 0, 100)
    expect(v.count).toBe(0)
    expect(v.min).toBeNaN()
    expect(v.last).toBeNaN()
    expect(ring.lastValue(0, 5000)).toBeNull()
  })

  it('lastValue()：O(1) 取窗口内最新一个样本', () => {
    const ring = new SeriesRing(100, 1)
    for (let i = 0; i < 50; i++) ring.push(i * 10, [i * 2])
    expect(ring.lastValue(0, 10000)).toBe(98)
    // 窗口只剩最后一帧时也是它（`range()` 的右端永远是最后一帧）
    expect(ring.lastValue(0, 0)).toBe(98)
    // 空窗口：窗口起点被推到最后一帧之后 —— 数据全被裁掉
    expect(ring.lastValue(0, 10, 1e9)).toBeNull()
  })
})
