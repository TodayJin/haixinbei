/**
 * 基础 UI 组件（全部无第三方依赖，样式用 Tailwind + 少量 SVG 自绘）。
 * 视觉语言：玻璃拟态卡片、霓虹强调、等宽数字、150ms 过渡。
 */

import React from 'react'
import { clamp } from '../lib/format'

export type Tone = 'cyan' | 'violet' | 'mint' | 'amber' | 'rose' | 'slate'

const TONE_TEXT: Record<Tone, string> = {
  cyan: 'text-neon-cyan',
  violet: 'text-neon-violet',
  mint: 'text-neon-mint',
  amber: 'text-neon-amber',
  rose: 'text-neon-rose',
  slate: 'text-slate-300',
}

const TONE_RING: Record<Tone, string> = {
  cyan: 'ring-neon-cyan/30 bg-neon-cyan/10 text-neon-cyan',
  violet: 'ring-neon-violet/30 bg-neon-violet/10 text-neon-violet',
  mint: 'ring-neon-mint/30 bg-neon-mint/10 text-neon-mint',
  amber: 'ring-neon-amber/30 bg-neon-amber/10 text-neon-amber',
  rose: 'ring-neon-rose/30 bg-neon-rose/10 text-neon-rose',
  slate: 'ring-white/15 bg-white/5 text-slate-300',
}

export function Card({
  title,
  right,
  children,
  className = '',
  dense = false,
}: {
  title?: React.ReactNode
  right?: React.ReactNode
  children: React.ReactNode
  className?: string
  dense?: boolean
}) {
  return (
    <section className={`glass animate-riseIn ${dense ? 'p-3' : 'p-4'} ${className}`}>
      {(title || right) && (
        <header className="mb-3 flex items-center justify-between gap-2">
          <h2 className="text-[13px] font-medium tracking-wide text-slate-300">{title}</h2>
          {right}
        </header>
      )}
      {children}
    </section>
  )
}

export function Stat({
  label,
  value,
  unit,
  tone = 'slate',
  hint,
  title,
  size = 'md',
}: {
  label: React.ReactNode
  value: React.ReactNode
  unit?: string
  tone?: Tone
  hint?: React.ReactNode
  /** 鼠标悬停时的一句话解释（英文缩写尽量靠它讲清楚） */
  title?: string
  size?: 'sm' | 'md' | 'lg'
}) {
  const sizeCls = size === 'lg' ? 'text-3xl' : size === 'sm' ? 'text-base' : 'text-xl'
  return (
    <div className="min-w-0" title={title}>
      <div className="truncate text-[11px] uppercase tracking-wider text-slate-400">{label}</div>
      <div className={`num ${sizeCls} font-semibold leading-tight ${TONE_TEXT[tone]}`}>
        {value}
        {unit && <span className="ml-1 text-xs font-normal text-slate-400">{unit}</span>}
      </div>
      {hint && <div className="mt-0.5 truncate text-[11px] text-slate-500">{hint}</div>}
    </div>
  )
}

export function Badge({
  tone = 'slate',
  title,
  children,
}: {
  tone?: Tone
  /** 鼠标悬停时的一句话解释（界面上尽量少摆英文缩写，靠它讲清楚） */
  title?: string
  children: React.ReactNode
}) {
  return (
    <span className={`chip ${TONE_RING[tone]}`} title={title}>
      {children}
    </span>
  )
}

export function StatusDot({ on, tone = 'mint', pulse = false }: { on: boolean; tone?: Tone; pulse?: boolean }) {
  const bg = on
    ? { cyan: 'bg-neon-cyan', violet: 'bg-neon-violet', mint: 'bg-neon-mint', amber: 'bg-neon-amber', rose: 'bg-neon-rose', slate: 'bg-slate-400' }[tone]
    : 'bg-slate-600'
  return <span className={`inline-block h-2 w-2 rounded-full ${bg} ${on && pulse ? 'animate-pulseDot' : ''}`} />
}

export function Toggle({
  on,
  onChange,
  label,
  disabled = false,
}: {
  on: boolean
  onChange: (v: boolean) => void
  label?: React.ReactNode
  disabled?: boolean
}) {
  return (
    <button
      type="button"
      disabled={disabled}
      onClick={() => onChange(!on)}
      className="inline-flex items-center gap-2 text-sm text-slate-300 disabled:opacity-40"
    >
      <span
        className={`relative h-5 w-9 rounded-full ring-1 transition ${
          on ? 'bg-neon-cyan/30 ring-neon-cyan/50' : 'bg-white/5 ring-white/15'
        }`}
      >
        <span
          className={`absolute top-0.5 h-4 w-4 rounded-full bg-white transition-all ${
            on ? 'left-[18px] shadow-glow' : 'left-0.5'
          }`}
        />
      </span>
      {label}
    </button>
  )
}

export function NumberField({
  value,
  onChange,
  min,
  max,
  step = 1,
  decimals = 0,
  unit,
  disabled = false,
  className = '',
}: {
  value: number
  onChange: (v: number) => void
  min?: number
  max?: number
  step?: number
  decimals?: number
  unit?: string
  disabled?: boolean
  className?: string
}) {
  const [text, setText] = React.useState(String(value.toFixed(decimals)))
  const focused = React.useRef(false)

  React.useEffect(() => {
    if (!focused.current) setText(value.toFixed(decimals))
  }, [value, decimals])

  const commit = (raw: string) => {
    let v = Number(raw)
    if (!Number.isFinite(v)) v = value
    if (min !== undefined) v = Math.max(min, v)
    if (max !== undefined) v = Math.min(max, v)
    setText(v.toFixed(decimals))
    onChange(v)
  }

  return (
    <span className={`inline-flex items-center gap-1 ${className}`}>
      <input
        className="field w-20 text-right"
        value={text}
        disabled={disabled}
        onFocus={() => (focused.current = true)}
        onBlur={(e) => {
          focused.current = false
          commit(e.target.value)
        }}
        onChange={(e) => setText(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur()
          if (e.key === 'ArrowUp') {
            e.preventDefault()
            commit(String(Number(text) + step))
          }
          if (e.key === 'ArrowDown') {
            e.preventDefault()
            commit(String(Number(text) - step))
          }
        }}
      />
      {unit && <span className="w-8 text-[11px] text-slate-500">{unit}</span>}
    </span>
  )
}

export function Slider({
  value,
  min,
  max,
  step = 1,
  onChange,
  disabled = false,
  className = '',
}: {
  value: number
  min: number
  max: number
  step?: number
  onChange: (v: number) => void
  disabled?: boolean
  className?: string
}) {
  return (
    <input
      type="range"
      className={`h-1.5 w-full cursor-pointer appearance-none rounded-full bg-white/10 accent-neon-cyan ${className}`}
      min={min}
      max={max}
      step={step}
      value={value}
      disabled={disabled}
      onChange={(e) => onChange(Number(e.target.value))}
    />
  )
}

/**
 * 8 路灰度阵列（总览/传感器页）：`line` = 压线标志，`gray` = 原始值 0..255（255 = 最黑）。
 *
 * 柱高就是原始值本身，所以**没黑到满量程时柱顶会留一段深色**——那是"到 255 还差多少"的
 * 余量，不是另一个元素（一度被当成 bug 问过）。加 `hint` 时在下面画一行图例把这件事写清楚。
 */
export function GrayArray({
  line,
  gray,
  height = 96,
  hint = false,
}: {
  line: number[]
  gray: number[]
  height?: number
  hint?: boolean
}) {
  return (
    <div>
      <div className="flex items-stretch gap-1.5" style={{ height }}>
        {Array.from({ length: 8 }, (_, i) => {
          const v = gray[i] ?? 0
          const active = (line[i] ?? 0) !== 0
          const ink = clamp(v / 255, 0, 1)
          return (
            <div key={i} className="flex flex-1 flex-col items-center gap-1">
              <div
                className={`relative w-full flex-1 overflow-hidden rounded-lg bg-ink-900/80 ring-1 ${
                  active ? 'ring-neon-mint/40' : 'ring-white/10'
                }`}
              >
                <div
                  className={`absolute bottom-0 left-0 w-full transition-[height] duration-150 ${
                    active ? 'bg-neon-mint/80' : 'bg-slate-500/35'
                  }`}
                  style={{ height: `${Math.max(3, ink * 100)}%` }}
                >
                  {/* 亮边画在**填充顶部**（柱头），不要画在轨道顶部 */}
                  <div className={`absolute inset-x-0 top-0 h-0.5 ${active ? 'bg-neon-mint' : 'bg-slate-400/50'}`} />
                </div>
              </div>
              <span className={`num text-[10px] ${active ? 'text-neon-mint' : 'text-slate-500'}`}>{i}</span>
              <span className="num text-[10px] text-slate-400">{Math.round(v)}</span>
            </div>
          )
        })}
      </div>
      {hint && (
        <div className="mt-2 flex flex-wrap items-center gap-x-4 gap-y-1 text-[11px] text-slate-500">
          <span>柱高 = 灰度原始值 0..255（越高越黑）</span>
          <span className="flex items-center gap-1">
            <i className="inline-block h-2.5 w-2.5 rounded-sm bg-neon-mint/80 align-middle" />
            绿色 = 该路判为压线
          </span>
          <span>柱顶深色的一段 = 到满量程 255 还差的余量</span>
        </div>
      )}
    </div>
  )
}

/** 单路灰度柱（0..255 -> 黑度），带"压线"标记 */
export function GrayBar({ value, active, label }: { value: number; active: boolean; label: string }) {
  const ink = clamp(value / 255, 0, 1)
  return (
    <div className="flex flex-1 flex-col items-center gap-1">
      <div className="relative h-24 w-full overflow-hidden rounded-lg bg-ink-900/80 ring-1 ring-white/10">
        <div
          className={`absolute bottom-0 left-0 w-full transition-[height] duration-150 ${
            active ? 'bg-neon-mint/80' : 'bg-slate-500/40'
          }`}
          style={{ height: `${Math.max(3, ink * 100)}%` }}
        />
        {active && <div className="absolute inset-x-0 top-0 h-0.5 bg-neon-mint" />}
      </div>
      <span className="num text-[10px] text-slate-500">{label}</span>
      <span className="num text-[10px] text-slate-400">{Math.round(value)}</span>
    </div>
  )
}

/**
 * 迷你趋势线（总览/IMU 页用；SVG，不引图表库）。
 *
 * `className` 决定**画出来的高度**（viewBox 里的 height 只是坐标刻度，靠 preserveAspectRatio
 * 拉伸），默认 h-9 是各处原来的大小；IMU 页的波形卡给更大的类。
 * `zeroScale` = 纵轴以 0 为中心对称（姿态角用：抬头/低头、左倾/右倾不能靠自动缩放看，
 * 自动缩放会把 0 附近的抖动放大成满屏，看着像在剧烈晃）。
 */
export function Sparkline({
  data,
  color = '#22d3ee',
  height = 34,
  fill = true,
  className = 'h-9 w-full',
  zeroScale = false,
}: {
  data: number[]
  color?: string
  height?: number
  fill?: boolean
  className?: string
  zeroScale?: boolean
}) {
  if (data.length < 2)
    return (
      <div className={`${className} flex items-center justify-center text-[11px] text-slate-600`}>等待数据…</div>
    )
  const min = Math.min(...data)
  const max = Math.max(...data)
  const lo = zeroScale ? -Math.max(Math.abs(min), Math.abs(max), 1e-6) : min
  const hi = zeroScale ? -lo : max
  const span = hi - lo || 1
  const w = 100
  const pts = data.map((v, i) => {
    const x = (i / (data.length - 1)) * w
    const y = height - ((v - lo) / span) * (height - 4) - 2
    return `${x.toFixed(2)},${y.toFixed(2)}`
  })
  return (
    <svg viewBox={`0 0 ${w} ${height}`} preserveAspectRatio="none" className={className}>
      {fill && (
        <polygon
          points={`0,${zeroScale ? height / 2 : height} ${pts.join(' ')} ${w},${zeroScale ? height / 2 : height}`}
          fill={color}
          opacity={0.12}
        />
      )}
      <polyline points={pts.join(' ')} fill="none" stroke={color} strokeWidth={1.2} vectorEffect="non-scaling-stroke" />
    </svg>
  )
}

/** 姿态仪表（yaw 圆盘 + pitch/roll 弧），纯 SVG */
export function AttitudeGauge({ yaw, pitch, roll, size = 190 }: { yaw: number; pitch: number; roll: number; size?: number }) {
  const r = size / 2 - 12
  const cx = size / 2
  const cy = size / 2
  const toXY = (deg: number, radius: number) => {
    // 屏幕坐标：0° 朝上，顺时针为正
    const rad = ((deg - 90) * Math.PI) / 180
    return [cx + radius * Math.cos(rad), cy + radius * Math.sin(rad)]
  }
  const [nx, ny] = toXY(yaw, r - 8)
  const ticks = Array.from({ length: 12 }, (_, i) => i * 30)

  return (
    <svg viewBox={`0 0 ${size} ${size}`} className="w-full max-w-[240px]">
      <defs>
        <radialGradient id="gaugeGlow" cx="50%" cy="50%">
          <stop offset="0%" stopColor="#22d3ee" stopOpacity="0.20" />
          <stop offset="70%" stopColor="#22d3ee" stopOpacity="0.02" />
          <stop offset="100%" stopColor="#22d3ee" stopOpacity="0" />
        </radialGradient>
      </defs>
      <circle cx={cx} cy={cy} r={r + 10} fill="url(#gaugeGlow)" />
      <circle cx={cx} cy={cy} r={r} fill="none" stroke="rgba(148,163,184,0.25)" strokeWidth="1" />
      <circle cx={cx} cy={cy} r={r * 0.62} fill="none" stroke="rgba(148,163,184,0.16)" strokeWidth="1" strokeDasharray="3 5" />
      {ticks.map((t) => {
        const [x1, y1] = toXY(t, r)
        const [x2, y2] = toXY(t, r - (t % 90 === 0 ? 9 : 5))
        const [tx, ty] = toXY(t, r - 20)
        return (
          <g key={t}>
            <line x1={x1} y1={y1} x2={x2} y2={y2} stroke="rgba(148,163,184,0.5)" strokeWidth={t % 90 === 0 ? 1.6 : 1} />
            {t % 90 === 0 && (
              <text x={tx} y={ty + 3} textAnchor="middle" fontSize="9" fill="rgba(148,163,184,0.75)">
                {t === 0 ? 'N' : t}
              </text>
            )}
          </g>
        )
      })}
      {/* 机身参考（横滚） */}
      <g transform={`rotate(${roll} ${cx} ${cy})`}>
        <line x1={cx - r * 0.42} y1={cy} x2={cx + r * 0.42} y2={cy} stroke="#a78bfa" strokeWidth="2" opacity="0.8" />
        <circle cx={cx} cy={cy} r="2.5" fill="#a78bfa" />
      </g>
      {/* 航向指针 */}
      <line x1={cx} y1={cy} x2={nx} y2={ny} stroke="#22d3ee" strokeWidth="2.4" strokeLinecap="round" />
      <circle cx={cx} cy={cy} r="3.5" fill="#22d3ee" />
      {/* 俯仰条 */}
      <g transform={`translate(${cx - r * 0.4} ${cy + r + 2})`}>
        <rect x="0" y="0" width={r * 0.8} height="4" rx="2" fill="rgba(148,163,184,0.2)" />
        <rect
          x={r * 0.4 - 2}
          y="-2"
          width="4"
          height="8"
          rx="2"
          fill="#34d399"
          transform={`translate(${clamp(pitch, -30, 30) * (r * 0.8 / 60)} 0)`}
        />
      </g>
      <text x={cx} y={cy - 16} textAnchor="middle" fontSize="15" fill="#e2e8f0" className="num">
        {yaw.toFixed(1)}°
      </text>
    </svg>
  )
}
