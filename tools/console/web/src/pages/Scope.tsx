import React from 'react'
import { Download, Pause, Play, Trash2 } from 'lucide-react'
import { Badge, Card, Toggle } from '../components/ui'
import { ScopeBoard, type CursorReadout } from '../components/ScopeChart'
import { CHANNELS, CHANNEL_BY_KEY } from '../lib/channels'
import { csvFileName, downloadText, snapshotToCsv, type CsvColumn } from '../lib/csv'
import { store, useStore } from '../lib/store'
import { fmt } from '../lib/format'

const WINDOWS: Array<5 | 10 | 20 | 30> = [5, 10, 20, 30]

export function ScopePage() {
  const { settings, ringFill } = useStore()
  const [readout, setReadout] = React.useState<CursorReadout | null>(null)
  const [full, setFull] = React.useState(false)

  const exportCsv = () => {
    const snap = full ? store.ring.all() : store.ring.snapshot(settings.windowSec * 1000)
    const cols: CsvColumn[] = []
    for (const k of settings.scopeChannels) {
      const def = CHANNEL_BY_KEY[k]
      const field = CHANNELS.findIndex((c) => c.key === k)
      if (!def || field < 0) continue
      cols.push({ header: `${def.label}${def.unit ? `(${def.unit})` : ''}`, field, decimals: def.decimals })
    }
    const text = snapshotToCsv(snap, cols)
    downloadText(csvFileName('console', Date.now()), text)
    store.toast('info', `已导出 ${snap.count} 行（${full ? '全缓冲' : `${settings.windowSec}s 窗口`}）`)
  }

  const rd = readout ? CHANNEL_BY_KEY[readout.key] : undefined

  return (
    <div className="grid gap-3">
      <Card
        title="波形"
        right={
          <div className="flex flex-wrap items-center gap-2">
            <div className="seg">
              {WINDOWS.map((w) => (
                <button
                  key={w}
                  className={`seg-item ${settings.windowSec === w ? 'seg-on' : ''}`}
                  onClick={() => store.patchSettings({ windowSec: w })}
                >
                  {w}s
                </button>
              ))}
            </div>
            <button className={`btn ${settings.paused ? 'btn-warn' : ''}`} onClick={() => store.patchSettings({ paused: !settings.paused })}>
              {settings.paused ? <Play size={13} /> : <Pause size={13} />}
              {settings.paused ? '继续' : '暂停'}
            </button>
            <button className="btn-ghost" onClick={exportCsv}>
              <Download size={13} /> 导出 CSV
            </button>
            <label className="flex items-center gap-1 text-[12px] text-slate-400">
              <input type="checkbox" checked={full} onChange={(e) => setFull(e.target.checked)} />
              全缓冲
            </label>
            <button className="btn-ghost" onClick={() => store.clearBuffers()}>
              <Trash2 size={13} /> 清空
            </button>
          </div>
        }
      >
        <div className="mb-3 flex flex-wrap items-center gap-3 text-[12px] text-slate-400">
          <span>
            缓冲 <span className="num text-slate-200">{ringFill.toLocaleString()}</span> / {settings.ringPoints.toLocaleString()} 点
          </span>
          <span>
            窗口 <span className="num text-slate-200">{settings.windowSec}s</span>
          </span>
          {settings.paused && <Badge tone="amber">显示已冻结（数据照收）</Badge>}
          {rd && readout && (
            <span className="num text-slate-200">
              游标：t {fmt(readout.tSec, 2)}s · {rd.label} {fmt(readout.value, rd.decimals)} {rd.unit}
            </span>
          )}
        </div>
        <ScopeBoard windowMs={settings.windowSec * 1000} paused={settings.paused} onHover={setReadout} />
      </Card>

      <Card title="显示哪些通道">
        <div className="grid grid-cols-2 gap-1.5 md:grid-cols-4">
          {CHANNELS.map((c) => {
            const on = settings.scopeChannels.includes(c.key)
            return (
              <Toggle
                key={c.key}
                on={on}
                label={
                  <span className="flex items-center gap-1.5">
                    <span className="h-2 w-2 rounded-sm" style={{ background: c.color }} />
                    {c.label}
                    <span className="text-[10px] text-slate-500">{c.group}</span>
                  </span>
                }
                onChange={(v) =>
                  store.patchSettings({
                    scopeChannels: v
                      ? [...settings.scopeChannels, c.key]
                      : settings.scopeChannels.filter((k) => k !== c.key),
                  })
                }
              />
            )
          })}
        </div>
        <p className="mt-3 text-[12px] text-slate-500">
          每条通道一根**独立量程**的泳道 —— 共用一根 Y 轴的话 PWM(±7200) 会把角度和电流压成直线。
        </p>
      </Card>
    </div>
  )
}
