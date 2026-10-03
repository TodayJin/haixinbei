import React from 'react'
import { store, useStore } from '../lib/store'
import { AttitudeGauge, Badge, Card, NumberField, Stat } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { fmt, fmtSigned, normDeg } from '../lib/format'

export function ImuPage() {
  const { telem, link } = useStore()
  const t = telem
  const [cmd, setCmd] = React.useState(0)

  const fwCaps = link?.fw?.caps ?? 0

  return (
    <div className="grid gap-3 xl:grid-cols-[420px_minmax(0,1fr)]">
      <Card title="姿态" right={<Badge tone={t?.flags.gyro_ok ? 'mint' : 'rose'}>{t?.flags.gyro_ok ? '陀螺在线' : '陀螺离线'}</Badge>}>
        <AttitudeGauge yaw={t?.yaw ?? 0} pitch={t?.pitch ?? 0} roll={t?.roll ?? 0} size={210} />
        <div className="mt-3 grid grid-cols-3 gap-3">
          <Stat label="偏航" value={fmt(t?.yaw, 1)} unit="°" tone="cyan" />
          <Stat label="俯仰" value={fmt(t?.pitch, 1)} unit="°" tone="violet" />
          <Stat label="横滚" value={fmt(t?.roll, 1)} unit="°" tone="mint" />
        </div>
        <div className="mt-3 grid gap-3">
          <div>
            <div className="mb-1 flex justify-between text-[12px] text-slate-400">
              <span>偏航趋势</span>
              <span className="num">{fmtSigned(normDeg(t?.yaw ?? 0), 1)}°</span>
            </div>
            <MiniTrend channel="yaw" height={44} />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <div className="mb-1 text-[12px] text-slate-400">俯仰</div>
              <MiniTrend channel="pitch" height={36} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">横滚</div>
              <MiniTrend channel="roll" height={36} />
            </div>
          </div>
        </div>
      </Card>

      <div className="grid gap-3">
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

        <Card title="陀螺操作">
          <div className="flex flex-wrap items-end gap-3">
            <div>
              <div className="mb-1 text-[12px] text-slate-400">转角命令（度，正=左转）</div>
              <NumberField value={cmd} onChange={setCmd} min={-360} max={360} step={5} unit="°" />
            </div>
            <button
              className="btn"
              onClick={() => void store.cmd('gyro_turn', cmd)}
              title={(fwCaps & 0x1) === 0 ? '固件没声明支持也不影响试：桥会照发' : undefined}
            >
              转这个角度
            </button>
            <button className="btn-ghost" onClick={() => void store.cmd('gyro_zero')}>
              当前角作为 0°
            </button>
            <button className="btn-ghost" onClick={() => void store.cmd('gyro_cal')}>
              陀螺校准
            </button>
            <span className="text-[12px] text-slate-500">
              固件能力位 0x{fwCaps.toString(16)}（bit0 遥测2 / bit1 动作表 / bit2 舵机 / bit3 驾驶 / bit4 标定 / bit5 Flash / bit6 看门狗）
            </span>
          </div>
        </Card>

        <Card title="温度/状态（如果有）">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="遥测帧率" value={fmt(link?.telem_hz, 1)} unit="Hz" />
            <Stat label="主循环" value={fmt(t?.loop_us, 0)} unit="µs" />
            <Stat label="状态机" value={`${t?.mode ?? '--'}`} hint={`renwu ${t?.renwu ?? 0}`} />
            <Stat label="动作号" value={`${t?.action ?? '--'}`} />
          </div>
        </Card>
      </div>
    </div>
  )
}
