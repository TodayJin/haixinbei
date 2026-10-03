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

  /** 取全部（按时间升序），CSV 导出用 */
  all(): Snapshot {
    return this.snapshot(Number.POSITIVE_INFINITY)
  }
}
