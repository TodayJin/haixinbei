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
import { nextRange, type ValueRange } from '../lib/sparkline'
import { store, useHotSelect, useHotVersion, useMeta } from '../lib/store'

/** channel key -> 环形缓冲里的字段下标（与 CHANNELS 顺序一致） */
const CHANNEL_KEYS = CHANNELS.map((c) => c.key)
function channelIndexOf(key: string): number {
  return CHANNEL_KEYS.indexOf(key)
}

/**
 * 一根泳道交给 uPlot 的最大点数。
 *
 * 原来把整个窗口（20s @100Hz = 2000 点）原样塞进去，5 条泳道 = 每帧 1 万点重新
 * 布点 + 重新计算。屏幕就 1000 多像素宽，800 点已经比像素还密；再用
 * `ring.view()` 的稳定相位抽稀，窗口滑动时重叠部分的采样点一个不动。
 */
const LANE_MAX_POINTS = 800

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
  // 窄订阅：只要热 tick 号 + 缓冲水位（标量），不要整份快照
  const version = useHotVersion()
  const ringFill = useHotSelect((s) => s.ringFill)
  const stepRef = React.useRef(1)
  // 纵轴量程的滞回包络（`lib/sparkline.ts` 的 `nextRange`）：幅度真的变大才立刻往外扩，
  // 往里收很慢。不管的话 uPlot 每 tick 按当前窗口极值重算 —— 极值差一点点整条线就
  // 上下重标，看起来就是"已经走过的那段在跳"（用户 m30822）。
  const rangeRef = React.useRef<ValueRange | null>(null)
  // x 轴右端固定成窗口长度（窗口选择变了由推数据 effect 更新）
  const winRef = React.useRef(windowMs)

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
        // x 钉在 [0, 窗口长度]：数据的时间零点取"窗口右端 - windowMs"（见推数据 effect），
        // 右端固定、曲线平滑左滑。交给 uPlot 自己 auto 的话，每 tick 都会按数据极值
        // 微调横轴（数据首点随抽稀相位变），看着也在抖。
        x: { time: false, auto: true, range: () => [0, winRef.current / 1000] },
        // 纵轴量程走 `rangeRef` 的滞回包络；`auto: true` 是为了让 uPlot 把 dataMin/dataMax
        // 交给下面的 range 回调（`auto: false` 时它根本不调 range，量程永远空）。
        y: {
          auto: true,
          range: (_u, dataMin, dataMax) => {
            const r = rangeRef.current
            return r ? [r.lo, r.hi] : [dataMin, dataMax]
          },
        },
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

  // 推数据（受 store 的 20Hz 合并刷新驱动；暂停时冻住 = 不 setData）
  React.useEffect(() => {
    const u = plotRef.current
    if (!u) return
    if (paused) return
    const idx = channelIndexOf(channelKey)
    if (idx < 0) return
    // 一趟扫描：抽稀后的线（稳定相位）+ 整窗极值；不再整窗拷贝 2000 点
    const view = store.ring.view(windowMs, idx, LANE_MAX_POINTS, stepRef.current)
    stepRef.current = view.step
    if (view.count < 2) return
    winRef.current = windowMs
    // x 的时间零点取**窗口右端 - 窗口长度**，不是第一个抽稀点的时间：右端固定、曲线整体
    // 平滑左滑。用 `view.t[0]` 的话，每换一档抽稀相位整条线就横移一格（边界上跳一下）。
    const t0 = (Number.isFinite(view.endT) ? view.endT : view.t[view.count - 1]) - windowMs
    const xs = new Float64Array(view.count)
    for (let i = 0; i < view.count; i++) xs[i] = (view.t[i] - t0) / 1000
    // 纵轴目标量程用**整窗**极值：`view.min/max` 是未抽稀那一趟扫出来的，抽稀数组算会漏尖峰。
    // 全平（max == min）时给点余量，免得量程塌成一条线。
    const pad = view.max > view.min ? 0 : Math.max(1, Math.abs(view.max) * 0.1)
    rangeRef.current = nextRange(rangeRef.current, { lo: view.min - pad, hi: view.max + pad })
    const data: uPlot.AlignedData = [xs, view.v]
    dataRef.current = data
    // resetScales 必须是 true：建图时是空数据，量程还是 [null, null]，
    // 传 false 就永远按空量程画 —— 画布在、坐标轴和曲线一条都没有（第一版起就空着）。
    u.setData(data)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [version, ringFill, windowMs, paused, channelKey])

  const last = React.useMemo(() => {
    const f = channelIndexOf(channelKey)
    if (f < 0) return null
    // O(1)：取窗口内最新一个样本，不拷数组
    return store.ring.lastValue(f, windowMs)
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
  onHover?: (r: CursorReadout | null) => void
}) {
  // 低频：通道开关是设置（meta），页面壳不该跟着 20Hz 的遥测重渲染
  const { settings } = useMeta()
  const keys = settings.scopeChannels.length ? settings.scopeChannels : ['yaw']
  return (
    <div className="grid gap-3">
      {keys.map((k) => (
        <ScopeLane
          key={k}
          channelKey={k}
          windowMs={windowMs}
          paused={paused}
          // 指针离开画布（idx = null）要清掉读数，否则页眉一直挂着上一条泳道的 "t --s"
          onHover={(key, _idx, tSec, value) => onHover?.(tSec === null ? null : { key, tSec, value })}
        />
      ))}
    </div>
  )
}
