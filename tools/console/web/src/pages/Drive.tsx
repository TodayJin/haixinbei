import React from 'react'
import { OctagonX } from 'lucide-react'
import { store, useStore } from '../lib/store'
import { Badge, Card, Slider, Stat, Toggle } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { fmt, fmtInt } from '../lib/format'
import { isTypingTarget, keyId, keyRoleById, rolesToDrive, type KeyRole } from '../lib/keys'

/** 键位盒子：`keys` 里只放"渲染用"的键（W/A/S/D 各一个盒子，方向键映射到同一个盒子）。 */
function KeyBox({ label, on, sub }: { label: string; on: boolean; sub?: string }) {
  return (
    <div
      className={`flex h-14 w-14 flex-col items-center justify-center rounded-xl text-[13px] font-semibold transition ${
        on
          ? 'bg-neon-cyan/25 text-neon-cyan ring-2 ring-neon-cyan/50'
          : 'bg-ink-900/60 text-slate-400 ring-1 ring-white/10'
      }`}
    >
      <span>{label}</span>
      {sub && <span className="num text-[10px] font-normal text-slate-500">{sub}</span>}
    </div>
  )
}

export function DrivePage() {
  const { telem, settings } = useStore()
  const t = telem
  const driving = !!t?.flags.host_drive

  // 速度：前进/后退的基准；转向：左右差（正 = 右转，与固件 T_DRIVE 的 b 同号）
  const [speed, setSpeed] = React.useState(600)
  const [turnRate, setTurnRate] = React.useState(500)
  const [heldKeys, setHeldKeys] = React.useState<string[]>([])
  const step = settings.driveStep

  const roles = React.useMemo(() => {
    const out: KeyRole[] = []
    for (const k of heldKeys) {
      const r = keyRoleById(k)
      if (r) out.push(r)
    }
    return out
  }, [heldKeys])

  const cmd = React.useMemo(() => rolesToDrive(roles, { speed, turn: turnRate }), [roles, speed, turnRate])
  const on = React.useCallback((r: KeyRole) => roles.includes(r), [roles])

  // 只在"指令变了"的时候下发一次；之后靠桥的 200ms 驾驶保活喂车上的 500ms 看门狗。
  const lastSent = React.useRef<string>('')
  React.useEffect(() => {
    const sig = JSON.stringify([cmd.mode, cmd.base, cmd.turn])
    if (sig === lastSent.current) return
    lastSent.current = sig
    if (cmd.mode === 'off') void store.drive({ mode: 'off' })
    else void store.drive({ mode: 'base_turn', base: cmd.base, turn: cmd.turn })
  }, [cmd.mode, cmd.base, cmd.turn])

  // 离开这个页面就把驾驶交还车上（不然切页之后车还在按最后一条指令跑）。
  React.useEffect(
    () => () => {
      if (lastSent.current && lastSent.current !== JSON.stringify(['off', 0, 0])) void store.drive({ mode: 'off' })
    },
    [],
  )

  React.useEffect(() => {
    const down = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.altKey || e.metaKey || e.isComposing) return
      if (isTypingTarget(e.target)) return
      const id = keyId(e)
      if (!id) return
      e.preventDefault()
      setHeldKeys((prev) => (prev.includes(id) ? prev : [...prev, id]))
    }
    const up = (e: KeyboardEvent) => {
      const id = keyId(e)
      if (!id) return
      e.preventDefault()
      setHeldKeys((prev) => prev.filter((k) => k !== id))
    }
    // 丢焦点（切窗口/点别处）就把所有键当松开：不能因为键盘事件没送到就一直开
    const reset = () => setHeldKeys([])
    const vis = () => {
      if (document.hidden) reset()
    }
    window.addEventListener('keydown', down)
    window.addEventListener('keyup', up)
    window.addEventListener('blur', reset)
    document.addEventListener('visibilitychange', vis)
    return () => {
      window.removeEventListener('keydown', down)
      window.removeEventListener('keyup', up)
      window.removeEventListener('blur', reset)
      document.removeEventListener('visibilitychange', vis)
    }
  }, [])

  const wheelL = cmd.base + cmd.turn
  const wheelR = cmd.base - cmd.turn

  return (
    <div className="grid gap-3 xl:grid-cols-[minmax(0,1fr)_380px]">
      <div className="grid gap-3">
        <Card
          title="键盘操控"
          right={
            <Badge tone={driving ? 'mint' : 'slate'}>
              上位机驾驶 {driving ? '开' : '关'}
            </Badge>
          }
        >
          <div className="flex flex-wrap items-start gap-6">
            <div className="select-none">
              <div className="grid grid-cols-3 gap-1.5">
                <span />
                <KeyBox label="W" sub="前进" on={on('fwd')} />
                <span />
                <KeyBox label="A" sub="左转" on={on('left')} />
                <KeyBox label="S" sub="后退" on={on('back')} />
                <KeyBox label="D" sub="右转" on={on('right')} />
              </div>
              <div className="mt-2 text-[11px] text-slate-500">
                方向键 ↑↓←→ 与 WASD 等价 · <span className="text-slate-400">空格</span> = 急停
              </div>
            </div>

            <div className="min-w-[240px] flex-1">
              <div className="mb-1 flex items-baseline gap-2">
                <span className="text-[13px] text-slate-300">当前指令</span>
                <span className="text-[13px] font-semibold text-neon-cyan">{cmd.label}</span>
              </div>
              <div className="grid grid-cols-2 gap-3">
                <Stat label="基准速度（前进量）" value={fmtInt(cmd.base)} unit="mm/s" tone={cmd.base ? 'mint' : undefined} />
                <Stat label="转向（左右差）" value={fmtInt(cmd.turn)} unit="mm/s" tone={cmd.turn ? 'amber' : undefined} />
                <Stat label="推算左轮" value={on('stop') ? '0' : fmtInt(wheelL)} unit="mm/s" />
                <Stat label="推算右轮" value={on('stop') ? '0' : fmtInt(wheelR)} unit="mm/s" />
              </div>
            </div>
          </div>

          <div className="mt-4 grid gap-4 md:grid-cols-2">
            <div>
              <div className="mb-1 flex items-center justify-between text-[13px]">
                <span className="text-slate-300">前进 / 后退速度</span>
                <span className="num text-slate-400">{speed} mm/s</span>
              </div>
              <Slider value={speed} min={0} max={3000} step={step} onChange={setSpeed} />
            </div>
            <div>
              <div className="mb-1 flex items-center justify-between text-[13px]">
                <span className="text-slate-300">转会速度（差速）</span>
                <span className="num text-slate-400">{turnRate} mm/s</span>
              </div>
              <Slider value={turnRate} min={0} max={3000} step={step} onChange={setTurnRate} />
            </div>
          </div>

          <div className="mt-4 flex flex-wrap items-center gap-2">
            <button className="btn-danger px-5 py-2 text-sm" onClick={() => void store.drive({ mode: 'off' })}>
              <OctagonX size={16} /> 停车（空格）
            </button>
            <span className="text-[12px] text-slate-500">
              松开所有键 = 自动交还车上任务流程；只要还按着，指令会一直被保活喂给车。
            </span>
          </div>
        </Card>

        <Card title="怎么按">
          <div className="grid gap-2 text-[13px] md:grid-cols-2">
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W</span>
              <span className="ml-2 text-slate-400">直着往前</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">S</span>
              <span className="ml-2 text-slate-400">直着往后</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">A</span>
              <span className="ml-2 text-slate-400">原地左转（左轮反转）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">D</span>
              <span className="ml-2 text-slate-400">原地右转</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + A</span>
              <span className="ml-2 text-slate-400">一边前进一边左转（斜着走）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + D</span>
              <span className="ml-2 text-slate-400">一边前进一边右转</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">A + D</span>
              <span className="ml-2 text-slate-400">左右抵消 ⇒ 转向为 0（W 还按着就是直行）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + S</span>
              <span className="ml-2 text-slate-400">前后抵消 ⇒ 不前进（A/D 还按着就原地转）</span>
            </div>
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
            <li>按 W/S 走、A/D 转；组合键就是差速：左转时右轮加速、左轮减速，正好是车上的三环串级 PID 在跟这个目标。</li>
            <li>只要这个页面开着并且按着键，桥每 200ms 会喂一次驾驶保活；真车模式下 0.5s 收不到会自动停车（防失联跑飞）。</li>
            <li>切走页面 / 窗口失焦 / 松开所有键 ⇒ 立刻交还车上任务流程，不会自己接着跑。</li>
            <li>比赛规则不允许遥控干预，这套只用于调试与标定。</li>
          </ul>
          <div className="mt-3">
            <Toggle
              on={!settings.safetyBanner}
              onChange={(v) => store.patchSettings({ safetyBanner: !v })}
              label="不再显示操控页安全提示"
            />
          </div>
        </Card>
      </div>
    </div>
  )
}
