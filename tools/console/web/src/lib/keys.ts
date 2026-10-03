/**
 * 键盘 → 驾驶意图的映射（操控页用）。
 *
 * 设计口径（对应用户要求）：
 *   - W/S = 前进 / 后退，A/D = 左转 / 右转；
 *   - 能组合：W+A = 一边前进一边左转（斜着走），W+S 相互抵消；
 *   - A+D 同时按 = 左右抵消 ⇒ 转向为 0（两个方向谁也不欠谁）；
 *   - 所有方向键都松开 / 按了空格 = 停机（把驾驶交还车上的任务流程，不是"0 速度继续接管"）。
 *
 * 注意这里给的是**意图**（基准速度 mm/s + 转向速度 °/s），不是最终帧：转向是速度，
 * 要由 `lib/heading.ts` 的 stepDrive 按节拍积分成目标航向偏移再下发。
 *
 * 这里刻意是纯函数（不碰 DOM、不碰 store），方便单测把"按哪些键得到什么意图"钉死。
 */
import { TURN_RATE_LIMIT, clamp, turnRateOf, type DriveIntent } from './heading'

export type KeyRole = 'fwd' | 'back' | 'left' | 'right' | 'stop'

/** 物理键 → 语义。`code` 优先（与键盘布局无关：中文输入法/非 QWERTY 也能按到 WASD）。 */
export const KEY_CODE_ROLE: Record<string, KeyRole> = {
  KeyW: 'fwd',
  KeyS: 'back',
  KeyA: 'left',
  KeyD: 'right',
  ArrowUp: 'fwd',
  ArrowDown: 'back',
  ArrowLeft: 'left',
  ArrowRight: 'right',
  Space: 'stop',
  Escape: 'stop',
}

/** `key` 兜底（`code` 缺失的浏览器/合成事件）。 */
export const KEY_NAME_ROLE: Record<string, KeyRole> = {
  w: 'fwd',
  s: 'back',
  a: 'left',
  d: 'right',
  arrowup: 'fwd',
  arrowdown: 'back',
  arrowleft: 'left',
  arrowright: 'right',
  ' ': 'stop',
  spacebar: 'stop',
  escape: 'stop',
  esc: 'stop',
}

/** 一个按键事件 → 语义角色；不关心这个键就返回 null。 */
export function keyRole(e: { key?: string; code?: string }): KeyRole | null {
  const code = e.code
  if (code && KEY_CODE_ROLE[code]) return KEY_CODE_ROLE[code]
  const key = (e.key ?? '').toLowerCase()
  return KEY_NAME_ROLE[key] ?? null
}

/**
 * 按键标识：页面拿它当"按住的是哪个键"的集合元素（W 与 ↑ 是两个键、但同一个角色）。
 * 不关心的键返回 null。`code` 优先，与键盘布局无关。
 */
export function keyId(e: { key?: string; code?: string }): string | null {
  const code = e.code
  if (code && KEY_CODE_ROLE[code]) return code
  const key = (e.key ?? '').toLowerCase()
  return KEY_NAME_ROLE[key] ? key : null
}

/** 按键标识 → 语义角色（`keyId` 的逆运算）。 */
export function keyRoleById(id: string): KeyRole | null {
  return KEY_CODE_ROLE[id] ?? KEY_NAME_ROLE[id] ?? null
}

/**
 * 焦点在输入框里的时候不抢键（否则没法输数字）。
 * 注意：`input[type=range]`（滑块）与按钮**不**算输入框 —— 点过滑块之后 WASD 仍然要能开车。
 */
export function isTypingTarget(t: EventTarget | null | undefined): boolean {
  if (!t || typeof t !== 'object') return false
  const el = t as { tagName?: string; type?: string; isContentEditable?: boolean }
  if (el.isContentEditable) return true
  const tag = (el.tagName ?? '').toUpperCase()
  if (tag === 'TEXTAREA' || tag === 'SELECT') return true
  if (tag === 'INPUT') {
    const type = (el.type ?? 'text').toLowerCase()
    return type !== 'range' && type !== 'button' && type !== 'checkbox' && type !== 'radio'
  }
  return false
}

export interface KeyDriveOptions {
  /** 前进/后退的基准速度 mm/s */
  speed: number
  /** 转向速度上限 °/s（正 = 左转，与固件 `T_DRIVE` 的 b 同号） */
  turn: number
}

/**
 * 按住的键 → 驾驶意图。转向给的是**速度**（°/s），不是角度：页面再按节拍把它
 * 积分成目标航向偏移，然后交给车上的角度环去追（见 `lib/heading.ts`）。
 */
export interface KeyDriveInput extends DriveIntent {
  /** 命中的语义角色，按 前进/后退/左/右/停 排序去重 */
  roles: KeyRole[]
  /** 界面上显示的一句话（为什么是这个意图） */
  label: string
}

const ROLE_ORDER: KeyRole[] = ['fwd', 'back', 'left', 'right', 'stop']

function clampMmps(v: number): number {
  const n = Math.round(Number.isFinite(v) ? v : 0)
  return Math.max(-3000, Math.min(3000, n))
}

/** 把"当前按住的角色集合"算成一个驾驶意图。 */
export function rolesToIntent(roles: Iterable<KeyRole>, opts: KeyDriveOptions): KeyDriveInput {
  const set = new Set(roles)
  const held = ROLE_ORDER.filter((r) => set.has(r))
  const speed = clampMmps(opts.speed)
  const turnMax = Math.abs(opts.turn)

  const idle = (label: string, stop = false): KeyDriveInput => ({ base: 0, turnRate: 0, stop, roles: held, label })

  if (set.has('stop')) return idle('急停（空格）', true)
  if (!set.has('fwd') && !set.has('back') && !set.has('left') && !set.has('right')) return idle('待命 —— 按 W/A/S/D')

  const fwd = set.has('fwd') ? 1 : 0
  const back = set.has('back') ? 1 : 0
  const left = set.has('left') ? 1 : 0
  const right = set.has('right') ? 1 : 0

  const base = clampMmps((fwd - back) * Math.abs(speed))
  // 左转 = 偏航角增大 = 正；A+D 同时按 ⇒ 0（谁也不欠谁）
  const turnRate = clamp(turnRateOf(!!left, !!right, turnMax), -TURN_RATE_LIMIT, TURN_RATE_LIMIT)

  if (base === 0 && turnRate === 0) {
    // 按了键但两个轴都抵消掉了：说清楚是哪一个轴抵消的，别让用户以为是坏了
    const fw = fwd || back
    const lr = left || right
    if (fw && lr) return idle('前后 + 左右都抵消 ⇒ 停车')
    if (fw) return idle('前后抵消 ⇒ 停车')
    return idle('左右抵消 ⇒ 停车')
  }

  const parts: string[] = []
  if (base > 0) parts.push('前进')
  if (base < 0) parts.push('后退')
  if (turnRate > 0) parts.push(base !== 0 ? '边左转' : '原地左转')
  if (turnRate < 0) parts.push(base !== 0 ? '边右转' : '原地右转')

  return { base, turnRate, stop: false, roles: held, label: parts.join(' + ') }
}
