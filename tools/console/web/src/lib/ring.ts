/**
 * 定长环形缓冲：遥测波形用它（默认 30000 点，设置页可改）。
 *
 * 设计取舍：
 *  - 每个通道一条 `Float32Array`，时间戳一条 `Float64Array`（毫秒）；
 *  - 写入 O(字段数)，不分配内存；
 *  - 只有"取窗口"时才把数据摊平成数组 —— 30s 窗口 @50Hz 才 1500 点，够轻；
 *    全量导出（CSV）才走 `all()`。
 */

export interface Snapshot {
  /** 时间戳（毫秒），长度 = count，按时间升序 */
  t: Float64Array
  /** 每个通道的值，各自长度 = count，与 t 对齐 */
  v: Float32Array[]
  count: number
}

/** 单通道窗口：波形一条泳道只用这一个 */
export interface FieldSnapshot {
  t: Float64Array
  v: Float32Array
  count: number
}

/**
 * 一条通道的"显示窗口"：抽稀后的线 + 整窗数字，**一次扫描算完**。
 *
 * 和 `snapshotField()` 的区别：这里同时给出抽稀网格的相位（`start`/`step`）和
 * 整窗的 min/max/last —— 折线要的是"画得动"，卡片上的数字要的是"准"，
 * 两者不该互相将就（抽稀后的数组算 min/max 会漏掉尖峰）。
 */
export interface FieldView {
  /** 抽稀后的时间戳（毫秒），长度 = count */
  t: Float64Array
  /** 抽稀后的值，长度 = count */
  v: Float32Array
  /** 抽稀后的点数 */
  count: number
  /** `v[0]` 在环形缓冲里的**绝对下标**（0 = 当前最旧的一条） */
  start: number
  /** 抽稀步长（绝对下标 % step === 0 的样本才会被留下） */
  step: number
  /** 窗口内的原始点数（未抽稀） */
  windowCount: number
  /** 整窗最小值（不受抽稀影响）；窗口为空时 NaN */
  min: number
  /** 整窗最大值；窗口为空时 NaN */
  max: number
  /** 窗口内最新一个值；窗口为空时 NaN */
  last: number
}

const EMPTY_VIEW: FieldView = {
  t: new Float64Array(0),
  v: new Float32Array(0),
  count: 0,
  start: 0,
  step: 1,
  windowCount: 0,
  min: Number.NaN,
  max: Number.NaN,
  last: Number.NaN,
}

/**
 * 稳定相位抽稀的步长：抽稀后点数 ≈ count/step ≤ maxPoints。
 *
 * 为什么要 `hint`（死区）：`ceil(count/maxPoints)` 在窗口边界上会在相邻两档之间来回跳，
 * 每跳一次采样网格的相位就整体挪一格 ⇒ 折线抖。给了 hint 就"能不动就不动"：
 * 只有 count 明确超过上一档容量时才调大，明确低于下一档容量 ×0.98 时才调小。
 */
export function decimationStep(count: number, maxPoints: number, hint = 1): number {
  const cap = Math.max(1, Math.floor(maxPoints))
  const need = Math.max(1, Math.ceil(count / cap))
  const prev = Math.max(1, Math.floor(hint))
  if (need > prev) return need
  const smaller = Math.max(1, prev - 1)
  return count <= cap * smaller * 0.98 ? smaller : prev
}

export class SeriesRing {
  private cap: number
  private fields: number
  private t: Float64Array
  private ch: Float32Array[]
  private head = 0 // 下一个写入位置
  private len = 0

  constructor(capacity: number, fields: number) {
    if (capacity < 1) throw new Error('容量必须 ≥ 1')
    if (fields < 1) throw new Error('通道数必须 ≥ 1')
    this.cap = Math.floor(capacity)
    this.fields = Math.floor(fields)
    this.t = new Float64Array(this.cap)
    this.ch = Array.from({ length: this.fields }, () => new Float32Array(this.cap))
  }

  get capacity(): number {
    return this.cap
  }

  get size(): number {
    return this.len
  }

  get fieldCount(): number {
    return this.fields
  }

  clear(): void {
    this.head = 0
    this.len = 0
  }

  /** 改容量：保留最近的数据 */
  resize(capacity: number): void {
    const next = Math.max(1, Math.floor(capacity))
    if (next === this.cap) return
    const keep = Math.min(this.len, next)
    const nt = new Float64Array(next)
    const nch = Array.from({ length: this.fields }, () => new Float32Array(next))
    for (let k = 0; k < keep; k++) {
      const src = (this.head - keep + k + this.cap * 2) % this.cap
      nt[k] = this.t[src]
      for (let f = 0; f < this.fields; f++) nch[f][k] = this.ch[f][src]
    }
    this.cap = next
    this.t = nt
    this.ch = nch
    this.head = keep % next
    this.len = keep
  }

  /** 压入一帧（values 长度不足按 0 补齐，超出丢弃） */
  push(tMs: number, values: readonly number[]): void {
    this.t[this.head] = tMs
    for (let f = 0; f < this.fields; f++) {
      const v = f < values.length ? values[f] : 0
      this.ch[f][this.head] = Number.isFinite(v) ? v : 0
    }
    this.head = (this.head + 1) % this.cap
    if (this.len < this.cap) this.len++
  }

  /** 第 k 个"逻辑下标"（0 = 最旧）对应的物理下标 */
  private phys(k: number): number {
    return (this.head - this.len + k + this.cap * 2) % this.cap
  }

  /** 取最近 windowMs 毫秒的**起止下标**（二分定位，snapshot/snapshotField 共用） */
  private range(windowMs: number, nowMs?: number): { lo: number; count: number } {
    if (this.len === 0) return { lo: 0, count: 0 }
    const lastIdx = this.phys(this.len - 1)
    const end = nowMs === undefined ? this.t[lastIdx] : nowMs
    const from = end - Math.max(0, windowMs)

    // 二分找第一个 >= from 的位置（数据在环形里是升序的）
    let lo = 0
    let hi = this.len
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if (this.t[this.phys(mid)] < from) lo = mid + 1
      else hi = mid
    }
    return { lo, count: this.len - lo }
  }

  /** 取最近 windowMs 毫秒（相对最后一帧时间戳），按时间升序 */
  snapshot(windowMs: number, nowMs?: number): Snapshot {
    if (this.len === 0) {
      return { t: new Float64Array(0), v: Array.from({ length: this.fields }, () => new Float32Array(0)), count: 0 }
    }
    const { lo, count } = this.range(windowMs, nowMs)
    const t = new Float64Array(count)
    const v = Array.from({ length: this.fields }, () => new Float32Array(count))
    for (let k = 0; k < count; k++) {
      const p = this.phys(lo + k)
      t[k] = this.t[p]
      for (let f = 0; f < this.fields; f++) v[f][k] = this.ch[f][p]
    }
    return { t, v, count }
  }

  /**
   * 只取一个通道 —— 波形每条泳道画一根线就用它。
   * `snapshot()` 会把全部通道都拷一份（21 个通道 × count），泳道只用一个通道时白拷 20 份。
   */
  snapshotField(windowMs: number, fieldIndex: number, nowMs?: number): FieldSnapshot {
    const f = Math.max(0, Math.min(this.fields - 1, Math.floor(fieldIndex)))
    const { lo, count } = this.range(windowMs, nowMs)
    const t = new Float64Array(count)
    const v = new Float32Array(count)
    const src = this.ch[f]
    for (let k = 0; k < count; k++) {
      const p = this.phys(lo + k)
      t[k] = this.t[p]
      v[k] = src[p]
    }
    return { t, v, count }
  }

  /**
   * 显示窗口 = **稳定相位**抽稀 + 整窗极值/末值，一次扫描。
   *
   * 相位锚在**绝对下标**上（只留 `absIdx % step === 0` 的样本），不是锚在窗口数组的
   * `i=0` 上：窗口每滑一格，`snapshotField()[0]` 就换一个样本，从 `i=0` 起步的抽稀网格
   * 会让**每个**采样点都换样本 ⇒ 折线整条抖（用户 m22973「波形一直在闪来闪去」的根因之一）。
   * 锚绝对下标后，两次窗口重叠部分的采样点一个不动，只有两端进出。
   */
  view(windowMs: number, fieldIndex: number, maxPoints: number, stepHint = 1, nowMs?: number): FieldView {
    const f = Math.max(0, Math.min(this.fields - 1, Math.floor(fieldIndex)))
    const { lo, count } = this.range(windowMs, nowMs)
    if (count === 0) return EMPTY_VIEW
    const src = this.ch[f]

    // 一整趟：极值 + 末值（数字要准，不受抽稀影响）
    let min = Number.POSITIVE_INFINITY
    let max = Number.NEGATIVE_INFINITY
    for (let k = 0; k < count; k++) {
      const val = src[this.phys(lo + k)]
      if (val < min) min = val
      if (val > max) max = val
    }
    const last = src[this.phys(lo + count - 1)]

    const step = decimationStep(count, maxPoints, stepHint)
    // 第一个落在绝对网格上的偏移：使 (lo + off) % step === 0
    const off = (step - (lo % step)) % step
    const n = off >= count ? 0 : Math.floor((count - 1 - off) / step) + 1
    const t = new Float64Array(n)
    const v = new Float32Array(n)
    for (let k = 0; k < n; k++) {
      const p = this.phys(lo + off + k * step)
      t[k] = this.t[p]
      v[k] = src[p]
    }
    return { t, v, count: n, start: lo + off, step, windowCount: count, min, max, last }
  }

  /**
   * 窗口内最新一个值 —— O(1)，**不拷数组**。
   * 卡片/泳道角上的数字原来走 `snapshotField(...).v` 再取最后一个：20Hz × 每条泳道
   * 白拷一整窗口（波形页 5 条 = 5005 点/帧），纯浪费。窗口空给 null。
   *
   * `range()` 的右端永远是最后一帧（`nowMs` 只决定**左端**往哪裁），所以取
   * `lo + count - 1` 与取全局最新等价 —— 写出来是为了语义诚实：取的是"窗口内"的末值。
   */
  lastValue(fieldIndex: number, windowMs = Number.POSITIVE_INFINITY, nowMs?: number): number | null {
    const f = Math.max(0, Math.min(this.fields - 1, Math.floor(fieldIndex)))
    const { lo, count } = this.range(windowMs, nowMs)
    if (count === 0) return null
    return this.ch[f][this.phys(lo + count - 1)]
  }

  /** 取全部（按时间升序） */
  all(): Snapshot {
    return this.snapshot(Number.POSITIVE_INFINITY)
  }
}
