/**
 * 迷你趋势（总览/IMU 页用）：直接读本地环形缓冲，不引图表库。
 * uPlot 只用在波形页；这里几十个点的 SVG 更轻、刷新也不会闪。
 */

import React from 'react'
import { CHANNELS, CHANNEL_BY_KEY } from '../lib/channels'
import { store, useStore } from '../lib/store'
import { Sparkline } from './ui'

/** 通道 key -> 环形缓冲字段下标（与 CHANNELS 顺序一致） */
const ORDER = CHANNELS.map((c) => c.key)

/**
 * 取某一路的窗口数据（抽稀后）。抽稀到最多 `maxPoints` 点，SVG 才不费。
 * 单独导出是因为"波形卡"除了画线还要算窗口内的最小/最大/当前值。
 */
export function useChannelSeries(channel: string, windowMs = 20000, maxPoints = 120): number[] {
  const { version, ringFill } = useStore()

  return React.useMemo(() => {
    const idx = ORDER.indexOf(channel)
    if (idx < 0) return [] as number[]
    const snap = store.ring.snapshot(windowMs)
    const arr = snap.v[idx]
    if (!arr || arr.length === 0) return [] as number[]
    const step = Math.max(1, Math.floor(arr.length / maxPoints))
    const out: number[] = []
    for (let i = 0; i < arr.length; i += step) out.push(arr[i])
    return out
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [version, ringFill, windowMs, channel, maxPoints])
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
