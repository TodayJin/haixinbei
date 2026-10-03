/**
 * 波形：**每通道独立量程的泳道**（stacked mini-plots），基于 uPlot。
 *
 * 为什么要一条通道一个 uPlot：PWM(±7200) / 航向(±180) / 电流(0~2500) 共用一个
 * Y 轴会把小信号压成直线 —— 这是旧上位机被用户说"不好用"的点之一。
 *
 * 数据来自 `store.ring`（本地环形缓冲），不依赖后端追加。
 */

import React from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'
import { CHANNEL_BY_KEY, CHANNELS } from '../lib/channels'
import { store, useStore } from '../lib/store'

/** channel key -> 环形缓冲里的字段下标（与 CHANNELS 顺序一致） */
const CHANNEL_KEYS = CHANNELS.map((c) => c.key)
function channelIndexOf(key: string): number {
  return CHANNEL_KEYS.indexOf(key)
}

interface LaneProps {
  channelKey: string
  windowMs: number
  paused: boolean
  height?: number
  onHover?: (key: string, idx: number | null, tSec: number | null, value: number | null) => void
}

function ScopeLane({ channelKey, windowMs, paused, height = 96, onHover }: LaneProps) {
  const def = CHANNEL_BY_KEY[channelKey]
  const hostRef = React.useRef<HTMLDivElement | null>(null)
  const plotRef = React.useRef<uPlot | null>(null)
  const dataRef = React.useRef<uPlot.AlignedData>([new Float64Array(0), new Float32Array(0)])
  const { version, ringFill } = useStore()

  // 建图（只建一次）
  React.useEffect(() => {
    if (!hostRef.current || plotRef.current) return
    const opts: uPlot.Options = {
      width: hostRef.current.clientWidth || 600,
      height,
      pxAlign: 0,
      legend: { show: false },
      cursor: {
        y: false,
        points: { size: 5 },
        drag: { x: false, y: false },
      },
      scales: {
        x: { time: false },
        y: { auto: true },
      },
      axes: [
        {
          stroke: 'rgba(148,163,184,0.6)',
          grid: { stroke: 'rgba(148,163,184,0.10)', width: 1 },
          ticks: { stroke: 'rgba(148,163,184,0.2)' },
          font: '10px "Cascadia Mono", Consolas, monospace',
          size: 22,
        },
        {
          stroke: 'rgba(148,163,184,0.6)',
          grid: { stroke: 'rgba(148,163,184,0.10)', width: 1 },
          ticks: { stroke: 'rgba(148,163,184,0.2)' },
          font: '10px "Cascadia Mono", Consolas, monospace',
          size: 46,
        },
      ],
      series: [
        {},
        {
          stroke: def?.color ?? '#22d3ee',
          width: 1.4,
          fill: `${def?.color ?? '#22d3ee'}22`,
          points: { show: false },
        },
      ],
      hooks: {
        setCursor: [
          (u) => {
            const idx = u.cursor.idx
            if (!onHover) return
            if (idx === null || idx === undefined || idx < 0) {
              onHover(channelKey, null, null, null)
              return
            }
            const t = u.data[0][idx]
            const v = u.data[1][idx]
            onHover(channelKey, idx, typeof t === 'number' ? t : null, typeof v === 'number' ? v : null)
          },
        ],
      },
    }
    const u = new uPlot(opts, dataRef.current, hostRef.current)
    plotRef.current = u
    const ro = new ResizeObserver(() => {
      const w = hostRef.current?.clientWidth
      if (w && w > 10) u.setSize({ width: w, height })
    })
    ro.observe(hostRef.current)
    return () => {
      ro.disconnect()
      u.destroy()
      plotRef.current = null
    }
    // 只建一次：通道/窗口变化由下面的 effect 负责 setData
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [channelKey, height])

  // 推数据（受 store 的 10Hz 节流驱动；暂停时冻住 = 不 setData）
  React.useEffect(() => {
    const u = plotRef.current
    if (!u) return
    if (paused) return
    const snap = store.ring.snapshot(windowMs)
    if (snap.count < 2) return
    const t0 = snap.t[0]
    const xs = new Float64Array(snap.count)
    for (let i = 0; i < snap.count; i++) xs[i] = (snap.t[i] - t0) / 1000
    const idx = channelIndexOf(channelKey)
    if (idx < 0) return
    const data: uPlot.AlignedData = [xs, snap.v[idx]]
    dataRef.current = data
    u.setData(data, false)
    // y 轴自动量程：加一点余量，避免贴边
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [version, ringFill, windowMs, paused, channelKey])

  const last = React.useMemo(() => {
    const snap = store.ring.snapshot(windowMs)
    if (snap.count === 0) return null
    const f = channelIndexOf(channelKey)
    if (f < 0) return null
    const arr = snap.v[f]
    return arr.length ? arr[arr.length - 1] : null
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [version, windowMs, channelKey])

  return (
    <div className="glass p-3">
      <div className="mb-2 flex items-baseline justify-between">
        <div className="flex items-center gap-2">
          <span className="h-2.5 w-2.5 rounded-sm" style={{ background: def?.color }} />
          <span className="text-[13px] text-slate-200">{def?.label ?? channelKey}</span>
          <span className="text-[11px] text-slate-500">{def?.unit}</span>
        </div>
        <span className="num text-lg font-semibold" style={{ color: def?.color }}>
          {last === null ? '--' : last.toFixed(def?.decimals ?? 1)}
        </span>
      </div>
      <div ref={hostRef} className="w-full" />
    </div>
  )
}

/** channel key -> ring 里的字段下标（与 CHANNELS 顺序一致） */

export interface CursorReadout {
  key: string
  tSec: number | null
  value: number | null
}

export function ScopeBoard({
  windowMs,
  paused,
  onHover,
}: {
  windowMs: number
  paused: boolean
  onHover?: (r: CursorReadout) => void
}) {
  const { settings } = useStore()
  const keys = settings.scopeChannels.length ? settings.scopeChannels : ['yaw']
  return (
    <div className="grid gap-3">
      {keys.map((k) => (
        <ScopeLane
          key={k}
          channelKey={k}
          windowMs={windowMs}
          paused={paused}
          onHover={(key, _idx, tSec, value) => onHover?.({ key, tSec, value })}
        />
      ))}
    </div>
  )
}
