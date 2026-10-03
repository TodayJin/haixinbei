import React from 'react'
import { ArrowDown, ArrowLeft, ArrowRight, ArrowUp, OctagonX, SlidersHorizontal } from 'lucide-react'
import { store, useStore } from '../lib/store'
import { Badge, Card, NumberField, Slider, Stat, Toggle } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { fmt, fmtInt } from '../lib/format'
import type { DriveMode } from '../lib/types'

const MODE_LABEL: Record<DriveMode, string> = {
  off: '停（交回车自己）',
  wheels: '左右轮速直给',
  base_turn: '基准速度 + 转向',
}

export function DrivePage() {
  const { telem, settings } = useStore()
  const [mode, setMode] = React.useState<DriveMode>('wheels')
  const [vl, setVl] = React.useState(0)
  const [vr, setVr] = React.useState(0)
  const [base, setBase] = React.useState(0)
  const [turn, setTurn] = React.useState(0)
  const step = settings.driveStep

  const t = telem
  const driving = !!t?.flags.host_drive

  const apply = (next?: Partial<{ vl: number; vr: number; base: number; turn: number; mode: DriveMode }>) => {
    const m = next?.mode ?? mode
    const body =
      m === 'wheels'
        ? { mode: m, vl: next?.vl ?? vl, vr: next?.vr ?? vr }
        : m === 'base_turn'
          ? { mode: m, base: next?.base ?? base, turn: next?.turn ?? turn }
          : { mode: 'off' as DriveMode }
    void store.drive(body)
  }

  const quick = (kind: 'fwd' | 'back' | 'left' | 'right' | 'stop') => {
    if (kind === 'stop') {
      void store.drive({ mode: 'off' })
      return
    }
    if (mode === 'wheels') {
      const s = step
      if (kind === 'fwd') apply({ vl: s, vr: s })
      if (kind === 'back') apply({ vl: -s, vr: -s })
      if (kind === 'left') apply({ vl: 0, vr: s })
      if (kind === 'right') apply({ vl: s, vr: 0 })
    } else {
      const s = step
      if (kind === 'fwd') apply({ base: s, turn: 0 })
      if (kind === 'back') apply({ base: -s, turn: 0 })
      if (kind === 'left') apply({ base: 0, turn: -s })
      if (kind === 'right') apply({ base: 0, turn: s })
    }
  }

  return (
    <div className="grid gap-3 xl:grid-cols-[minmax(0,1fr)_380px]">
      <div className="grid gap-3">
        <Card
          title="驾驶指令"
          right={
            <Badge tone={driving ? 'mint' : 'slate'}>
              上位机驾驶 {driving ? '开' : '关'}
            </Badge>
          }
        >
          <div className="mb-3 flex flex-wrap items-center gap-3">
            <div className="seg">
              {(['wheels', 'base_turn', 'off'] as DriveMode[]).map((m) => (
                <button key={m} className={`seg-item ${mode === m ? 'seg-on' : ''}`} onClick={() => setMode(m)}>
                  {MODE_LABEL[m]}
                </button>
              ))}
            </div>
            <span className="text-[12px] text-slate-500">
              都走车上正常的三环串级 PID，这里只改目标速度
            </span>
          </div>

          {mode === 'wheels' ? (
            <div className="grid gap-4 md:grid-cols-2">
              <div>
                <div className="mb-1 flex items-center justify-between text-[13px]">
                  <span className="text-slate-300">左轮目标</span>
                  <NumberField value={vl} onChange={setVl} min={-3000} max={3000} step={step} unit="mm/s" />
                </div>
                <Slider value={vl} min={-3000} max={3000} step={step} onChange={setVl} />
              </div>
              <div>
                <div className="mb-1 flex items-center justify-between text-[13px]">
                  <span className="text-slate-300">右轮目标</span>
                  <NumberField value={vr} onChange={setVr} min={-3000} max={3000} step={step} unit="mm/s" />
                </div>
                <Slider value={vr} min={-3000} max={3000} step={step} onChange={setVr} />
              </div>
            </div>
          ) : mode === 'base_turn' ? (
            <div className="grid gap-4 md:grid-cols-2">
              <div>
                <div className="mb-1 flex items-center justify-between text-[13px]">
                  <span className="text-slate-300">基准速度（前进量）</span>
                  <NumberField value={base} onChange={setBase} min={-3000} max={3000} step={step} unit="mm/s" />
                </div>
                <Slider value={base} min={-3000} max={3000} step={step} onChange={setBase} />
              </div>
              <div>
                <div className="mb-1 flex items-center justify-between text-[13px]">
                  <span className="text-slate-300">转向（左右差）</span>
                  <NumberField value={turn} onChange={setTurn} min={-3000} max={3000} step={step} unit="mm/s" />
                </div>
                <Slider value={turn} min={-3000} max={3000} step={step} onChange={setTurn} />
              </div>
            </div>
          ) : (
            <div className="text-[13px] text-slate-400">已选「停」：点下面的下发把上位机驾驶交还车上的任务流程。</div>
          )}

          <div className="mt-4 flex flex-wrap gap-2">
            <button className="btn-primary" onClick={() => apply()}>
              下发
            </button>
            <button className="btn-danger" onClick={() => quick('stop')}>
              <OctagonX size={14} /> 立刻停
            </button>
            <div className="ml-auto flex items-center gap-2">
              <span className="text-[12px] text-slate-500">步长</span>
              <NumberField
                value={step}
                onChange={(v) => store.patchSettings({ driveStep: Math.max(10, Math.round(v)) })}
                min={10}
                max={1000}
                step={10}
                unit="mm/s"
              />
            </div>
          </div>
        </Card>

        <Card title="快捷动作">
          <div className="grid w-full max-w-[420px] grid-cols-3 gap-2">
            <span />
            <button className="btn h-14 flex-col" onClick={() => quick('fwd')}>
              <ArrowUp size={18} /> 前进
            </button>
            <span />
            <button className="btn h-14 flex-col" onClick={() => quick('left')}>
              <ArrowLeft size={18} /> 左转
            </button>
            <button className="btn-danger h-14 flex-col" onClick={() => quick('stop')}>
              <OctagonX size={18} /> 停
            </button>
            <button className="btn h-14 flex-col" onClick={() => quick('right')}>
              <ArrowRight size={18} /> 右转
            </button>
            <span />
            <button className="btn h-14 flex-col" onClick={() => quick('back')}>
              <ArrowDown size={18} /> 后退
            </button>
            <span />
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            {settings.drivePresets.map((s) => (
              <button key={s} className="btn-ghost" onClick={() => apply({ vl: s, vr: s, base: s, turn: 0 })}>
                {s} mm/s
              </button>
            ))}
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card title="实时反馈">
          <div className="grid grid-cols-2 gap-3">
            <Stat label="实测左 / 右" value={`${fmt(t?.vl, 0)} / ${fmt(t?.vr, 0)}`} unit="mm/s" />
            <Stat label="PWM 左 / 右" value={`${fmtInt(t?.pwm_l)} / ${fmtInt(t?.pwm_r)}`} tone="amber" />
            <Stat label="偏航" value={fmt(t?.yaw, 1)} unit="°" tone="cyan" />
            <Stat label="角度环修正" value={fmtInt(t?.angle.corr)} tone="violet" />
          </div>
          <div className="mt-3 grid gap-3">
            <div>
              <div className="mb-1 text-[12px] text-slate-400">左轮速</div>
              <MiniTrend channel="vl" height={36} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">右轮速</div>
              <MiniTrend channel="vr" height={36} />
            </div>
          </div>
        </Card>
        <Card title="提示">
          <ul className="list-disc space-y-1 pl-4 text-[12px] leading-relaxed text-slate-400">
            <li>上位机驾驶只在「遥测里 host_drive 为开」时有效；车上的任务流程一启动就会把它顶掉。</li>
            <li>桥有链路看门狗：真车模式下 0.5s 收不到上位机心跳会自动停车（防失联跑飞）。</li>
            <li>比赛规则不允许遥控干预，这套只用于调试与标定。</li>
          </ul>
          <div className="mt-3">
            <Toggle
              on={!settings.safetyBanner}
              onChange={(v) => store.patchSettings({ safetyBanner: !v })}
              label="不再显示驾驶页安全提示"
            />
          </div>
        </Card>
      </div>
    </div>
  )
}
