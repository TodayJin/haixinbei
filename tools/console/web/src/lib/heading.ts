/**
 * 朝向（航向）控制的口径与纯逻辑 —— IMU 页的朝向滑动条、操控页的转向键都走这里。
 *
 * 为什么是"朝向"而不是"差速"：驾驶模式的 `base_turn` 在车上是这么实现的
 * （`firmware/App/Svc/host_ext.c` HostExt_DriveSet）：
 *
 *     s_base_yaw = Yaw;                                  // 进入本模式时的航向
 *     Motion_SetSpeed(a);                                // a = 基准速度刻度（0..7200，见 PWM_MAX）
 *     Motion_SetMode(MOTION_GYRO);
 *     GyroControl_SetTargetAngle(NormalizeAngle(s_base_yaw + b));   // b = 目标转角 °
 *
 * 也就是把 `Motion_GYRO`（角度环 + 速度环）的目标航向设成 abs 基准航向 + b。
 * ⇒ **b 的单位是度，不是 mm/s；正 b = 偏航角增大 = 左转**（`gyro_control.c`
 * TurnLeft 的注释：偏航角增大 = 向左）。基准速度给 0 就是"原地转到该航向并保持"。
 *
 * 上位机因此不再直接发差速，而是发"想要的目标航向偏移"，由车上的角度环去追。
 */
import type { DriveRequest } from './types'

/** 单次操作里允许的目标偏移上限（度）：一直按着转向键最多累计这么多就饱和。 */
export const HOLD_LIMIT = 540

/**
 * 目标最多允许领先车头多少度（**原地转实测出来的安全线**）。
 *
 * 为什么必须有它：页面是按设定速率**无脑**拧目标角的，而车的原地转速有物理上限
 * （2026-10-07 真车实测：设定 30°/s → 实测 30.0；60 → 48.8；90 → 75.9；120 → 79.2；
 * 180 → 只有 ~79，见 `out/_g_hold*.txt` 与 `out/_ganalyze.py`）。设定高于上限时
 * 目标就一路跑远，而车上的误差是 `NormalizeAngle(目标 − 航向)`（gyro_control.c:264-269
 * 的 `CalculateAngleError`，**永远走最近路**）—— 一旦领先超过 180°，它会把"继续往左"
 * 理解成"往右 160°"，`rate_ref`/`corr` 当场翻号，车**突然倒着拐**；领先缩回 180° 以内
 * 又翻回来 ⇒ 就是用户说的"按住不动却突然拐、来回拐"。
 *
 * 实测翻号那一刻的领先量：183.4° / 201.5° / 203.9° / 209.0°（四次独立实验，见
 * `out/_g_r180.txt`、`out/_g_hold120.txt`、`out/_g_hold180.txt`、`out/_pw_before180.txt`）。
 * 卡在 140° 就永远到不了 180°；实测这条线在"车跟得上"的档位（≤90°/s，领先峰值 ~104°）
 * 碰不到，所以那些档位的行为与以前逐位相同。
 */
export const MAX_LEAD = 140
/** 转向速度滑块的上限（°/s）—— 只是"键按着时航向每秒变多少"，不是轮速。 */
export const TURN_RATE_LIMIT = 180
/** 操控页把转向速度积分成目标角度的节拍（ms）。 */
export const TURN_TICK_MS = 100

/**
 * 速度刻度的满量程。固件 `SpeedControl_SetTarget()` / `GyroControl_SetBaseSpeed()` 吃的就是
 * 这个刻度（0..7200），**不是 mm/s**：7200 对应速度环目标 25 计数/5ms ≈ 516 mm/s
 * （1980 计数/转、轮径 65mm）。`base_turn` 的 `base`、`wheels` 的左右轮目标都按它算。
 *
 * 用户 m20267 报"满速太低"就是因为早先这里只给到 3000（= 42% 满量程）。
 */
export const PWM_MAX = 7200

/**
 * 静摩擦死区上沿（速度刻度）。速度环前馈是 `PWM ≈ 2327 + 0.662 × 刻度`
 * （`HP_SPEED_FF_FRIC` / `HP_SPEED_FF_SLOPE`），刻度 1000 ⇒ 轮上约 3000 PWM —— 正好是实测的
 * 起步死区（开路试验：左轮 3000 PWM 才起转，右轮要 3500）。刻度比它低时速度环只能靠积分一点点
 * 把 PWM 顶上去，实测要好几秒才动，所以操控页把这一段当"死区"（可一键跳过）。
 */
export const DEAD_SCALE = 1000

/** 夹取；NaN 当 0（宁可不动，也不要听一个坏数），±Infinity 自然被夹到端点上。 */
export function clamp(v: number, lo: number, hi: number): number {
  if (Number.isNaN(v)) return 0
  return v < lo ? lo : v > hi ? hi : v
}

/** 收进 (-180, 180]：与固件 NormalizeAngle 同意图（角度差要走近路）。 */
export function norm180(deg: number): number {
  if (!Number.isFinite(deg)) return 0
  let d = ((deg + 180) % 360 + 360) % 360 - 180
  if (d === -180) d = 180
  return d
}

/** 目标航向 = 基准航向 + 偏移（都按 (-180,180] 报出来，方便和遥测里的 yaw 比）。 */
export function targetHeading(baseYaw: number, offset: number): number {
  return norm180(baseYaw + offset)
}

/** 角度环误差 = 目标 − 当前（正 = 还要往左转）。 */
export function headingError(targetYaw: number, yaw: number): number {
  return norm180(targetYaw - yaw)
}

/** 把目标偏移夹进 ±HOLD_LIMIT。 */
export function clampOffset(offset: number): number {
  return clamp(offset, -HOLD_LIMIT, HOLD_LIMIT)
}

/** 按转向速度积分一个节拍：`dtMs` 毫秒里航向偏移变化 `turnRate * dt/1000` 度。 */
export function offsetAfter(offset: number, turnRate: number, dtMs: number): number {
  return clampOffset(offset + (turnRate * dtMs) / 1000)
}

/** 转向速度：按了 A/← 是正（左转），按了 D/→ 是负（右转）；两边都按 = 抵消。 */
export function turnRateOf(left: boolean, right: boolean, rateLimit: number): number {
  const d = (left ? 1 : 0) - (right ? 1 : 0)
  return d * Math.abs(rateLimit)
}

/** 偏移量的人话：0 就是回正。 */
export function offsetLabel(offset: number): string {
  const v = Math.round(offset)
  if (v === 0) return '回正 0°'
  return v > 0 ? `左转 ${v}°` : `右转 ${-v}°`
}

// ------------------------------------------------------------------ 操控页状态机

export interface DriveIntent {
  /** 基准速度刻度（−PWM_MAX..+PWM_MAX，前进为正、后退为负、前后抵消为 0）。 */
  base: number
  /** 期望转向速度 °/s（正 = 左打轮：前进时车头左偏、倒车时车尾左摆）。 */
  turnRate: number
  /** 急停（空格 / Esc）：立刻停车并把偏移清零。 */
  stop: boolean
}

export interface DriveRuntime {
  /** 当前目标航向相对"基准航向"的偏移（度，正 = 左）。 */
  offset: number
  /** 车上是否还挂着上位机的驾驶指令。 */
  armed: boolean
  /** 上一次真正发出去的帧，用来去重（键按着不动时不必重复下发）。 */
  last: DriveRequest | null
  /**
   * 目标**领先车头**多少度（正 = 目标在车头左边）。用"页面发出的偏移增量 − 车实际转过的角度"
   * 累加出来（见 `stepDrive`），用来实现 `MAX_LEAD` 保护。
   */
  lead: number
  /** 上一拍看到的车头航向（遥测 `yaw`，±180 的原始值），用来算这一拍车转了多少。 */
  lastYaw: number | null
  /**
   * ★2026-10-18：**接管那一刻**的车头航向（绝对，±180）。
   * 松手时用它 + offset 算出绝对目标航向，交给固件的 hold 模式（见 stepDrive 的松手分支）。
   */
  baseYaw: number | null
}

export function initDriveRuntime(): DriveRuntime {
  return { offset: 0, armed: false, last: null, lead: 0, lastYaw: null, baseYaw: null }
}

function same(a: DriveRequest | null, b: DriveRequest): boolean {
  if (!a || a.mode !== b.mode) return false
  if (a.mode === 'off') return true
  // ★hold 比的是绝对航向 yaw，base_turn 比的是相对偏移 turn
  if (a.mode === 'hold') return a.base === b.base && a.yaw === b.yaw
  return a.base === b.base && a.turn === b.turn
}

/**
 * 一个节拍：把"按了什么键"变成"要发哪几帧"。**纯函数**，所有下发决定都在这里，
 * 页面只负责把返回的帧按顺序 await 下去。
 *
 * 规则：
 *   * 急停 ⇒ 发一帧 off，偏移清零；
 *   * 按着转向键 ⇒ 按 turnRate 积分偏移（**倒车时取反**），然后发 base_turn（基准速度 + 目标偏移）；
 *     但目标**领先车头不超过 `MAX_LEAD`**（见 MAX_LEAD 的注释：再往前拧，车上的"最近路"误差会
 *     翻号 ⇒ 车突然倒拐）。要这条保护就得把当前航向 `yawNow` 传进来；不传（老调用/单测）时
 *     保护不生效，其余行为完全不变。
 *   * 松开全部键（基准 0 且不转）⇒ 发一帧 off（交还车上任务流程），偏移清零；
 *     这样"松手再按"就等于重新对零，不会攒着一个越滚越大的目标航向。
 */
export function stepDrive(
  rt: DriveRuntime,
  intent: DriveIntent,
  dtMs: number,
  yawNow?: number,
): { rt: DriveRuntime; send: DriveRequest[] } {
  if (intent.stop) {
    return { rt: initDriveRuntime(), send: rt.armed ? [{ mode: 'off' }] : [] }
  }

  const turning = intent.turnRate !== 0

  // ★2026-10-18：接管那一刻的车头航向（绝对方位）。之后一直沿用，绝不改 ——
  //   否则"基准"会跟着车跑，目标就永远追不上（这正是 base_turn 1 秒重抓基准的老毛病）。
  let baseYaw = rt.baseYaw
  if (baseYaw === null && yawNow !== undefined && Number.isFinite(yawNow)) baseYaw = yawNow

  // 全松开 ⇒ **发改 hold（绝对目标航向），让车原地转到目标并锁住**，而不是 off。
  //   用户 m（原话）："我一松的时候，它不应该立马结束控制，就算我松了，它也应该回到这个目标角度"。
  //   为什么用 hold 而不是继续发 base_turn：base_turn 的 turn 是**相对**固件抓的基准，
  //   固件在 HOST_BASE_REGRAB_MS(1s) 后会重抓基准 ⇒ 目标被反复累加 ⇒ 震荡/反向（已实测踩过）。
  //   hold 的 yaw 是绝对量、且幂等 ⇒ 连发 N 帧 == 1 帧，安全。
  if (!turning && intent.base === 0) {
    if (rt.armed && baseYaw !== null) {
      const yaw = norm180(baseYaw + rt.offset)
      const hold: DriveRequest = { mode: 'hold', base: 0, yaw: Math.round(yaw * 10) / 10 }
      const send = same(rt.last, hold) ? [] : [hold]
      return { rt: { ...rt, baseYaw, last: hold, lead: 0 }, send }
    }
    return { rt: initDriveRuntime(), send: rt.armed ? [{ mode: 'off' }] : [] }
  }

  // 倒车时打轮方向要翻过来（用户 m20267：按住 S+D 应该往右后方走，原来却是左后方）。
  // 发出去的 `turn` 是**目标航向偏移**（正 = 偏航角增大 = 左），而车倒着走时它的行进方向是
  // (航向 + 180°)：航向转了 θ，走的其实是 (航向 + 180° + θ)。所以想让车尾往右摆，车头得往左拧
  // （θ 增大）；前进时不用翻（车头往右就是车往右）。原地转（base == 0）也不翻：A 始终是"车头往左"。
  const yawRate = intent.base < 0 ? -intent.turnRate : intent.turnRate

  let offset = turning ? offsetAfter(rt.offset, yawRate, dtMs) : rt.offset
  let lead = rt.lead
  let lastYaw = rt.lastYaw

  if (yawNow !== undefined && Number.isFinite(yawNow)) {
    // 这一拍车实际转过的角度（航向是 ±180 的原始值 ⇒ 按最近路取小量，一拍最多几十度）
    const turned = lastYaw === null ? 0 : norm180(yawNow - lastYaw)
    lead = rt.lead - turned + (offset - rt.offset)
    if (turning && Math.abs(lead) > MAX_LEAD) {
      // 领先超线：把目标**朝车拉回来**（不是只"停拧"）。为什么必须拉：车被打滑/被线缆拽/
      // 被挡住时自己会往回转，只停拧的话领先仍会被推过 180°，车上的"最近路"照旧翻号
      // （2026-10-07 实测就是这么翻的：领先 150° 时车被拽回 40° ⇒ 190° ⇒ 车倒拐）。
      // 拉回后领先恒等于 MAX_LEAD，且**方向不变**（车继续朝原方向转，只是目标贴着它走）。
      const excess = Math.abs(lead) - MAX_LEAD
      offset -= Math.sign(lead) * excess
      lead = Math.sign(lead) * MAX_LEAD
    }
    lastYaw = yawNow
  }

  const cmd: DriveRequest = { mode: 'base_turn', base: Math.round(intent.base), turn: Math.round(offset) }
  const send = same(rt.last, cmd) ? [] : [cmd]
  return { rt: { offset, lead, lastYaw, baseYaw, armed: true, last: cmd }, send }
}
