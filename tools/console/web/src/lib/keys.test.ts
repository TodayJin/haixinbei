/**
 * 操控页键盘映射的单测：把"按哪些键 ⇒ 什么驾驶意图"钉死。
 *
 * 这里只测纯函数（`lib/keys.ts`）：键盘事件 → 意图的整条链路里，唯一有分支的就是
 * 这层映射；意图 → 帧（积分目标航向）在 `lib/heading.ts`，由 heading.test.ts 覆盖；
 * 剩下的 listen/cleanup 由 SSR 冒烟与手工核对覆盖。
 *
 * 注意量纲：`turn` 是**转向速度 °/s**（正 = 左转 = 偏航角增大），不是左右轮差速。
 */
import { describe, expect, it } from 'vitest'
import { isTypingTarget, keyId, keyRole, keyRoleById, rolesToIntent, type KeyRole } from './keys'

const OPT = { speed: 600, turn: 90 }
const cmd = (roles: KeyRole[]) => rolesToIntent(roles, OPT)
/** 用真实 KeyboardEvent 形状（`code` 优先）压一遍：键 → 角色 → 意图。 */
const press = (...codes: string[]) =>
  rolesToIntent(
    codes.map((code) => keyRole({ code })).filter((r): r is KeyRole => r !== null),
    OPT,
  )

describe('键盘 → 驾驶意图', () => {
  it('W 前进：base 为正、不转向', () => {
    const c = cmd(['fwd'])
    expect(c.base).toBe(600)
    expect(c.turnRate).toBe(0)
    expect(c.stop).toBe(false)
    expect(c.label).toContain('前进')
  })

  it('S 后退：base 为负', () => {
    const c = cmd(['back'])
    expect(c.base).toBe(-600)
    expect(c.turnRate).toBe(0)
    expect(c.label).toContain('后退')
  })

  it('A 左转 = turnRate 为正（偏航角增大），D 右转 = 负', () => {
    expect(cmd(['left'])).toMatchObject({ base: 0, turnRate: 90, stop: false })
    expect(cmd(['right'])).toMatchObject({ base: 0, turnRate: -90, stop: false })
    expect(cmd(['left']).label).toContain('原地左转')
    expect(cmd(['right']).label).toContain('原地右转')
  })

  it('组合键：W+A 一边前进一边左转', () => {
    const c = cmd(['fwd', 'left'])
    expect(c).toMatchObject({ base: 600, turnRate: 90 })
    expect(c.label).toContain('前进')
    expect(c.label).toContain('边左转')
  })

  it('A+D 左右抵消 ⇒ 不转；W 还按着就是直行', () => {
    expect(cmd(['left', 'right'])).toMatchObject({ base: 0, turnRate: 0, stop: false })
    expect(cmd(['left', 'right']).label).toContain('左右抵消')
    expect(cmd(['fwd', 'left', 'right'])).toMatchObject({ base: 600, turnRate: 0 })
  })

  it('W+S 前后抵消 ⇒ 不前进；A/D 还按着就原地转', () => {
    expect(cmd(['fwd', 'back'])).toMatchObject({ base: 0, turnRate: 0 })
    expect(cmd(['fwd', 'back']).label).toContain('前后抵消')
    expect(cmd(['fwd', 'back', 'right'])).toMatchObject({ base: 0, turnRate: -90 })
    expect(cmd(['fwd', 'back', 'left', 'right'])).toMatchObject({ base: 0, turnRate: 0 })
  })

  it('什么都不按 = 待命；空格 ⇒ stop（交还车上任务流程，不是 0 速度继续接管）', () => {
    expect(cmd([])).toMatchObject({ base: 0, turnRate: 0, stop: false })
    expect(cmd([]).label).toContain('待命')
    expect(cmd(['stop'])).toMatchObject({ base: 0, turnRate: 0, stop: true })
    expect(cmd(['fwd', 'stop']).stop).toBe(true) // 空格优先级最高：还按着 W 也停
    expect(cmd(['stop']).label).toContain('急停')
  })

  it('速度取绝对值并夹在 ±3000 mm/s；转向速度夹在 ±180 °/s', () => {
    expect(rolesToIntent(['fwd'], { speed: -600, turn: 90 }).base).toBe(600)
    expect(rolesToIntent(['fwd'], { speed: 99999, turn: 0 }).base).toBe(3000)
    expect(rolesToIntent(['right'], { speed: 0, turn: -99999 }).turnRate).toBe(-180)
    expect(rolesToIntent(['left'], { speed: 0, turn: 99999 }).turnRate).toBe(180)
    expect(rolesToIntent(['fwd'], { speed: Number.NaN, turn: 0 }).base).toBe(0)
  })

  it('物理键映射：WASD 与方向键同义，空格=急停', () => {
    expect(press('KeyW')).toMatchObject({ base: 600 })
    expect(press('ArrowUp')).toMatchObject({ base: 600 })
    expect(press('KeyW', 'ArrowRight')).toMatchObject({ base: 600, turnRate: -90 })
    expect(press('KeyA', 'ArrowRight')).toMatchObject({ base: 0, turnRate: 0 }) // 方向键与 WASD 同义 ⇒ 也抵消
    expect(press('KeyW', 'KeyD')).toMatchObject({ base: 600, turnRate: -90 })
    expect(press('Space').stop).toBe(true)
    expect(press('KeyQ')).toMatchObject({ base: 0, turnRate: 0, stop: false }) // 不关心的键 = 没按
    expect(keyRole({ code: 'KeyW' })).toBe('fwd')
    expect(keyRole({ key: 'w' })).toBe('fwd') // code 缺失时走 key 兜底
    expect(keyRole({ code: 'KeyQ', key: 'q' })).toBeNull()
  })

  it('按键标识：W 与 ↑ 是两个键、同一个角色（keyup 才能各松各的）', () => {
    expect(keyId({ code: 'KeyW' })).toBe('KeyW')
    expect(keyId({ code: 'ArrowUp' })).toBe('ArrowUp')
    expect(keyId({ code: 'KeyQ' })).toBeNull()
    expect(keyRoleById('KeyW')).toBe('fwd')
    expect(keyRoleById('ArrowUp')).toBe('fwd')
    expect(keyRoleById('Space')).toBe('stop')
    expect(keyRoleById('KeyQ')).toBeNull()
  })

  it('输入框里不抢键，但滑块/按钮不算输入框', () => {
    // 真实的 EventTarget 上没有 tagName/type（那是 Element 上的），这里只是喂给它 DOM 的形状
    const el = (o: Record<string, unknown>) => o as unknown as EventTarget
    expect(isTypingTarget(el({ tagName: 'INPUT', type: 'text' }))).toBe(true)
    expect(isTypingTarget(el({ tagName: 'INPUT', type: 'number' }))).toBe(true)
    expect(isTypingTarget(el({ tagName: 'INPUT' }))).toBe(true) // type 缺省 = text
    expect(isTypingTarget(el({ tagName: 'TEXTAREA' }))).toBe(true)
    expect(isTypingTarget(el({ tagName: 'SELECT' }))).toBe(true)
    expect(isTypingTarget(el({ tagName: 'DIV', isContentEditable: true }))).toBe(true)
    expect(isTypingTarget(el({ tagName: 'INPUT', type: 'range' }))).toBe(false)
    expect(isTypingTarget(el({ tagName: 'INPUT', type: 'checkbox' }))).toBe(false)
    expect(isTypingTarget(el({ tagName: 'BUTTON' }))).toBe(false)
    expect(isTypingTarget(el({ tagName: 'BODY' }))).toBe(false)
    expect(isTypingTarget(null)).toBe(false)
  })
})
