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

  it('全松开 ⇒ 发一帧 off 并把偏移清零（下次按 A 从新的正前方开始）', () => {
    const { rt, sent } = run([
      { intent: { base: 600, turnRate: 90 }, dtMs: 0 },
      { intent: { base: 600, turnRate: 90 }, dtMs: 500 },
      { intent: {}, dtMs: 0 },
    ])
    expect(sent.at(-1)).toEqual({ mode: 'off' })
    expect(rt).toEqual(initDriveRuntime())

    // 已经停了就别再刷 off
    const again = stepDrive(rt, intent({}), 0)
    expect(again.send).toEqual([])
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
      if (r.send.length) {
        const f = r.send[0] as { mode: string; turn?: number; yaw?: number }
        // ★2026-10-18：接管期间改成 hold（绝对航向）⇒ 目标用 yaw 表示，物理上在 (-180,180]。
        //   baseYaw 在这些用例里恒为 0（yaw 从 0 开始），所以 yaw 就等于偏移量（取模后）。
        turns.push(f.mode === 'hold' ? norm180(f.yaw ?? 0) : (f.turn ?? 0))
      }
      yaw = norm180(yaw + (carRate * 100) / 1000)
      if (kick && i === kick.at) yaw = norm180(yaw + kick.deg)
      maxLead = Math.max(maxLead, Math.abs(rt.lead))
    }
    return { rt, turns, maxLead, yaw }
  }

  it('★2026-10-19 新语义：按住时目标**一直涨**（不因车跟不上而拉回）', () => {
    const h = hold(60, 180, 80) // 6s：按住不放
    // 用户要求「按住时持续改变，松开保持不变」⇒ 按住期间领先可以超过 MAX_LEAD
    expect(h.maxLead).toBeGreaterThan(MAX_LEAD)
    // 一直按到底：目标仍然顶到 HOLD_LIMIT（车转过 540° 就该停），只是"贴着车"走完的
    expect(h.rt.offset).toBe(HOLD_LIMIT)
    // hold 的绝对航向物理上在 (-180,180] ⇒ 540° 等价于 180°
    expect(h.turns.at(-1)).toBe(norm180(HOLD_LIMIT))
    // 目标涨到 HOLD_LIMIT 就饱和，之后不再发新帧（不会像旧行为那样被反复拉回）
    expect(h.turns.length).toBeGreaterThan(10)
    expect(h.turns.length).toBeLessThanOrEqual(60)
  })

  it('车跟得上（设 90°/s、车也 90°/s）⇒ 这条线根本碰不到，每拍照常下发 9°', () => {
    const h = hold(20, 90, 90)
    expect(h.maxLead).toBeLessThan(1)
    expect(h.turns).toEqual([0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90, 99, 108, 117, 126, 135, 144, 153, 162, 171])
  })

  it('★松开时若车被外力带偏 ⇒ 仍拉回到 MAX_LEAD（防 180° 翻号倒拐）', () => {
    // 注意：turnRate=0 = 没按转向键（松开）。此时车被拽着转，领先会超线 ⇒ 必须拉回。
    const h = hold(60, 0, 80, { at: 30, deg: -40 })
    expect(h.maxLead).toBeLessThanOrEqual(MAX_LEAD + 1e-6)
  })

  it('★用户要求：按住持续改变、松开后保持不变', () => {
    let rt: DriveRuntime = initDriveRuntime()
    let yaw = 0
    // 按住 A 转 10 拍（90°/s ⇒ 每拍 +9°）
    for (let i = 0; i < 10; i++) {
      rt = stepDrive(rt, intent({ turnRate: 90 }), i === 0 ? 0 : 100, yaw).rt
      yaw = norm180(yaw + 9)
    }
    const held = rt.offset
    expect(held).toBeCloseTo(81, 0)          // 按住期间一直在涨（首拍 dt=0 不计）✓
    // 松开 10 拍（turnRate=0），车停住不动
    for (let i = 0; i < 10; i++) {
      rt = stepDrive(rt, intent({ turnRate: 0 }), 100, yaw).rt
    }
    expect(rt.offset).toBeCloseTo(held, 6)   // 松开后保持不变 ✓
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

/* __HOLD_MODE_TESTS__ */
/**
 * ★2026-10-18：松手 ⇒ 发 hold（绝对目标航向），让车原地转到位并锁住。
 *
 * 用户原话："我一松的时候，它不应该立马结束控制。就算我松了，它也应该回到这个目标角度"。
 * 为什么必须用 hold 而不是继续发 base_turn：base_turn 的 turn 是**相对**固件抓的基准，
 * 而固件在 HOST_BASE_REGRAB_MS(1s) 后会重抓基准 ⇒ 目标被反复累加 ⇒ 震荡/反向（已实测踩过）。
 * hold 的 yaw 是绝对量、幂等 ⇒ 连发 N 帧 == 1 帧。
 */
describe('松手保持目标角度（HOLD 模式）', () => {
  it('接管→转向→松手 ⇒ 发 hold(base=0, yaw=基准+偏移)，且仍然 armed', () => {
    let rt = initDriveRuntime()
    const sent: { mode: string; base?: number; yaw?: number }[] = []
    const tick = (i: Partial<DriveIntent>, dt: number, yaw?: number) => {
      const r = stepDrive(rt, intent(i), dt, yaw)
      rt = r.rt
      for (const f of r.send) sent.push(f as { mode: string; base?: number; yaw?: number })
    }
    tick({ base: 600, turnRate: 90 }, 0, 30)    // 接管那一刻车头 30° ⇒ baseYaw = 30
    tick({ base: 600, turnRate: 90 }, 500)      // 按住 A 0.5 秒 ⇒ offset = +45°
    const offsetAtRelease = rt.offset
    tick({}, 0, 34)                             // 全松开

    const last = sent[sent.length - 1]
    expect(last.mode).toBe('hold')
    expect(last.base).toBe(0)
    expect(last.yaw).toBeCloseTo(norm180(30 + offsetAtRelease), 1)
    expect(rt.armed).toBe(true)                 // 不撒手
    expect(rt.offset).toBe(offsetAtRelease)     // 目标不丢
  })

  it('hold 帧幂等：同样的目标再走一拍不会再发（去重）', () => {
    let rt = initDriveRuntime()
    const tick = (i: Partial<DriveIntent>, dt: number, yaw?: number) => {
      const r = stepDrive(rt, intent(i), dt, yaw)
      rt = r.rt
      return r.send
    }
    tick({ base: 600, turnRate: 90 }, 0, 30)
    const rel = tick({}, 0, 30)                 // 第一次松手 ⇒ 必发一帧
    expect(rel.length).toBe(1)
    expect(rel[0].mode).toBe('hold')
    const again = tick({}, 0, 30)               // 再来一拍 ⇒ 完全一样，去重后不发
    expect(again.length).toBe(0)
  })

  it('没有遥测航向（yawNow 未定义）时回退成 off，绝不乱发绝对角', () => {
    const { sent } = run([
      { intent: { base: 600, turnRate: 90 }, dtMs: 0 },
      { intent: {}, dtMs: 0 },
    ])
    expect((sent[sent.length - 1] as { mode: string }).mode).toBe('off')
  })
})
