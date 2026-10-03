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
})
