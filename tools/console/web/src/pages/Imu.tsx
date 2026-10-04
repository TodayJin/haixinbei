import React from 'react'
import { store, useHotSelect } from '../lib/store'
import { Badge, Card, Slider, Sparkline, Stat } from '../components/ui'
import { MiniTrend, useChannelView } from '../components/Trend'
import { Car3D } from '../components/Car3D'
import { CHANNEL_BY_KEY } from '../lib/channels'
import { fmt, fmtInt, fmtSigned, normDeg } from '../lib/format'
import { TURN_TICK_MS, norm180, offsetLabel } from '../lib/heading'
import { compassText } from '../lib/attitude3d'
import { taskText } from '../lib/types'

/** 三张姿态波形的窗口（和 MiniTrend 的默认一致，写出来是为了印在卡片上） */
const WAVE_WINDOW_MS = 20000

/**
 * 一张姿态波形卡（偏航/俯仰/横滚）：窗口内最小/最大/当前 + 趋势线。
 * 纵轴**以 0 为中心**（zeroScale）：姿态角自动缩放会把 0 附近的抖动拉满整屏，看着像在剧烈晃。
 *
 * 取数走 `useChannelView()`：抽稀相位稳定（窗口每滑一格，老的点不动）+ 整窗 min/max
 * 与末值**一趟扫描**算完；不再 `Math.min(...data)` 展开几百个实参，也不再按整窗拷数组。
 */
function WaveCard({ channel }: { channel: string }) {
  const def = CHANNEL_BY_KEY[channel]
  const view = useChannelView(channel, WAVE_WINDOW_MS, 240)
  const u = def.unit
  return (
    <Card
      title={`${def.label}波形`}
      right={
        <span className="num text-[12px]" style={{ color: def.color }}>
          {fmt(view.last, def.decimals)}
          {u}
        </span>
      }
    >
      <Sparkline data={view.values} color={def.color} height={96} className="h-24 w-full" zeroScale />
      <div className="mt-2 grid grid-cols-3 gap-2 text-[11px] text-slate-500">
        <span>
          最小{' '}
          <span className="num text-slate-300">
            {fmt(view.min, def.decimals)}
            {u}
          </span>
        </span>
        <span>
          最大{' '}
          <span className="num text-slate-300">
            {fmt(view.max, def.decimals)}
            {u}
          </span>
        </span>
        <span className="text-right">窗口 {WAVE_WINDOW_MS / 1000}s</span>
      </div>
    </Card>
  )
}

/**
 * 下面几张卡各自只订**自己用到的标量**（`useHotSelect`，值没变就不重渲染）。
 *
 * 这一页原来是整页 `useStore()`：每个热 tick（50ms 合并 ⇒ 20Hz）都重渲染整棵树 ——
 * Car3D 上百个 SVG 节点、六七张卡、三条波形全部重算。页面壳（滑块那点状态）现在
 * 一个 tick 都不动，只有"数字真的变了"的那张卡重渲染。
 */

/** 车姿态（3D）+ 三个读数 */
function AttitudeCard() {
  const yaw = useHotSelect((s) => s.telem?.yaw ?? null)
  const pitch = useHotSelect((s) => s.telem?.pitch ?? null)
  const roll = useHotSelect((s) => s.telem?.roll ?? null)
  const gyroOk = useHotSelect((s) => s.telem?.flags.gyro_ok ?? false)
  return (
    <Card
      title="车姿态（3D）"
      right={<Badge tone={gyroOk ? 'mint' : 'rose'}>{gyroOk ? '陀螺在线' : '陀螺离线'}</Badge>}
    >
      <Car3D yaw={yaw ?? undefined} pitch={pitch ?? undefined} roll={roll ?? undefined} />
      <div className="mt-3 grid grid-cols-3 gap-3">
        <Stat
          label="偏航"
          value={fmt(yaw, 1)}
          unit="°"
          tone="cyan"
          hint={`罗盘 ${compassText(yaw ?? undefined, 0)}`}
        />
        <Stat label="俯仰" value={fmt(pitch, 1)} unit="°" tone="violet" hint="模型：+ = 向左倾" />
        <Stat label="横滚" value={fmt(roll, 1)} unit="°" tone="mint" hint="模型：+ = 低头" />
      </div>
      <div className="mt-2 text-[11px] leading-relaxed text-slate-500">
        3D 模型跟固件 OLED 上那个方框同口径（<span className="text-slate-400">app_ui.c 的 gyro_view_angles()，默认档 M3</span>）：
        固件把遥测的 <span className="text-slate-400">pitch 当横滚源（取反）、roll 当俯仰源</span>，
        所以模型是 <span className="text-slate-400">rotateZ(+pitch) rotateX(−roll)</span>
        —— 俯仰读数变大车向左倾、横滚读数变大车低头。
      </div>
      <div className="mt-1 text-[11px] leading-relaxed text-slate-500">
        两个读数的取值没动，仍是遥测原始帧的 pitch/roll（跟波形页同一把尺子）；
        固件屏上右侧那三行数字走的是另一套取反习惯，默认档下跟方框的符号并不一致。
      </div>
    </Card>
  )
}

/** 朝向控制：滑块那点状态在页面壳里，读数在这里窄订阅 */
function HeadingCard({
  offset,
  armed,
  baseYaw,
  target,
  onChange,
  release,
}: {
  offset: number
  armed: boolean
  baseYaw: number | null
  target: number | null
  onChange: (v: number) => void
  release: () => void
}) {
  const yaw = useHotSelect((s) => s.telem?.yaw ?? null)
  const err = useHotSelect((s) => s.telem?.angle?.err ?? null)
  return (
    <Card title="朝向控制" right={<Badge tone={armed ? 'mint' : 'slate'}>{armed ? '角度环接管中' : '未接管'}</Badge>}>
      <div className="flex items-baseline justify-between text-[13px]">
        <span className="text-slate-300">目标航向偏移（+ = 左转 = 偏航角增大）</span>
        <span className="num text-slate-400">
          {fmtInt(offset)}° {offset !== 0 && <span className="text-slate-500">（{offsetLabel(offset)}）</span>}
        </span>
      </div>
      <div className="mt-1">
        <Slider value={offset} min={-180} max={180} step={1} onChange={onChange} />
      </div>
      <div className="mt-1 flex justify-between text-[11px] text-slate-500">
        <span>−180° 右转到底</span>
        <span>0° 基准朝向</span>
        <span>+180° 左转到底</span>
      </div>

      <div className="mt-3 grid grid-cols-2 gap-3 md:grid-cols-4">
        <Stat
          label="基准朝向"
          value={baseYaw === null ? '--' : fmt(baseYaw, 1)}
          unit="°"
          hint="第一次拖动那一刻的航向"
        />
        <Stat label="目标朝向" value={target === null ? '--' : fmt(target, 1)} unit="°" tone="cyan" hint="基准 + 偏移" />
        <Stat label="当前朝向" value={fmt(yaw, 1)} unit="°" tone="mint" />
        <Stat label="角度环误差" value={fmtSigned(err, 2)} unit="°" tone="violet" hint="目标 − 当前" />
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <button className="btn-ghost" onClick={() => onChange(0)}>
          回到 0°
        </button>
        <button className="btn-ghost" onClick={release} disabled={!armed}>
          交还车上任务
        </button>
        <span className="text-[12px] text-slate-500">
          滑块值就是<span className="text-slate-300">下发的目标转角（度）</span>：车上的角度环 + 速度环把车头拧过去并保持；
          基准速度给 0 = 原地转。松开滑块继续保持这个朝向，改值就是换目标。
        </span>
      </div>
    </Card>
  )
}

/** 角度环内部量：四个读数 + 三条迷你趋势 */
function AngleLoopCard() {
  const yaw = useHotSelect((s) => s.telem?.yaw ?? null)
  const err = useHotSelect((s) => s.telem?.angle?.err ?? null)
  const rateRef = useHotSelect((s) => s.telem?.angle?.rate_ref ?? null)
  const corr = useHotSelect((s) => s.telem?.angle?.corr ?? null)
  return (
    <Card title="角度环内部量">
      <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Stat label="目标角" value={fmt(normDeg(err ?? 0) + (yaw ?? 0), 1)} unit="°" hint="= 当前 + 误差" />
        <Stat label="误差" value={fmtSigned(err, 2)} unit="°" tone="violet" />
        <Stat label="期望角速度" value={fmt(rateRef, 1)} unit="°/s" tone="cyan" />
        <Stat label="修正输出" value={fmtSigned(corr, 0)} tone="amber" />
      </div>
      <div className="mt-3 grid gap-3 md:grid-cols-3">
        <div>
          <div className="mb-1 text-[12px] text-slate-400">误差</div>
          <MiniTrend channel="angle_err" height={36} />
        </div>
        <div>
          <div className="mb-1 text-[12px] text-slate-400">期望角速度</div>
          <MiniTrend channel="rate_ref" height={36} />
        </div>
        <div>
          <div className="mb-1 text-[12px] text-slate-400">修正输出</div>
          <MiniTrend channel="angle_corr" height={36} />
        </div>
      </div>
    </Card>
  )
}

/** 车上上报的"支持哪些功能"：只让人看功能名，bit 号/十六进制留到悬停说明里 */
const FW_CAPS: { bit: number; name: string }[] = [
  { bit: 0x01, name: '遥测' },
  { bit: 0x02, name: '动作表' },
  { bit: 0x04, name: '舵机' },
  { bit: 0x08, name: '上位机驾驶' },
  { bit: 0x10, name: '标定' },
  { bit: 0x20, name: 'Flash 读写' },
  { bit: 0x40, name: '看门狗' },
  { bit: 0x80, name: '任务' },
]

/** 温度/状态（如果有）：固件节拍与模式 */
function StatusCard() {
  const telemHz = useHotSelect((s) => s.link?.telem_hz ?? null)
  const loopUs = useHotSelect((s) => s.telem?.loop_us ?? null)
  const mode = useHotSelect((s) => s.telem?.mode ?? null)
  const taskId = useHotSelect((s) => s.telem?.task_id ?? null)
  const action = useHotSelect((s) => s.telem?.action ?? null)
  const fwCaps = useHotSelect((s) => s.link?.fw?.caps ?? 0)
  return (
    <Card title="温度/状态（如果有）">
      <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Stat label="遥测帧率" value={fmt(telemHz, 1)} unit="Hz" />
        <Stat label="主循环" value={fmt(loopUs, 0)} unit="µs" />
        <Stat label="整车模式" value={`${mode ?? '--'}`} hint={`当前任务 ${taskText(taskId ?? undefined)}`} />
        <Stat label="动作号" value={`${action ?? '--'}`} />
      </div>
      <div
        className="mt-3 text-[12px] text-slate-500"
        title={
          `车上固件的能力位（内部编号 0x${fwCaps.toString(16)}）：\n` +
          FW_CAPS.map((c) => `${fwCaps & c.bit ? '✓' : '✗'} ${c.name}`).join('\n')
        }
      >
        车上支持的功能：{FW_CAPS.filter((c) => fwCaps & c.bit).map((c) => c.name).join(' · ') || '--（车上固件没上报）'}
      </div>
    </Card>
  )
}

export function ImuPage() {
  const [offset, setOffset] = React.useState(0)
  const [armed, setArmed] = React.useState(false)

  // 朝向控制：滑块 → `base_turn`（基准速度 0 + 目标偏移度），车上的角度环把车头
  // 拧到这个朝向并保持。滑动条拖很快，挡一下频率再下发（桥那边只管转发）。
  const baseYawRef = React.useRef<number | null>(null)
  const armedRef = React.useRef(false)
  const offsetRef = React.useRef(0)
  const lastMsRef = React.useRef(0)
  const timerRef = React.useRef<number | null>(null)

  const pushNow = () => {
    if (!armedRef.current) {
      // 第一次下发 = 接管那一刻：车上的 s_base_yaw 就是这时候的航向，记下来显示目标。
      // 航向直接从快照读：这一页的壳不订阅 telem，就不必为了记这一行每 tick 重渲染。
      armedRef.current = true
      baseYawRef.current = store.getSnapshot().telem?.yaw ?? 0
      setArmed(true)
    }
    void store.drive({ mode: 'base_turn', base: 0, turn: Math.round(offsetRef.current) })
  }

  const onChange = (v: number) => {
    setOffset(v)
    offsetRef.current = v
    const now = Date.now()
    if (now - lastMsRef.current >= TURN_TICK_MS) {
      lastMsRef.current = now
      pushNow()
    } else if (timerRef.current === null) {
      // 拖动的尾巴补一次，别丢掉最后一个值
      timerRef.current = window.setTimeout(() => {
        timerRef.current = null
        lastMsRef.current = Date.now()
        pushNow()
      }, TURN_TICK_MS)
    }
  }

  const release = () => {
    if (timerRef.current !== null) {
      window.clearTimeout(timerRef.current)
      timerRef.current = null
    }
    if (!armedRef.current) return
    armedRef.current = false
    baseYawRef.current = null
    setArmed(false)
    void store.drive({ mode: 'off' })
  }

  // 离开页面就把驾驶交还车上（不然切页之后车头还在被角度环按着）
  React.useEffect(() => () => {
    if (timerRef.current !== null) window.clearTimeout(timerRef.current)
    if (armedRef.current) void store.drive({ mode: 'off' })
  }, [])

  const baseYaw = baseYawRef.current
  const target = baseYaw === null ? null : norm180(baseYaw + offset)

  return (
    <div className="grid gap-3">
      <div className="grid gap-3 xl:grid-cols-2">
        <AttitudeCard />
        <HeadingCard
          offset={offset}
          armed={armed}
          baseYaw={baseYaw}
          target={target}
          onChange={onChange}
          release={release}
        />
      </div>

      <div className="grid gap-3 lg:grid-cols-3">
        <WaveCard channel="yaw" />
        <WaveCard channel="pitch" />
        <WaveCard channel="roll" />
      </div>

      <div className="grid gap-3 xl:grid-cols-2">
        <AngleLoopCard />
        <StatusCard />
      </div>
    </div>
  )
}
