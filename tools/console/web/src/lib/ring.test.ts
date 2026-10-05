import { describe, expect, it } from 'vitest'
import { SeriesRing } from './ring'

describe('SeriesRing', () => {
  it('按时间升序 push 后能取出窗口', () => {
    const r = new SeriesRing(10, 2)
    for (let i = 0; i < 5; i++) r.push(i * 100, [i, i * 2])
    expect(r.size).toBe(5)
    const snap = r.snapshot(1000)
    expect(snap.count).toBe(5)
    expect(Array.from(snap.t)).toEqual([0, 100, 200, 300, 400])
    expect(Array.from(snap.v[0])).toEqual([0, 1, 2, 3, 4])
    expect(Array.from(snap.v[1])).toEqual([0, 2, 4, 6, 8])
  })

  it('环形绕回后仍然只给最近 windowMs 的数据', () => {
    const r = new SeriesRing(4, 1)
    for (let i = 0; i < 10; i++) r.push(i * 10, [i])
    // 容量 4：只剩 6,7,8,9（t=60..90）
    expect(r.size).toBe(4)
    const snap = r.snapshot(25) // >=65ms
    expect(Array.from(snap.t)).toEqual([70, 80, 90])
    expect(Array.from(snap.v[0])).toEqual([7, 8, 9])
  })

  it('resize 保留最近的数据并改变容量', () => {
    const r = new SeriesRing(8, 1)
    for (let i = 0; i < 8; i++) r.push(i, [i])
    r.resize(3)
    expect(r.capacity).toBe(3)
    expect(r.size).toBe(3)
    const all = r.all()
    expect(Array.from(all.v[0])).toEqual([5, 6, 7])
  })

  it('非有限值写入 0，值不够时补 0', () => {
    const r = new SeriesRing(4, 3)
    r.push(1, [Number.NaN, 2])
    const s = r.snapshot(10)
    expect(Array.from(s.v[0])).toEqual([0])
    expect(Array.from(s.v[1])).toEqual([2])
    expect(Array.from(s.v[2])).toEqual([0])
  })

  it('clear 之后 size=0、窗口为空', () => {
    const r = new SeriesRing(4, 1)
    r.push(1, [1])
    r.clear()
    expect(r.size).toBe(0)
    expect(r.snapshot(100).count).toBe(0)
  })

  it('空时 snapshot 不炸', () => {
    const r = new SeriesRing(4, 2)
    const s = r.snapshot(1000)
    expect(s.count).toBe(0)
    expect(s.v.length).toBe(2)
  })

  /**
   * 回归（用户 m30822「已经走过的那段会乱跳」）：抽稀相位必须锚在**绝对样本号**上。
   *
   * 旧实现锚的是环内逻辑下标 `lo` —— 环没满时两码事恰好相等（所以看着没问题），
   * 一旦环绕回（`len === cap`），`phys(k)` 随 push 整体滑动 ⇒ 每帧整条线换一批样本。
   * 下面第一段就是旧实现的反例：旧代码给 [33, 37]，新代码给 [32, 36, 40]。
   */
  it('环绕回之后抽稀相位锚绝对样本号（已画过的点不乱跳）', () => {
    const r = new SeriesRing(20, 1)
    for (let i = 0; i <= 40; i++) r.push(i * 10, [i]) // t=0..400；容量 20 ⇒ 只剩 i=21..40
    const a = r.view(100, 0, 3) // 窗口 t>=300 ⇒ i=30..40，step=4
    expect(Array.from(a.v)).toEqual([32, 36, 40])
    expect(a.step).toBe(4)
    expect(a.start).toBe(32) // 绝对样本号（i=32）
    expect(a.start % a.step).toBe(0)
    expect(a.endT).toBe(400)

    // 再推一帧：窗口里重叠部分的采样点一个都不许换，网格也不许整体挪
    r.push(410, [41])
    const b = r.view(100, 0, 3)
    expect(Array.from(b.v)).toEqual([32, 36, 40])
    expect(b.start).toBe(a.start)
    expect(b.endT).toBe(410)
  })
})
