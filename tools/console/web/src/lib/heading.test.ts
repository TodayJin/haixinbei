/**
 * 朝向控制口径的单测（`lib/heading.ts`）。
 *
 * 这里要钉死两件事，都是我自己踩过的坑：
 *   1. 固件 `base_turn` 的 b 是**角度（度）**、正 = 偏航角增大 = 左转 —— 不是左右轮差速 mm/s；
 *   2. 按 A（左转）算出来的 turn 必须是**正**的，别再写反（写反了车就往反方向转，
 *      而仿真里当时又恰好没有角度环，谁都发现不了）。
 */
import { describe, expect, it } from 'vitest'
import {
  HOLD_LIMIT,
  MAX_LEAD,
  clamp,
  headingError,
  initDriveRuntime,
  norm180,
  offsetAfter,
  offsetLabel,
  stepDrive,
  targetHeading,
  turnRateOf,
  type DriveIntent,
  type DriveRuntime,
} from './heading'

const intent = (o: Partial<DriveIntent> = {}): DriveIntent => ({ base: 0, turnRate: 0, stop: false, ...o })

/** 按顺序跑一串节拍，返回每次发出的帧（打字用）。 */
function run(steps: { intent: Partial<DriveIntent>; dtMs: number }[]) {
  let rt: DriveRuntime = initDriveRuntime()
  const sent: unknown[] = []
  for (const s of steps) {
    const r = stepDrive(rt, intent(s.intent), s.dtMs)
    rt = r.rt
    sent.push(...r.send)
  }
  return { rt, sent }
}

describe('角度归一与朝向换算', () => {
  it('norm180 收进 (-180, 180]', () => {
    expect(norm180(0)).toBe(0)
    expect(norm180(180)).toBe(180)
    expect(norm180(-180)).toBe(180)
    expect(norm180(190)).toBe(-170)
    expect(norm180(-190)).toBe(170)
    expect(norm180(360)).toBe(0)
    expect(norm180(725)).toBe(5)
    expect(norm180(Number.NaN)).toBe(0)
  })

  it('目标航向 = 基准 + 偏移；误差 = 目标 − 当前（正 = 还要往左转）', () => {
    expect(targetHeading(-64.7, 30)).toBeCloseTo(-34.7, 6)
    expect(targetHeading(170, 30)).toBeCloseTo(-160, 6) // 跨 ±180 要走近路
    expect(headingError(-34.7, -64.7)).toBeCloseTo(30, 6) // 目标还在左边 ⇒ 正
    expect(headingError(-64.7, -34.7)).toBeCloseTo(-30, 6)
    expect(headingError(170, -170)).toBeCloseTo(-20, 6)
  })

  it('clamp 兜住 NaN/Infinity', () => {
    expect(clamp(5, 0, 10)).toBe(5)
    expect(clamp(-5, 0, 10)).toBe(0)
    expect(clamp(50, 0, 10)).toBe(10)
    expect(clamp(Number.NaN, -1, 1)).toBe(0)
    expect(clamp(Number.POSITIVE_INFINITY, -1, 1)).toBe(1)
    expect(clamp(Number.NEGATIVE_INFINITY, -1, 1)).toBe(-1)
  })

  it('offsetLabel 说人话', () => {
    expect(offsetLabel(0)).toBe('回正 0°')
    expect(offsetLabel(30)).toBe('左转 30°')
    expect(offsetLabel(-12.4)).toBe('右转 12°')
  })
})

describe('转向速度（°/s）', () => {
  it('左 = 正、右 = 负、两边都按 = 抵消', () => {
    expect(turnRateOf(true, false, 90)).toBe(90)
    expect(turnRateOf(false, true, 90)).toBe(-90)
    expect(turnRateOf(true, true, 90)).toBe(0)
    expect(turnRateOf(false, false, 90)).toBe(0)
    expect(turnRateOf(true, false, -90)).toBe(90) // 取绝对值，方向由左右决定
  })

  it('按节拍积分：90°/s 跑 1s = 90°，到 ±540° 饱和', () => {
    expect(offsetAfter(0, 90, 1000)).toBe(90)
    expect(offsetAfter(0, -90, 1000)).toBe(-90)
    expect(offsetAfter(0, 90, 50)).toBeCloseTo(4.5, 6)
    expect(offsetAfter(HOLD_LIMIT, 90, 1000)).toBe(HOLD_LIMIT)
    expect(offsetAfter(-HOLD_LIMIT, -90, 1000)).toBe(-HOLD_LIMIT)
  })
})

describe('操控页状态机 stepDrive', () => {
  it('按 W ⇒ 一帧 base_turn（基准速度 + 偏移 0），重复同样的意图不再发', () => {
    const a = run([{ intent: { base: 600 }, dtMs: 0 }])
    expect(a.sent).toEqual([{ mode: 'base_turn', base: 600, turn: 0 }])
    expect(a.rt.armed).toBe(true)

    const b = run([
      { intent: { base: 600 }, dtMs: 0 },
      { intent: { base: 600 }, dtMs: 100 },
      { intent: { base: 600 }, dtMs: 100 },
    ])
    expect(b.sent).toHaveLength(1) // 键按着不动就别刷帧
  })

  it('按 A 是 turn 正（左转），并且按 100ms 节拍累加', () => {
    const { sent, rt } = run([
      { intent: { turnRate: 90 }, dtMs: 0 },
      { intent: { turnRate: 90 }, dtMs: 100 },
      { intent: { turnRate: 90 }, dtMs: 100 },
    ])
    expect(sent).toEqual([
      { mode: 'base_turn', base: 0, turn: 0 },
      { mode: 'base_turn', base: 0, turn: 9 },
      { mode: 'base_turn', base: 0, turn: 18 },
    ])
    expect(rt.offset).toBeCloseTo(18, 6)
  })

  it('按 D 是 turn 负（右转）', () => {
    const { sent } = run([
      { intent: { turnRate: -90 }, dtMs: 0 },
      { intent: { turnRate: -90 }, dtMs: 100 },
    ])
    expect(sent[0]).toEqual({ mode: 'base_turn', base: 0, turn: 0 })
    expect(sent[1]).toEqual({ mode: 'base_turn', base: 0, turn: -9 })
  })

  it('松手保持偏移（不再发帧），一直按着到 HOLD_LIMIT 就饱和', () => {
    const hold = run([
      { intent: { base: 600, turnRate: 90 }, dtMs: 0 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 1000 }, // 7s × 90 = 630 ⇒ 夹到 540
    ])
    expect(hold.rt.offset).toBe(HOLD_LIMIT)
    expect(hold.sent.at(-1)).toEqual({ mode: 'base_turn', base: 600, turn: HOLD_LIMIT })

    // 松开 A、W 还按着：偏移保持，帧不变 ⇒ 不重复下发
    const after = stepDrive(hold.rt, intent({ base: 600 }), 100)
    expect(after.rt.offset).toBe(HOLD_LIMIT)
    expect(after.send).toEqual([])
  })

  it('全松开但目标角度不为0 ⇒ 原地转到目标并保持（不再发 off）', () => {
    const { rt, sent } = run([
      { intent: { base: 600, turnRate: 90 }, dtMs: 0 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 500 },
      { intent: {}, dtMs: 0 },
    ])
    // 2026-10-18 用户要求：设了目标角度时，松手应该原地转到目标并保持（角度环继续工作），
    // 而不是发 off 把角度环关掉（那会留下追不到的误差）。
    expect(sent.at(-1)).toEqual({ mode: 'base_turn', base: 0, turn: Math.round(rt.offset) })

    // 已经停了就别再刷 off
    const again = stepDrive(rt, intent({}), 0)
    // 保持模式下每拍都会重复下发同一帧（和正常操控一样），不会累加
    expect(again.send).toEqual([{ mode: 'base_turn', base: 0, turn: Math.round(rt.offset) }])
  })

  it('空格急停 ⇒ 一帧 off；本来就没接管则什么都不发', () => {
    const driving = run([
      { intent: { base: 600 }, dtMs: 0 },
      { intent: { stop: true }, dtMs: 0 },
    ])
    expect(driving.sent.at(-1)).toEqual({ mode: 'off' })
    expect(driving.rt.armed).toBe(false)

    expect(stepDrive(initDriveRuntime(), intent({ stop: true }), 0).send).toEqual([])
  })

  it('反向转回来：turn 从正走到负，误差回正（跨 0 不跳）', () => {
    const { sent } = run([
      { intent: { turnRate: 90 }, dtMs: 0 },
      { intent: { turnRate: 90 }, dtMs: 1000 },
      { intent: { turnRate: -90 }, dtMs: 1000 },
    ])
    expect(sent.map((f) => (f as { turn?: number }).turn)).toEqual([0, 90, 0])
  })

  it('倒车时打轮翻号：S+D 车尾往右摆（turn 走正）、S+A 往左摆（turn 走负）', () => {
    // S+D：base 负 + turnRate 负（D）⇒ 取反后偏移往正走 = 目标航向往左拧 = 车尾往右
    const sd = run([
      { intent: { base: -600, turnRate: -90 }, dtMs: 0 },
      { intent: { base: -600, turnRate: -90 }, dtMs: 100 },
    ])
    expect(sd.sent).toEqual([
      { mode: 'base_turn', base: -600, turn: 0 },
      { mode: 'base_turn', base: -600, turn: 9 },
    ])

    // S+A：base 负 + turnRate 正（A）⇒ 取反后偏移往负走
    const sa = run([{ intent: { base: -600, turnRate: 90 }, dtMs: 100 }])
    expect(sa.sent).toEqual([{ mode: 'base_turn', base: -600, turn: -9 }])

    // 前进不翻：W+D 仍然是 turn 负（车头往右）
    const wd = run([{ intent: { base: 600, turnRate: -90 }, dtMs: 100 }])
    expect(wd.sent).toEqual([{ mode: 'base_turn', base: 600, turn: -9 }])

    // 原地转（base 0）不翻：A 仍然是车头往左
    const pivot = run([{ intent: { turnRate: 90 }, dtMs: 100 }])
    expect(pivot.sent).toEqual([{ mode: 'base_turn', base: 0, turn: 9 }])

    // 一直按着 S+D：偏移累积到 HOLD_LIMIT 就饱和（方向不会穿过 0 跑反）
    const long = run(Array.from({ length: 8 }, () => ({ intent: { base: -600, turnRate: -90 }, dtMs: 1000 })))
    expect(long.rt.offset).toBe(HOLD_LIMIT)
  })
})

describe('MAX_LEAD 领先保护（2026-10-07 真车实测：领先过 180° 车会倒拐）', () => {
  /**
   * 模拟"按住 A 一直转"：页面第一拍 pump(0)（锁存基准），之后每 100ms 一拍照常积分；
   * 车按 carRate °/s 真的转，`kick` 用来注入一次"车被拽/打滑"的反向角度。
   */
  function hold(ticks: number, rate: number, carRate: number, kick?: { at: number; deg: number }) {
    let rt: DriveRuntime = initDriveRuntime()
    let yaw = 0
    let maxLead = 0
    const turns: number[] = []
    for (let i = 0; i < ticks; i++) {
      const r = stepDrive(rt, intent({ turnRate: rate }), i === 0 ? 0 : 100, yaw)
      rt = r.rt
      if (r.send.length) turns.push((r.send[0] as { turn: number }).turn)
      yaw = norm180(yaw + (carRate * 100) / 1000)
      if (kick && i === kick.at) yaw = norm180(yaw + kick.deg)
      maxLead = Math.max(maxLead, Math.abs(rt.lead))
    }
    return { rt, turns, maxLead, yaw }
  }

  it('车跟不上（设 180°/s、车只有 80°/s）⇒ 目标贴着车走，领先恒 ≤ MAX_LEAD', () => {
    const h = hold(60, 180, 80) // 6s：不保护的话目标早到 540°、领先 300°+
    expect(h.maxLead).toBeLessThanOrEqual(MAX_LEAD + 1e-6)
    // 一直按到底：目标仍然顶到 HOLD_LIMIT（车转过 540° 就该停），只是"贴着车"走完的
    expect(h.rt.offset).toBe(HOLD_LIMIT)
    expect(h.turns.at(-1)).toBe(HOLD_LIMIT)
    // 大部分拍都在发帧（目标前进或被拉回）；最后几拍目标饱和、车追上了才不发
    expect(h.turns.length).toBeGreaterThan(40)
    expect(h.turns.length).toBeLessThanOrEqual(60)
  })

  it('车跟得上（设 90°/s、车也 90°/s）⇒ 这条线根本碰不到，每拍照常下发 9°', () => {
    const h = hold(20, 90, 90)
    expect(h.maxLead).toBeLessThan(1)
    expect(h.turns).toEqual([0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90, 99, 108, 117, 126, 135, 144, 153, 162, 171])
  })

  it('车被打滑/拽回转 40° ⇒ 领先被拉回 MAX_LEAD（不会推到 180° 的翻号线）', () => {
    const h = hold(60, 180, 80, { at: 30, deg: -40 })
    expect(h.maxLead).toBeLessThanOrEqual(MAX_LEAD + 1e-6)
  })

  it('不传航向（老调用）时保护不生效 —— 行为与以前逐位相同', () => {
    const { rt } = run(Array.from({ length: 8 }, () => ({ intent: { turnRate: 90 }, dtMs: 1000 })))
    expect(rt.offset).toBe(HOLD_LIMIT)
  })

  it('往回拧不受保护影响：领先卡住时按 D 仍能把目标拧回来', () => {
    let rt: DriveRuntime = initDriveRuntime()
    let yaw = 0
    for (let i = 0; i < 30; i++) {
      rt = stepDrive(rt, intent({ turnRate: 180 }), i === 0 ? 0 : 100, yaw).rt
      yaw = norm180(yaw + 8)
    }
    const locked = rt.offset
    const back = stepDrive(rt, intent({ turnRate: -180 }), 100, yaw)
    expect(back.rt.offset).toBeLessThan(locked)
    expect(back.send).not.toEqual([])
  })
})
