/**
 * 迷你趋势（总览/IMU 页用）：直接读本地环形缓冲，不引图表库。
 * uPlot 只用在波形页；这里几十个点的 SVG 更轻、刷新也不会闪。
 */

import React from 'react'
import { CHANNELS, CHANNEL_BY_KEY } from '../lib/channels'
import { store, useHotVersion } from '../lib/store'
import { Sparkline } from './ui'

/** 通道 key -> 环形缓冲字段下标（与 CHANNELS 顺序一致） */
const ORDER = CHANNELS.map((c) => c.key)

/** 一帧窗口视图：抽稀后的点 + 整窗 min/max/末值（来自 `ring.view()` 的**同一趟**扫描） */
export interface ChannelView {
  values: Float32Array
  count: number
  min: number
  max: number
  last: number | null
}

const EMPTY: ChannelView = {
  values: new Float32Array(0),
  count: 0,
  min: Number.NaN,
  max: Number.NaN,
  last: null,
}

/**
 * 取某一路的窗口视图（抽稀后的线 + 整窗极值/末值）。
 *
 * 抽稀相位锚在**环形缓冲的绝对下标**上（`ring.view()`），不是锚在窗口数组的 `i=0` 上：
 * 窗口每滑一格就换一个起点的话，抽稀选中的样本每帧几乎全换 ⇒ 折线整条抖（用户 m22973
 * 「波形一直在闪来闪去」的根因之一）。卡片上的 min/max/当前值取的是**整窗**，不受抽稀影响；
 * 而 `snapshotField()` 会把整窗原样拷一份（20s @100Hz = 2000 点 × 每个订阅者），
 * 这些拷贝原先每个热 tick 都要重来一遍。
 *
 * 只有热订阅（`useHotVersion`），不搬整份快照 —— 一个迷你趋势线不该让整页重渲染。
 */
export function useChannelView(channel: string, windowMs = 20000, maxPoints = 120): ChannelView {
  const version = useHotVersion()
  const stepRef = React.useRef(1)

  return React.useMemo(() => {
    const idx = ORDER.indexOf(channel)
    if (idx < 0) return EMPTY
    const v = store.ring.view(windowMs, idx, maxPoints, stepRef.current)
    stepRef.current = v.step
    if (v.count === 0) return EMPTY
    return { values: v.v, count: v.count, min: v.min, max: v.max, last: v.last }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [version, windowMs, channel, maxPoints])
}

/** 只要抽稀后的折线（`Sparkline` 收 `ArrayLike<number>`，`Float32Array` 直接用，不转数组） */
export function useChannelSeries(channel: string, windowMs = 20000, maxPoints = 120): Float32Array {
  return useChannelView(channel, windowMs, maxPoints).values
}

export function MiniTrend({
  channel,
  windowMs = 20000,
  height = 40,
  color,
  className,
  maxPoints = 120,
  zeroScale = false,
}: {
  channel: string
  windowMs?: number
  height?: number
  color?: string
  /** 给了就以它为准（画出来的高度由它决定，见 Sparkline）；不给还是原来的 h-9 */
  className?: string
  maxPoints?: number
  zeroScale?: boolean
}) {
  const def = CHANNEL_BY_KEY[channel]
  const tint = color ?? def?.color ?? '#22d3ee'
  const data = useChannelSeries(channel, windowMs, maxPoints)
  return <Sparkline data={data} color={tint} height={height} className={className} zeroScale={zeroScale} />
}
