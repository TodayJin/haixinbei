/**
 * 迷你折线的纵轴量程：**放大即时、收缩缓慢**（包络式滞回）。
 *
 * 为什么需要：`Sparkline` 原来每个 tick 都按当前窗口的极值重算量程。窗口每滑一格，
 * 极值就可能差一点点 ⇒ 整条曲线上下重新缩放 ⇒ 看起来"一直在闪"（用户 m22973 的
 * 另一个候选根因）。`zeroScale`（姿态角：0 必须居中）时还要求左右对称，抖动更明显。
 *
 * 规则（每侧独立，保证**绝不裁掉**当前窗口）：
 *  - 原始量程要往外扩 → 立刻扩（不能裁数据）；
 *  - 要往里收 → 每 tick 只朝目标挪 `decay`（默认 2%，20Hz 下 ≈ 每秒 1/3）；
 *  - 幅度掉得太多（不到原来的 `snap`）→ 直接贴合，别让老的量程长时间把曲线压扁。
 *
 * 效果：量程 ≈ 滑动窗口极值的"峰值包络"，只在幅度真的变大时动，看起来是稳的。
 * 纯函数，单测见 `sparkline.test.ts`。
 */

export interface ValueRange {
  lo: number
  hi: number
}

/** 每 tick 朝目标收缩的比例（越小越稳、越慢跟上） */
export const RANGE_DECAY = 0.02
/** 目标幅度不足当前幅度的这个比例时，直接贴合（快速脱离老的大范围） */
export const RANGE_SNAP = 0.55

/** 数出极值。`Math.min(...data)` 在几千点的数组上会把实参表撑爆，这里一律循环。 */
export function extent(values: ArrayLike<number>): { min: number; max: number } {
  let min = Number.POSITIVE_INFINITY
  let max = Number.NEGATIVE_INFINITY
  for (let i = 0; i < values.length; i++) {
    const v = values[i]
    if (v < min) min = v
    if (v > max) max = v
  }
  if (values.length === 0) return { min: Number.NaN, max: Number.NaN }
  return { min, max }
}

/**
 * 当前窗口的"原始"量程。
 * `zeroScale` = 以 0 为中心对称（姿态角用）：自动缩放会把 0 附近的抖动放大成满屏。
 */
export function rawRange(values: ArrayLike<number>, zeroScale = false): ValueRange {
  const { min, max } = extent(values)
  if (!Number.isFinite(min) || !Number.isFinite(max)) return { lo: -1, hi: 1 }
  if (!zeroScale) return { lo: min, hi: max }
  const a = Math.max(Math.abs(min), Math.abs(max), 1e-6)
  return { lo: -a, hi: a }
}

/** 上一 tick 的量程 -> 这一 tick 该用的量程（滞回/包络） */
export function nextRange(
  prev: ValueRange | null,
  raw: ValueRange,
  decay = RANGE_DECAY,
  snap = RANGE_SNAP,
): ValueRange {
  if (!prev || !Number.isFinite(prev.lo) || !Number.isFinite(prev.hi) || prev.hi <= prev.lo) {
    return { lo: raw.lo, hi: raw.hi }
  }
  const prevSpan = prev.hi - prev.lo
  const rawSpan = raw.hi - raw.lo
  if (!(rawSpan > 0)) return { lo: raw.lo, hi: raw.hi }
  if (rawSpan < prevSpan * snap) return { lo: raw.lo, hi: raw.hi } // 幅度掉太多：贴合
  // 往外扩立刻扩（绝不裁数据）；往里收慢慢收
  const lo = raw.lo < prev.lo ? raw.lo : prev.lo + (raw.lo - prev.lo) * decay
  const hi = raw.hi > prev.hi ? raw.hi : prev.hi + (raw.hi - prev.hi) * decay
  return { lo, hi }
}
