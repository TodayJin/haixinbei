import React from 'react'
import { RotateCcw, Save, Search, Upload } from 'lucide-react'
import { Badge, Card, NumberField } from '../components/ui'
import { store, useMeta } from '../lib/store'
import { fmt } from '../lib/format'
import type { ParamItem } from '../lib/types'

const GROUP_ORDER = ['速度环', '角度环', '循迹环', '串级', '任务', '车辆', '电源', '机械']

function ParamRow({ p }: { p: ParamItem }) {
  const dirty = p.dirty
  const atEdge =
    p.value <= p.min + (p.max - p.min) * 0.02 || p.value >= p.max - (p.max - p.min) * 0.02
  const span = p.max - p.min || 1
  const pos = Math.max(0, Math.min(1, (p.value - p.min) / span))
  const dpos = Math.max(0, Math.min(1, (p.default - p.min) / span))

  return (
    <div className="grid grid-cols-[minmax(120px,1.4fr)_minmax(150px,1fr)_minmax(140px,1.2fr)] items-center gap-3 border-t border-white/5 py-1.5">
      <div className="min-w-0">
        <div className="flex items-center gap-2">
          <span className="truncate text-[13px] text-slate-200">{p.label}</span>
          {p.unit && <span className="text-[11px] text-slate-500">{p.unit}</span>}
          {dirty && <Badge tone="rose">改了</Badge>}
          {atEdge && <Badge tone="amber">贴边界</Badge>}
        </div>
        <div className="truncate text-[10px] text-slate-600">
          {p.key} · id 0x{p.id.toString(16).toUpperCase()} {p.note ? `· ${p.note}` : ''}
        </div>
      </div>

      <div className="flex items-center gap-2">
        <NumberField
          value={p.value}
          decimals={p.decimals}
          step={p.step}
          min={p.min}
          max={p.max}
          onChange={(v) => store.setParam(p.id, v)}
        />
        <button className="btn-ghost !px-2 !py-0.5 text-[11px]" title="改回固件默认值（只是本地改，点下发才生效）" onClick={() => store.setParam(p.id, p.default)}>
          默认 {fmt(p.default, p.decimals)}
        </button>
      </div>

      <div className="min-w-0">
        <div className="relative h-2 w-full rounded-full bg-white/5">
          <div className="absolute top-0 h-2 w-0.5 bg-slate-500" style={{ left: `${dpos * 100}%` }} title={`默认 ${fmt(p.default, p.decimals)}`} />
          <div
            className={`absolute -top-0.5 h-3 w-1.5 rounded-full ${atEdge ? 'bg-neon-amber' : 'bg-neon-cyan'}`}
            style={{ left: `calc(${pos * 100}% - 3px)` }}
          />
        </div>
        <div className="mt-0.5 flex justify-between text-[10px] text-slate-600 num">
          <span>{fmt(p.min, p.decimals)}</span>
          <span>{fmt(p.max, p.decimals)}</span>
        </div>
      </div>
    </div>
  )
}

export function ParamsPage() {
  const { params } = useMeta()
  const [q, setQ] = React.useState('')
  const [onlyDirty, setOnlyDirty] = React.useState(false)
  const items = React.useMemo(() => {
    const all = params?.items ?? []
    const needle = q.trim().toLowerCase()
    return all.filter(
      (p) =>
        (!onlyDirty || p.dirty) &&
        (!needle ||
          p.label.toLowerCase().includes(needle) ||
          p.key.toLowerCase().includes(needle) ||
          p.group.toLowerCase().includes(needle)),
    )
  }, [params, q, onlyDirty])

  const groups = React.useMemo(() => {
    const map = new Map<string, ParamItem[]>()
    for (const p of items) {
      const list = map.get(p.group) ?? []
      list.push(p)
      map.set(p.group, list)
    }
    const order = [...GROUP_ORDER.filter((g) => map.has(g)), ...[...map.keys()].filter((g) => !GROUP_ORDER.includes(g))]
    return order.map((g) => [g, map.get(g)!] as const)
  }, [items])

  const dirtyCount = (params?.items ?? []).filter((p) => p.dirty).length

  return (
    <div className="grid gap-3">
      <Card
        title="参数表"
        right={
          <div className="flex flex-wrap items-center gap-2">
            <Badge tone={params?.source === 'fw' ? 'mint' : 'amber'}>来源 {params?.source ?? '--'}</Badge>
            <Badge tone="slate" title="车上参数表的版本号；网页算出来的和它不一致说明两边参数没同步">指纹 0x{(params?.param_crc ?? 0).toString(16).toUpperCase()}</Badge>
            {dirtyCount > 0 && <Badge tone="rose">{dirtyCount} 项改了没下发</Badge>}
          </div>
        }
      >
        <div className="mb-3 flex flex-wrap items-center gap-2">
          <div className="relative">
            <Search size={14} className="pointer-events-none absolute left-2 top-2 text-slate-500" />
            <input className="field w-56 pl-7" placeholder="搜名字 / key / 分组" value={q} onChange={(e) => setQ(e.target.value)} />
          </div>
          <button className={`btn-ghost ${onlyDirty ? 'ring-1 ring-neon-rose/40' : ''}`} onClick={() => setOnlyDirty((v) => !v)}>
            只看改动
          </button>
          <div className="ml-auto flex flex-wrap gap-2">
            <button className="btn-primary" onClick={() => void store.pushParams()} disabled={dirtyCount === 0}>
              <Upload size={13} /> 全部下发（{dirtyCount}）
            </button>
            <button className="btn" onClick={() => void store.saveParams()} title="让车把这批参数写进自己的 Flash">
              <Save size={13} /> 固化到车
            </button>
            <button className="btn-ghost" onClick={() => void store.reloadParams()}>
              从车重读
            </button>
            <button className="btn-ghost" onClick={() => void store.paramsDefaults()}>
              <RotateCcw size={13} /> 回默认
            </button>
          </div>
        </div>

        {groups.length === 0 && <div className="text-[13px] text-slate-500">没有匹配的参数。</div>}
        {groups.map(([g, list]) => (
          <div key={g} className="mb-4">
            <div className="mb-1 flex items-center gap-2">
              <h3 className="text-[12px] font-medium uppercase tracking-wider text-slate-400">{g}</h3>
              <span className="text-[11px] text-slate-600">{list.length} 项</span>
            </div>
            {list.map((p) => (
              <ParamRow key={p.id} p={p} />
            ))}
          </div>
        ))}
      </Card>
    </div>
  )
}
