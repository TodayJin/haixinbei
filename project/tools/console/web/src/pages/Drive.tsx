import React from 'react'
import { OctagonX } from 'lucide-react'
import { store, useStore } from '../lib/store'
import { Badge, Card, Slider, Stat, Toggle } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { fmt, fmtInt } from '../lib/format'
import { isTypingTarget, keyId, keyRoleById, rolesToIntent, type KeyRole } from '../lib/keys'
import {
  DEAD_SCALE,
  MAX_LEAD,
  PWM_MAX,
  TURN_RATE_LIMIT,
  TURN_TICK_MS,
  initDriveRuntime,
  norm180,
  offsetLabel,
  stepDrive,
  type DriveRuntime,
} from '../lib/heading'

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

  // 速度：前进/后退的基准**速度刻度**（0..7200 = 固件速度环的目标刻度，不是 mm/s）；
  // 转向：按住 A/D 时航向每秒变多少度（车上是角度环在追）。
  // 默认 3000：刻度 1000 以下是静摩擦死区（见 lib/heading.ts 的 DEAD_SCALE），一上手先给个能跑起来的值。
  const [speed, setSpeed] = React.useState(3000)
  const [turnRate, setTurnRate] = React.useState(90)
  const [heldKeys, setHeldKeys] = React.useState<string[]>([])
  const step = settings.driveStep
  // 「跳过死区」打开时滑块从 DEAD_SCALE 起步；关掉就是完整的 0..7200（"0 到 7200 都能设置"）。
  const speedMin = settings.driveSkipDead ? DEAD_SCALE : 0

  React.useEffect(() => {
    // 打开「跳过死区」时如果当前值还在死区里，直接抬到可动区起点
    if (speed < speedMin) setSpeed(speedMin)
  }, [speed, speedMin])

  const roles = React.useMemo(() => {
    const out: KeyRole[] = []
    for (const k of heldKeys) {
      const r = keyRoleById(k)
      if (r) out.push(r)
    }
    return out
  }, [heldKeys])

  const input = React.useMemo(() => rolesToIntent(roles, { speed, turn: turnRate }), [roles, speed, turnRate])
  const on = React.useCallback((r: KeyRole) => roles.includes(r), [roles])

  // 状态机放在纯函数里（lib/heading.ts stepDrive）：这里只负责"按节拍喂意图、把帧发出去"。
  const rt = React.useRef<DriveRuntime>(initDriveRuntime())
  const intentRef = React.useRef(input)
  intentRef.current = input
  const yawRef = React.useRef(0)
  yawRef.current = t?.yaw ?? 0
  // 接管那一刻的航向就是车上 `s_base_yaw`（HostExt_DriveSet 进入 base 模式时锁存），
  // 页面上要显示目标航向就得记住它。
  const baseYawRef = React.useRef<number | null>(null)
  const [offset, setOffset] = React.useState(0)
  // 目标领先车头多少度（stepDrive 用它做 MAX_LEAD 保护：领先超过 180° 车上的"最近路"误差会翻号）
  const [lead, setLead] = React.useState(0)

  const pump = React.useCallback((dtMs: number) => {
    const wasArmed = rt.current.armed
    const r = stepDrive(rt.current, intentRef.current, dtMs, yawRef.current)
    rt.current = r.rt
    setOffset(r.rt.offset)
    setLead(r.rt.lead)
    if (!wasArmed && r.rt.armed) baseYawRef.current = yawRef.current
    if (!r.rt.armed) baseYawRef.current = null
    for (const f of r.send) void store.drive(f)
  }, [])

  // 车把驾驶会话丢了（链路丢一帧 ⇒ 桥等 0.6s 才重发 ⇒ 车的 500ms 看门狗先停车；或低压保护/
  // 任务抢电机）：车再进来时会**重新锁存**基准航向（host_ext.c:196-198 的 s_base_yaw = Yaw），
  // 页面要是继续发同一个累计偏移，那个偏移就被再叠加一次 —— 2026-10-07 真车实测这一下是
  // **+73.7° 的突跳**（out/_pw_before180.txt：t=2514~2609ms 有 18 帧 mode=0/host_drive=0，
  // 由 err 反推的基准航向从 126.3° 跳到 −160.2°）。所以这里跟着重新对零：车保持当前朝向，
  // 页面从"现在"重新开始积分（与"松手/空格 ⇒ 重新对零"同一条口径）。
  const drivingRef = React.useRef(false)
  React.useEffect(() => {
    const was = drivingRef.current
    drivingRef.current = driving
    if (was && !driving && rt.current.armed) {
      rt.current = initDriveRuntime()
      baseYawRef.current = null
      setOffset(0)
      setLead(0)
    }
  }, [driving])

  const turning = input.turnRate !== 0 && !input.stop

  // 意图一变立刻结算一次；按着转向键的时候每 TURN_TICK_MS 再积分一拍。
  React.useEffect(() => {
    pump(0)
    if (!turning) return
    const id = window.setInterval(() => pump(TURN_TICK_MS), TURN_TICK_MS)
    return () => window.clearInterval(id)
  }, [input.base, input.turnRate, input.stop, turning, pump])

  // 离开这个页面就把驾驶交还车上（不然切页之后车还在按最后一条指令跑）。
  React.useEffect(() => () => {
    if (rt.current.armed) void store.drive({ mode: 'off' })
  }, [])

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

  const baseYaw = baseYawRef.current
  const target = baseYaw === null ? null : norm180(baseYaw + offset)

  const halt = () => {
    setHeldKeys([])
    rt.current = initDriveRuntime()
    baseYawRef.current = null
    setOffset(0)
    void store.drive({ mode: 'off' })
  }

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

            <div className="min-w-[260px] flex-1">
              <div className="mb-1 flex items-baseline gap-2">
                <span className="text-[13px] text-slate-300">当前指令</span>
                <span className="text-[13px] font-semibold text-neon-cyan">{input.label}</span>
              </div>
              <div className="grid grid-cols-2 gap-3">
                <Stat label="基准速度（前进量）" value={fmtInt(input.base)} unit={`/${PWM_MAX}`} tone={input.base ? 'mint' : undefined} />
                <Stat
                  label="转向速度"
                  value={fmtInt(input.turnRate)}
                  unit="°/s"
                  tone={input.turnRate ? 'amber' : undefined}
                  hint="按住 A/D 时航向每秒变多少"
                />
                <Stat
                  label="目标偏移"
                  value={fmtInt(offset)}
                  unit="°"
                  tone={offset ? 'amber' : undefined}
                  hint={
                    driving
                      ? `${offsetLabel(offset)} · 目标领先车头 ${fmtInt(lead)}°${
                          Math.abs(lead) >= MAX_LEAD - 1 ? '（已到领先上限：目标贴着车走）' : ''
                        }`
                      : offsetLabel(offset)
                  }
                />
                <Stat
                  label="目标航向"
                  value={target === null ? '--' : fmt(target, 1)}
                  unit="°"
                  tone="cyan"
                  hint={baseYaw === null ? '还没接管' : `基准 ${fmt(baseYaw, 1)}° + 偏移`}
                />
              </div>
            </div>
          </div>

          <div className="mt-4 grid gap-4 md:grid-cols-2">
            <div>
              <div className="mb-1 flex items-center justify-between text-[13px]">
                <span className="text-slate-300">前进 / 后退速度刻度</span>
                <span className="num text-slate-400">
                  {speed} / {PWM_MAX}
                  {speed < DEAD_SCALE && <span className="ml-2 text-amber-400/80">死区内，车可能要等几秒才动</span>}
                </span>
              </div>
              <Slider value={speed} min={speedMin} max={PWM_MAX} step={step} onChange={setSpeed} />
              <div className="mt-1 text-[11px] leading-snug text-slate-500">
                0..{PWM_MAX} 是固件速度环的目标刻度（不是 mm/s；{PWM_MAX} = 满量程 ≈ 516 mm/s）。刻度 {DEAD_SCALE} 以下时轮上
                PWM 还不到 3000，实测是静摩擦死区（前馈 PWM ≈ 2327 + 0.662 × 刻度）。
              </div>
              <div className="mt-2">
                <Toggle
                  on={settings.driveSkipDead}
                  onChange={(v) => store.patchSettings({ driveSkipDead: v })}
                  label={`跳过静摩擦死区（滑块直接从 ${DEAD_SCALE} 起步）`}
                />
              </div>
            </div>
            <div>
              <div className="mb-1 flex items-center justify-between text-[13px]">
                <span className="text-slate-300">转向速度（角度环给的目标角速度）</span>
                <span className="num text-slate-400">{turnRate} °/s</span>
              </div>
              <Slider value={turnRate} min={0} max={TURN_RATE_LIMIT} step={5} onChange={setTurnRate} />
              <div className="mt-1 text-[11px] leading-snug text-slate-500">
                这是「每秒想转多少度」，不是保证值：<span className="text-slate-300">原地转速实测上限 ≈80~85°/s</span>
                （设定 30 → 实测 30.0、60 → 48.8、90 → 75.9、120 → 79.2、180 → 也只有 ~79）。设得再高只是「尽快转」。
                目标一旦领先车头超过 180°，车上的「最近路」误差会翻号让车
                <span className="text-amber-400/90">倒着拐</span>，所以页面把领先量卡在 {MAX_LEAD}°（车跟得上时这条线碰不到）。
              </div>
            </div>
          </div>

          <div className="mt-4 flex flex-wrap items-center gap-2">
            <button className="btn-danger px-5 py-2 text-sm" onClick={halt}>
              <OctagonX size={16} /> 停车（空格）
            </button>
            <span className="text-[12px] text-slate-500">
              松开所有键 = 自动交还车上任务流程；只要还按着，桥每 200ms 喂一次驾驶保活。
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
              <span className="ml-2 text-slate-400">
                按住就以设定角速度左打轮，松开保持当前朝向（最多累计 540°）：前进时车头往左，倒车时车尾往左
              </span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">D</span>
              <span className="ml-2 text-slate-400">同上，向右转</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + A</span>
              <span className="ml-2 text-slate-400">一边前进一边左转（弧线）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + D</span>
              <span className="ml-2 text-slate-400">一边前进一边右转</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">S + D</span>
              <span className="ml-2 text-slate-400">
                一边后退一边右打轮 = <span className="text-slate-300">车尾往右摆</span>（往右后方走）
              </span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">S + A</span>
              <span className="ml-2 text-slate-400">
                一边后退一边左打轮 = <span className="text-slate-300">车尾往左摆</span>（往左后方走）
              </span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">A + D</span>
              <span className="ml-2 text-slate-400">左右抵消 ⇒ 不再转（W 还按着就是直行）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2">
              <span className="num font-semibold text-slate-200">W + S</span>
              <span className="ml-2 text-slate-400">前后抵消 ⇒ 不前进（A/D 还按着就原地转）</span>
            </div>
            <div className="rounded-lg bg-ink-900/50 px-3 py-2 md:col-span-2">
              <span className="num font-semibold text-slate-200">全松开 / 空格</span>
              <span className="ml-2 text-slate-400">
                交还车上任务流程并把"基准朝向"重新对到当前朝向 —— 下次按 A/D 是从新的正前方开始算
              </span>
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card title="实时反馈">
          <div className="grid grid-cols-2 gap-3">
            <Stat label="实测左 / 右" value={`${fmt(t?.vl, 0)} / ${fmt(t?.vr, 0)}`} unit="mm/s" />
            <Stat label="PWM 左 / 右" value={`${fmtInt(t?.pwm_l)} / ${fmtInt(t?.pwm_r)}`} tone="amber" title="PWM = 左右轮实际拿到的占空比刻度（0..7200，负数是反转）" />
            <Stat label="当前航向" value={fmt(t?.yaw, 1)} unit="°" tone="cyan" />
            <Stat label="角度环误差" value={fmt(t?.angle.err, 2)} unit="°" tone="violet" hint="目标 − 当前" />
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
            <li>
              这里只下发<span className="text-slate-200">目标速度 + 目标转向角</span>，怎么打轮是车上的角度环/速度环在算
              （`base_turn` 模式）。原地转就是基准速度给 0。
            </li>
            <li>转向不是"左右轮差速 mm/s"，而是目标航向角（度，正 = 左转）：角度环把车头拧到那个朝向并保持。</li>
            <li>
              前进/后退速度是<span className="text-slate-200">速度刻度 0..{PWM_MAX}</span>（固件速度环的目标刻度，不是 mm/s）：
              {PWM_MAX} = 满量程。刻度 {DEAD_SCALE} 以下（轮上 PWM 不到 3000）实测是静摩擦死区，速度环要顶好几秒才动 ——
              开上面的「跳过死区」滑块就从可动区起步。
            </li>
            <li>
              倒车打轮会自动翻过来：按住 <span className="num text-slate-200">S+D</span> 是
              <span className="text-slate-200">车尾往右摆</span>（往右后方走），<span className="num text-slate-200">S+A</span>
              是车尾往左摆 —— 不翻的话车会往反方向跑。
            </li>
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
