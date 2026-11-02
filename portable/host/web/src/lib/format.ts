/**
 * 格式化 / 单位换算。全部纯函数，单测覆盖（`format.test.ts`）。
 */

export function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v
}

/** 数字格式化（等宽数字用；NaN/Inf 一律显示 --） */
export function fmt(v: number | null | undefined, decimals = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return '--'
  return v.toFixed(decimals)
}

/** 带正号（用于误差、修正量这类有方向的量） */
export function fmtSigned(v: number | null | undefined, decimals = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return '--'
  const s = v.toFixed(decimals)
  return v > 0 ? `+${s}` : s
}

export function fmtInt(v: number | null | undefined): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return '--'
  return Math.round(v).toString()
}

/** 毫秒 -> 人类可读运行时长 */
export function fmtUptime(ms: number | null | undefined): string {
  if (ms === null || ms === undefined || !Number.isFinite(ms) || ms < 0) return '--'
  const s = Math.floor(ms / 1000)
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = s % 60
  if (d > 0) return `${d} 天 ${h} 时`
  if (h > 0) return `${h} 时 ${m} 分`
  if (m > 0) return `${m} 分 ${sec} 秒`
  return `${sec} 秒`
}

export function fmtClock(ms: number | null | undefined): string {
  if (!ms) return '--:--:--'
  const d = new Date(ms)
  const p = (n: number) => n.toString().padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}

/** PWM 刻度 -> 占空比百分比 */
export function pwmToPct(pwm: number, pwmMax = 7200): number {
  if (!Number.isFinite(pwm) || pwmMax <= 0) return 0
  return clamp((pwm / pwmMax) * 100, -100, 100)
}

/** 舵机脉宽 -> 角度（500..2500us ↔ 0..180°，与固件 servo.c 一致） */
export function pulseToDeg(us: number): number {
  return clamp(((us - 500) / 2000) * 180, 0, 180)
}

export function degToRad(deg: number): number {
  return (deg * Math.PI) / 180
}

export function radToDeg(rad: number): number {
  return (rad * 180) / Math.PI
}

/** 角度归一化到 [-180, 180) */
export function normDeg(deg: number): number {
  if (!Number.isFinite(deg)) return 0
  let d = deg % 360
  if (d >= 180) d -= 360
  if (d < -180) d += 360
  return d
}

/** 灰度原始值 0..255 -> 0..1 的黑度（255 = 最黑） */
export function grayToInk(v: number): number {
  return clamp(v / 255, 0, 1)
}

export function bytesToHuman(n: number): string {
  if (!Number.isFinite(n)) return '--'
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / 1024 / 1024).toFixed(2)} MB`
}

/** mm/s -> km/h（调试时看着直观） */
export function mmpsToKmh(mmps: number): number {
  return (mmps / 1000) * 3.6
}

/** 时间戳 -> 相对秒（波形 X 轴） */
export function relSec(tMs: number, t0Ms: number): number {
  return (tMs - t0Ms) / 1000
}
