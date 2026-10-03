import React from 'react'
import { Download, Play, RotateCcw, Save, Upload } from 'lucide-react'
import { Badge, Card, NumberField, Slider, Stat, StatusDot } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { store, useStore } from '../lib/store'
import { fmt, fmtInt } from '../lib/format'
import type { ActionItem } from '../lib/types'

function ServoCard({ ch }: { ch: 1 | 2 }) {
  const { telem } = useStore()
  const [angle, setAngle] = React.useState(0)
  const [dev, setDev] = React.useState(0)
  const us = ch === 1 ? telem?.servo1_us : telem?.servo2_us
  const deg = ch === 1 ? telem?.servo1_deg : telem?.servo2_deg

  return (
    <Card
      title={`舵机 ${ch}`}
      right={
        <Badge tone="slate">
          <StatusDot on={!!telem} tone="mint" />
          {fmt(us, 0)} µs · {fmt(deg, 1)}°
        </Badge>
      }
    >
      <div className="mb-2 flex items-end justify-between">
        <Stat label="目标角度" value={fmt(angle, 0)} unit="°" size="lg" tone="cyan" />
        <div className="flex items-center gap-2">
          <NumberField value={angle} onChange={setAngle} min={0} max={180} step={1} decimals={0} unit="°" />
          <button className="btn" onClick={() => void store.setServo(ch, angle)}>
            下发
          </button>
        </div>
      </div>
      <Slider value={angle} min={0} max={180} step={1} onChange={(v) => { setAngle(v); void store.setServo(ch, v) }} />
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <button className="btn-ghost" onClick={() => { setAngle(90); void store.setServo(ch, 90) }}>90°</button>
        <button className="btn-ghost" onClick={() => { setAngle(0); void store.setServo(ch, 0) }}>0°（收回）</button>
        <button className="btn-ghost" onClick={() => void store.centerServo()}>两路回中</button>
        <div className="ml-auto flex items-center gap-1">
          <span className="text-[11px] text-slate-500">中位偏差</span>
          <NumberField value={dev} onChange={setDev} min={-100} max={100} step={1} unit="µs" />
          <button className="btn-ghost" onClick={() => void store.setDeviation(ch, dev)}>应用</button>
        </div>
      </div>
      <div className="mt-3">
        <div className="mb-1 text-[12px] text-slate-400">角度趋势</div>
        <MiniTrend channel={ch === 1 ? 'servo1' : 'servo2'} height={36} />
      </div>
    </Card>
  )
}

function ActionRow({ a }: { a: ActionItem }) {
  const [a1, setA1] = React.useState(a.a1)
  const [a2, setA2] = React.useState(a.a2)
  const { telem } = useStore()
  const active = telem?.action === a.n

  React.useEffect(() => {
    setA1(a.a1)
    setA2(a.a2)
  }, [a.a1, a.a2])

  return (
    <tr className={`border-t border-white/5 ${active ? 'bg-neon-cyan/5' : ''}`}>
      <td className="py-1 pr-2">
        <span className={`num inline-block w-6 text-right ${active ? 'text-neon-cyan' : 'text-slate-400'}`}>{a.n}</span>
      </td>
      <td className="py-1 pr-2">
        <span className="text-slate-200">{a.name}</span>
        {a.compound && <Badge tone="amber">复合</Badge>}
        {a.dirty && <Badge tone="rose">未保存</Badge>}
      </td>
      <td className="py-1 pr-2">
        <NumberField value={a1} onChange={setA1} min={0} max={180} step={5} decimals={0} />
      </td>
      <td className="py-1 pr-2">
        <NumberField value={a2} onChange={setA2} min={0} max={180} step={5} decimals={0} />
      </td>
      <td className="py-1 pr-2 text-right text-[11px] text-slate-500 num">
        {a.preset_a1}/{a.preset_a2}
      </td>
      <td className="py-1">
        <div className="flex flex-wrap justify-end gap-1">
          <button
            className="btn-ghost !px-2 !py-0.5 text-[11px]"
            onClick={() => void store.setAction(a.n, a1, a2)}
            title="把这一行的角度下发到车的 RAM（立刻生效）"
          >
            下发
          </button>
          <button
            className="btn-ghost !px-2 !py-0.5 text-[11px]"
            onClick={async () => {
              await store.setAction(a.n, a1, a2)
              await store.playAction(a.n)
            }}
            title="下发后完整播放一次（复合动作会走中间步骤）"
          >
            <Play size={11} /> 播放
          </button>
          {a.compound && (
            <span className="self-center text-[10px] text-slate-500" title="复合动作的中间步骤由固件写死，这里只改起止角">
              中间步骤固定
            </span>
          )}
        </div>
      </td>
    </tr>
  )
}

export function ServoPage() {
  const { actions, presets, telem } = useStore()
  const [presetName, setPresetName] = React.useState('')
  const items = actions?.items ?? []
  const dirty = items.filter((i) => i.dirty).length

  return (
    <div className="grid gap-3 xl:grid-cols-2">
      <div className="grid gap-3">
        <ServoCard ch={1} />
        <ServoCard ch={2} />
        <Card title="当前状态">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="正在执行动作" value={`${telem?.action ?? '--'}`} tone="cyan" />
            <Stat label="1 号脉宽" value={fmtInt(telem?.servo1_us)} unit="µs" />
            <Stat label="2 号脉宽" value={fmtInt(telem?.servo2_us)} unit="µs" />
            <Stat label="动作表来源" value={actions?.ok ? '固件' : '本地'} tone={actions?.ok ? 'mint' : 'amber'} />
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card
          title="动作表（14 个动作）"
          right={
            <div className="flex items-center gap-1">
              {dirty > 0 && <Badge tone="rose">{dirty} 项改了</Badge>}
              <button
                className="btn-ghost"
                onClick={() => void store.saveActions()}
                title="让车把整张动作表写进自己的 Flash（掉电不丢）"
              >
                <Save size={13} /> 固化到车
              </button>
            </div>
          }
        >
          <div className="overflow-x-auto">
            <table className="w-full text-left text-[12px]">
              <thead className="text-slate-500">
                <tr>
                  <th className="py-1 pr-2">号</th>
                  <th className="py-1 pr-2">动作</th>
                  <th className="py-1 pr-2">舵机1</th>
                  <th className="py-1 pr-2">舵机2</th>
                  <th className="py-1 pr-2 text-right">出厂</th>
                  <th className="py-1 text-right">操作</th>
                </tr>
              </thead>
              <tbody>
                {items.map((a) => (
                  <ActionRow key={a.n} a={a} />
                ))}
              </tbody>
            </table>
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            <button className="btn" onClick={() => void store.refreshActions()}>
              <Upload size={13} /> 从车重读动作表
            </button>
            <button className="btn-ghost" onClick={() => void store.refreshParams()}>
              从车重读参数
            </button>
            <button
              className="btn-ghost"
              onClick={() => void store.paramsDefaults()}
              title="把参数表的默认值拉回来（不含动作表）"
            >
              <RotateCcw size={13} /> 参数回默认
            </button>
          </div>
        </Card>

        <Card title="预设（存在这台电脑上）">
          <div className="flex flex-wrap items-center gap-2">
            <input
              className="field w-40"
              placeholder="预设名，如 高台-能过"
              value={presetName}
              onChange={(e) => setPresetName(e.target.value)}
            />
            <button
              className="btn-primary"
              disabled={!presetName.trim()}
              onClick={() => void store.savePreset(presetName.trim(), items.map((i) => `action.${i.n}`))}
            >
              <Download size={13} /> 存当前动作表
            </button>
          </div>
          <div className="mt-3 grid gap-2">
            {presets.length === 0 && <div className="text-[12px] text-slate-500">还没有预设。</div>}
            {presets.map((p) => (
              <div key={p.name} className="flex items-center gap-2 rounded-lg bg-white/5 px-2 py-1.5">
                <span className="text-[13px] text-slate-200">{p.name}</span>
                <Badge tone="slate">{p.kind}</Badge>
                <span className="truncate text-[11px] text-slate-500">{p.keys.join(', ')}</span>
                <div className="ml-auto flex gap-1">
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.applyPreset(p.name, ['actions', 'params', 'servo'])}>
                    应用到车
                  </button>
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.deletePreset(p.name)}>
                    删
                  </button>
                </div>
              </div>
            ))}
          </div>
        </Card>
      </div>
    </div>
  )
}
