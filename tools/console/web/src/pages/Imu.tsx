import React from 'react'
import { store, useStore } from '../lib/store'
import { Badge, Card, Slider, Sparkline, Stat } from '../components/ui'
import { MiniTrend, useChannelSeries } from '../components/Trend'
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
 */
function WaveCard({ channel }: { channel: string }) {
  const def = CHANNEL_BY_KEY[channel]
  const data = useChannelSeries(channel, WAVE_WINDOW_MS, 240)
  const last = data.length ? data[data.length - 1] : undefined
  const min = data.length ? Math.min(...data) : undefined
  const max = data.length ? Math.max(...data) : undefined
  const u = def.unit
  return (
    <Card
      title={`${def.label}波形`}
      right={
        <span className="num text-[12px]" style={{ color: def.color }}>
          {fmt(last, def.decimals)}
          {u}
        </span>
      }
    >
      <Sparkline data={data} color={def.color} height={96} className="h-24 w-full" zeroScale />
      <div className="mt-2 grid grid-cols-3 gap-2 text-[11px] text-slate-500">
        <span>
          最小{' '}
          <span className="num text-slate-300">
            {fmt(min, def.decimals)}
            {u}
          </span>
        </span>
        <span>
          最大{' '}
          <span className="num text-slate-300">
            {fmt(max, def.decimals)}
            {u}
          </span>
        </span>
        <span className="text-right">窗口 {WAVE_WINDOW_MS / 1000}s</span>
      </div>
    </Card>
  )
}

export function ImuPage() {
  const { telem, link } = useStore()
  const t = telem
  const [offset, setOffset] = React.useState(0)
  const [armed, setArmed] = React.useState(false)

  const fwCaps = link?.fw?.caps ?? 0

  // 朝向控制：滑块 → `base_turn`（基准速度 0 + 目标偏移度），车上的角度环把车头
  // 拧到这个朝向并保持。滑动条拖很快，挡一下频率再下发（桥那边只管转发）。
  const yawRef = React.useRef(0)
  yawRef.current = t?.yaw ?? 0
  const baseYawRef = React.useRef<number | null>(null)
  const armedRef = React.useRef(false)
  const offsetRef = React.useRef(0)
  const lastMsRef = React.useRef(0)
  const timerRef = React.useRef<number | null>(null)

  const pushNow = () => {
    if (!armedRef.current) {
      // 第一次下发 = 接管那一刻：车上的 s_base_yaw 就是这时候的航向，记下来显示目标
      armedRef.current = true
      baseYawRef.current = yawRef.current
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
        <Card
          title="车姿态（3D）"
          right={<Badge tone={t?.flags.gyro_ok ? 'mint' : 'rose'}>{t?.flags.gyro_ok ? '陀螺在线' : '陀螺离线'}</Badge>}
        >
          <Car3D yaw={t?.yaw} pitch={t?.pitch} roll={t?.roll} />
          <div className="mt-3 grid grid-cols-3 gap-3">
            <Stat label="偏航" value={fmt(t?.yaw, 1)} unit="°" tone="cyan" hint={`罗盘 ${compassText(t?.yaw, 0)}`} />
            <Stat label="俯仰" value={fmt(t?.pitch, 1)} unit="°" tone="violet" hint="+ = 抬头" />
            <Stat label="横滚" value={fmt(t?.roll, 1)} unit="°" tone="mint" hint="+ = 向左倾" />
          </div>
          <div className="mt-2 text-[11px] leading-relaxed text-slate-500">
            模型和读数用同一把尺子（遥测原始帧）：<span className="text-slate-400">+偏航 = 左转、+俯仰 = 抬头、+横滚 = 向左倾</span>
            。固件给 OLED 画图时另做了一次轴序交换（<span className="text-slate-400">app_ui.c 的 M1..M8，默认 M3</span>），
            真车上哪个方向反了，就是 <span className="text-slate-400">lib/attitude3d.ts</span> 里一行符号的事。
          </div>
        </Card>

        <Card
          title="朝向控制"
          right={<Badge tone={armed ? 'mint' : 'slate'}>{armed ? '角度环接管中' : '未接管'}</Badge>}
        >
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
            <Stat label="当前朝向" value={fmt(t?.yaw, 1)} unit="°" tone="mint" />
            <Stat label="角度环误差" value={fmtSigned(t?.angle.err, 2)} unit="°" tone="violet" hint="目标 − 当前" />
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
      </div>

      <div className="grid gap-3 lg:grid-cols-3">
        <WaveCard channel="yaw" />
        <WaveCard channel="pitch" />
        <WaveCard channel="roll" />
      </div>

      <div className="grid gap-3 xl:grid-cols-2">
        <Card title="角度环内部量">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="目标角" value={fmt(normDeg(t?.angle.err ?? 0) + (t?.yaw ?? 0), 1)} unit="°" hint="= 当前 + 误差" />
            <Stat label="误差" value={fmtSigned(t?.angle.err, 2)} unit="°" tone="violet" />
            <Stat label="期望角速度" value={fmt(t?.angle.rate_ref, 1)} unit="°/s" tone="cyan" />
            <Stat label="修正输出" value={fmtSigned(t?.angle.corr, 0)} tone="amber" />
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

        <Card title="温度/状态（如果有）">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="遥测帧率" value={fmt(link?.telem_hz, 1)} unit="Hz" />
            <Stat label="主循环" value={fmt(t?.loop_us, 0)} unit="µs" />
            <Stat label="整车模式" value={`${t?.mode ?? '--'}`} hint={`当前任务 ${taskText(t?.task_id)}`} />
            <Stat label="动作号" value={`${t?.action ?? '--'}`} />
          </div>
          <div className="mt-3 text-[12px] text-slate-500">
            固件能力位 0x{fwCaps.toString(16)}（bit0 遥测2 / bit1 动作表 / bit2 舵机 / bit3 驾驶 / bit4 标定 / bit5 Flash / bit6 看门狗 / bit7 任务）
          </div>
        </Card>
      </div>
    </div>
  )
}
