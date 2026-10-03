/**
 * 操控页键盘映射的单测：把"按哪些键 ⇒ 下发什么指令"钉死。
 *
 * 这里只测纯函数（`lib/keys.ts`），不需要 jsdom：键盘事件 → 指令的整条链路里，
 * 唯一有分支的就是这层映射；剩下的（listen/cleanup）由 SSR 冒烟与手工核对覆盖。
 */
import { describe, expect, it } from 'vitest'
import { isTypingTarget, keyId, keyRole, keyRoleById, keysToDrive, rolesToDrive, type KeyRole } from './keys'

const OPT = { speed: 600, turn: 500 }
const cmd = (roles: KeyRole[]) => rolesToDrive(roles, OPT)
/** 用真实 KeyboardEvent 形状（`code` 优先）压一遍。 */
const press = (...codes: string[]) => keysToDrive(codes.map((code) => ({ code })), OPT)

describe('键盘 → 驾驶指令', () => {
  it('W 前进：base 为正、不转向', () => {
    const c = cmd(['fwd'])
    expect(c.mode).toBe('base_turn')
    expect(c.base).toBe(600)
    expect(c.turn).toBe(0)
    expect(c.label).toContain('前进')
  })

  it('S 后退：base 为负', () => {
    const c = cmd(['back'])
    expect(c.mode).toBe('base_turn')
    expect(c.base).toBe(-600)
    expect(c.turn).toBe(0)
    expect(c.label).toContain('后退')
  })

  it('A 左转 = turn 负（右轮快），D 右转 = turn 正（左轮快）', () => {
    expect(cmd(['left'])).toMatchObject({ mode: 'base_turn', base: 0, turn: -500 })
    expect(cmd(['right'])).toMatchObject({ mode: 'base_turn', base: 0, turn: 500 })
    expect(cmd(['left']).label).toContain('原地左转')
    expect(cmd(['right']).label).toContain('原地右转')
  })

  it('组合键：W+A 一边前进一边左转', () => {
    const c = cmd(['fwd', 'left'])
    expect(c).toMatchObject({ mode: 'base_turn', base: 600, turn: -500 })
    expect(c.label).toContain('前进')
    expect(c.label).toContain('边左转')
  })

  it('A+D 左右抵消 ⇒ turn 0；W 还按着就是直行', () => {
    expect(cmd(['left', 'right'])).toMatchObject({ mode: 'off', base: 0, turn: 0 })
    expect(cmd(['left', 'right']).label).toContain('左右抵消')
    expect(cmd(['fwd', 'left', 'right'])).toMatchObject({ mode: 'base_turn', base: 600, turn: 0 })
  })

  it('W+S 前后抵消 ⇒ 不前进；A/D 还按着就原地转', () => {
    expect(cmd(['fwd', 'back']).mode).toBe('off')
    expect(cmd(['fwd', 'back']).label).toContain('前后抵消')
    expect(cmd(['fwd', 'back', 'right'])).toMatchObject({ mode: 'base_turn', base: 0, turn: 500 })
    expect(cmd(['fwd', 'back', 'left', 'right']).mode).toBe('off')
  })

  it('什么都不按 / 空格 ⇒ off（交还车上任务流程，不是 0 速度继续接管）', () => {
    expect(cmd([])).toMatchObject({ mode: 'off', base: 0, turn: 0 })
    expect(cmd([]).label).toContain('待命')
    expect(cmd(['stop'])).toMatchObject({ mode: 'off', base: 0, turn: 0 })
    expect(cmd(['fwd', 'stop']).mode).toBe('off') // 空格优先级最高：还按着 W 也停
    expect(cmd(['stop']).label).toContain('急停')
  })

  it('速度/转向取绝对值，量程夹在 ±3000', () => {
    expect(rolesToDrive(['fwd'], { speed: -600, turn: 500 }).base).toBe(600)
    expect(rolesToDrive(['fwd'], { speed: 99999, turn: 0 }).base).toBe(3000)
    expect(rolesToDrive(['right'], { speed: 0, turn: -99999 }).turn).toBe(3000)
    expect(rolesToDrive(['fwd'], { speed: Number.NaN, turn: 0 }).mode).toBe('off')
  })

  it('物理键映射：WASD 与方向键同义，空格=急停', () => {
    expect(press('KeyW')).toMatchObject({ mode: 'base_turn', base: 600 })
    expect(press('ArrowUp')).toMatchObject({ mode: 'base_turn', base: 600 })
    expect(press('KeyW', 'ArrowRight')).toMatchObject({ mode: 'base_turn', base: 600, turn: 500 })
    expect(press('KeyA', 'ArrowRight')).toMatchObject({ mode: 'off', base: 0, turn: 0 }) // 方向键与 WASD 同义 ⇒ 也抵消
    expect(press('KeyW', 'KeyD')).toMatchObject({ base: 600, turn: 500 })
    expect(press('Space').mode).toBe('off')
    expect(press('KeyQ').mode).toBe('off') // 不关心的键 = 没按
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
