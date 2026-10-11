import React from 'react'
import { Badge, Card, GrayArray, Stat, Toggle } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { store, useStore } from '../lib/store'
import { fmt, fmtInt, grayToInk } from '../lib/format'
import type { CalChannel } from '../lib/types'

/** 一套标定画一组"黑→白"量程条（每路一条，带阈值刻度）。
 *
 *  ★2026-10-11 加（用户："在上位机传感器里加上两种标定的值用图显示"）。
 *  为什么非要有图：表里的数字能看出"标了多少"，但**看不出两套是不是真的分开了** ——
 *  而"切到亚克力之后循迹失效"最常见的原因就是**第二套其实是在同一种材料上标的**
 *  （两套数值几乎一样，一切过去控制环立刻失去参考）。画成量程条，一眼就能看出来。
 */
function CalSetBars({
  ch,
  label,
  color,
  active,
  max,
}: {
  ch: CalChannel[]
  label: string
  color: string
  active: boolean
  max: number
}) {
  const W = 400
  const padL = 18
  const padR = 104
  const rowH = 13
  const gap = 3
  const span = W - padL - padR
  const H = ch.length * (rowH + gap)

  return (
    <div className="mt-3">
      <div className="mb-1 flex items-center gap-2 text-[12px]">
        <span className="inline-block h-2.5 w-2.5 rounded-sm" style={{ background: color }} />
        <span className={active ? 'font-medium text-slate-100' : 'text-slate-400'}>{label}</span>
        {active ? <Badge tone="mint">当前在用</Badge> : <Badge tone="slate">另一套</Badge>}
      </div>
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ height: H }} role="img" aria-label={label}>
        {ch.map((c, i) => {
          const y = i * (rowH + gap) + 2
          const lo = Math.min(c.blk, c.wht)
          const hi = Math.max(c.blk, c.wht)
          const x0 = padL + (lo / max) * span
          const x1 = padL + (hi / max) * span
          const xt = padL + (c.thr / max) * span
          return (
            <g key={c.i}>
              {/* 量程底槽 */}
              <rect x={padL} y={y} width={span} height={rowH - 5} rx={1.5} fill="#ffffff12" />
              {/* 黑→白 的那一段（这段越宽 = 对比度越好） */}
              <rect
                x={x0}
                y={y}
                width={Math.max(2, x1 - x0)}
                height={rowH - 5}
                rx={1.5}
                fill={color}
                opacity={active ? 0.9 : 0.42}
              />
              {/* 阈值刻度 */}
              <line x1={xt} x2={xt} y1={y - 1.5} y2={y + rowH - 3.5} stroke="#0b1220" strokeWidth={2} />
              <text x={padL - 4} y={y + rowH - 6} fontSize={9} fill="#94a3b8" textAnchor="end">
                {c.i}
              </text>
              <text x={W - padR + 6} y={y + rowH - 6} fontSize={9} fill={c.ok ? '#94a3b8' : '#fb7185'}>
                {c.blk}→{c.wht} 阈{c.thr}
                {c.ok ? '' : ` ✕${c.reason}`}
              </text>
            </g>
          )
        })}
      </svg>
    </div>
  )
}

/** 两套之间差多少 —— 判断"亚克力到底有没有在亚克力上标"的关键数字。 */
function CalCompare({ a, b }: { a: CalChannel[]; b: CalChannel[] }) {
  const d = a.map((c, i) => ({
    i,
    blk: Math.abs(c.blk - (b[i]?.blk ?? 0)),
    wht: Math.abs(c.wht - (b[i]?.wht ?? 0)),
    thr: Math.abs(c.thr - (b[i]?.thr ?? 0)),
  }))
  const maxBlk = Math.max(...d.map((x) => x.blk))
  const maxWht = Math.max(...d.map((x) => x.wht))
  const maxThr = Math.max(...d.map((x) => x.thr))
  // 每路自己的黑白跨度（对比度）—— 跨度的差比绝对值的差更能说明"是不是两种材料"
  const spanA = a.map((c) => Math.abs(c.wht - c.blk))
  const spanB = b.map((c) => Math.abs(c.wht - c.blk))
  const nearlySame = maxBlk <= 3 && maxWht <= 5
  return (
    <div className="mt-3 rounded-md border border-white/10 bg-white/[0.03] p-2 text-[12px]">
      <div className="flex flex-wrap items-center gap-2">
        <span className="text-slate-400">两套最大差异</span>
        <span className="num text-slate-200">黑 {maxBlk}</span>
        <span className="num text-slate-200">白 {maxWht}</span>
        <span className="num text-slate-200">阈 {maxThr}</span>
        {nearlySame && <Badge tone="rose">两套几乎一样</Badge>}
      </div>
      {nearlySame && (
        <p className="mt-1.5 leading-relaxed text-rose-300/90">
          两套数值几乎相同 ⇒ <b>第二套很可能是在同一种材料上标的</b>，切过去等于没换。
          要在亚克力上用的话，把车放到<b>亚克力（和反光条）</b>上重新走一遍黑白标定。
        </p>
      )}
      <div className="mt-1.5 text-slate-500">
        对比度（白−黑）：
        {a.map((c, i) => (
          <span key={i} className="num ml-1.5">
            <span className="text-slate-400">{i}</span>
            <span className="text-[#22d3ee]">{spanA[i]}</span>
            <span className="text-slate-600">/</span>
            <span className="text-[#fbbf24]">{spanB[i]}</span>
          </span>
        ))}
      </div>
    </div>
  )
}

export function SensorsPage() {
  const { telem, cal, settings } = useStore()
  const t = telem
  const line = t?.line ?? Array(8).fill(0)
  const gray = t?.gray ?? Array(8).fill(0)
  const lit = line.reduce((a, b) => a + (b ? 1 : 0), 0)

  // ★把桥给的两套摆正：ch 是"当前在用那套"，other 是另一套；active_set 说明哪套是当前。
  const sets = React.useMemo(() => {
    const ch = cal?.ch ?? []
    if (!ch.length) return null
    const other = cal?.other?.length ? cal.other : null
    const a = cal?.active_set
    const s0 = a === 1 ? other : ch
    const s1 = a === 1 ? ch : other
    const all = [...ch, ...(other ?? [])].flatMap((c) => [c.blk, c.wht, c.thr])
    return {
      active: a,
      max: Math.max(1, ...all) * 1.1,
      s0,
      s1,
      both: s0 && s1 ? { a: s0, b: s1 } : null,
    }
  }, [cal])

  return (
    <div className="grid gap-3 xl:grid-cols-[minmax(0,1fr)_420px]">
      <div className="grid gap-3">
        <Card
          title="灰度阵列"
          right={
            <div className="flex items-center gap-2">
              <Badge tone={t?.flags.lost ? 'rose' : 'mint'}>{t?.flags.lost ? '丢线' : `压线 ${lit} 路`}</Badge>
              <Badge tone="slate">原始值 0..255</Badge>
            </div>
          }
        >
          <GrayArray line={line} gray={gray} height={140} hint />
          <div className="mt-3 grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="重心偏差" value={fmt(t?.trace.pos, 3)} hint="相对 4.5（车正中）" />
            <Stat label="循迹 PID 输出" value={fmtInt(t?.trace.pid)} tone="amber" title="PID = 循迹控制器的修正量输出，越大说明它判断车越偏" />
            <Stat label="黑度高" value={fmt(Math.max(...gray.map(grayToInk)) * 255, 0)} />
            <Stat label="路口标志" value={t?.flags.slope ? '有' : '无'} />
          </div>
        </Card>

        <Card title="趋势">
          <div className="grid gap-3 md:grid-cols-2">
            <div>
              <div className="mb-1 text-[12px] text-slate-400">重心偏差</div>
              <MiniTrend channel="trace_pos" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400" title="PID = 循迹控制器的修正量输出，越大说明它判断车越偏">循迹 PID 输出</div>
              <MiniTrend channel="trace_pid" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">灰度峰值</div>
              <MiniTrend channel="gray_max" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">主循环耗时</div>
              <MiniTrend channel="loop_us" height={44} />
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card
          title="两套标定对比"
          right={
            <Badge tone={cal?.quality === 0 ? 'mint' : 'amber'}>
              {cal?.active_set == null ? '当前套 ?' : `第 ${cal.active_set} 套`} · 质量 {cal?.quality ?? '--'}
            </Badge>
          }
        >
          {!sets ? (
            <div className="text-[13px] text-slate-500">暂无标定数据（后端没返回 /api/cal）。</div>
          ) : (
            <>
              <p className="text-[12px] leading-relaxed text-slate-500">
                每条是那一路的<b>黑→白量程</b>，竖线是阈值。两套分开画，方便看它们到底差多少。
              </p>
              {sets.s0 && (
                <CalSetBars
                  ch={sets.s0}
                  label="第 0 套 · 普通（宝丽布）"
                  color="#22d3ee"
                  active={sets.active === 0}
                  max={sets.max}
                />
              )}
              {sets.s1 ? (
                <CalSetBars
                  ch={sets.s1}
                  label="第 1 套 · 亚克力"
                  color="#fbbf24"
                  active={sets.active === 1}
                  max={sets.max}
                />
              ) : (
                <div className="mt-3 rounded-md border border-white/10 bg-white/[0.03] p-2 text-[12px] leading-relaxed text-slate-400">
                  只收到<b>一套</b>标定 —— 车上的固件还没更新到会发第二套的版本
                  （新固件的 <span className="num">T_CAL_REPORT_B</span> 帧才带另一套 + 真实套号）。
                  固件已在仓库里改好，<b>烧上去就能同时看到两套</b>。
                </div>
              )}
              {sets.both && <CalCompare a={sets.both.a} b={sets.both.b} />}
            </>
          )}
        </Card>
        <Card title="显示设置">
          <Toggle
            on={settings.scopeChannels.includes('gray_max')}
            onChange={(v) =>
              store.patchSettings({
                scopeChannels: v
                  ? [...settings.scopeChannels, 'gray_max']
                  : settings.scopeChannels.filter((k) => k !== 'gray_max'),
              })
            }
            label="把「灰度峰值」加进波形页"
          />
          <p className="mt-2 text-[12px] leading-relaxed text-slate-500">
            屏幕上的灰度是 8 路（车头朝前，左边是 0 号）。压线越多、数组越长，说明线越宽或者车斜着压线。
          </p>
        </Card>
      </div>
    </div>
  )
}
