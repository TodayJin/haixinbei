import React from 'react'
import { Plug, Power, RefreshCw, Zap } from 'lucide-react'
import { store, useStore } from '../lib/store'
import { Badge, Card, GrayArray, Stat, StatusDot } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { fmt, fmtClock, fmtInt, fmtUptime, pwmToPct } from '../lib/format'
import { MODE_TEXT, NVRAM_TEXT, taskText, type Telem } from '../lib/types'

function Flags({ t }: { t: Telem | null }) {
  if (!t) return <span className="text-[12px] text-slate-500">暂无遥测</span>
  const f = t.flags
  const items: [string, boolean, boolean?][] = [
    ['丢线', f.lost, true],
    ['坡度', f.slope],
    ['陀螺在线', f.gyro_ok],
    ['电机输出', f.motor],
    ['上位机驾驶', f.host_drive, true],
    ['记录中', f.logging],
    ['Flash 忙', f.flash],
    ['故障', f.fault, true],
  ]
  return (
    <div className="flex flex-wrap gap-1.5">
      {items.map(([label, on, warn]) => (
        <Badge key={label} tone={on ? (warn ? 'rose' : 'mint') : 'slate'}>
          <StatusDot on={on} tone={on ? (warn ? 'rose' : 'mint') : 'slate'} pulse={!!on && !!warn} />
          {label}
        </Badge>
      ))}
    </div>
  )
}

export function OverviewPage() {
  const { link, telem, stats, params, sse } = useStore()
  const [busy, setBusy] = React.useState(false)
  const t = telem

  const connect = async (mode: 'sim' | 'serial', port?: string) => {
    setBusy(true)
    await store.connect(mode, port)
    setBusy(false)
  }

  return (
    <div className="grid gap-3 xl:grid-cols-3">
      {/* 链路 */}
      <Card title="链路" right={<Badge tone={link?.connected ? 'mint' : 'slate'}>{link?.mode ?? 'off'}</Badge>}>
        <div className="grid gap-2 text-[13px]">
          <div className="flex items-center justify-between">
            <span className="text-slate-400">状态</span>
            <span className="num text-slate-200">
              {link?.connected ? `${link.port ?? ''} @ ${link.baud ?? 115200}` : '未连接'}
            </span>
          </div>
          <div className="flex items-center justify-between">
            <span className="text-slate-400">遥测帧率</span>
            <span className="num text-slate-200">{fmt(stats?.telem_hz ?? link?.telem_hz, 1)} Hz</span>
          </div>
          <div className="flex items-center justify-between">
            <span className="text-slate-400">最后收包</span>
            <span className="num text-slate-200">{stats?.last_rx_ms !== undefined ? `${fmt(stats.last_rx_ms, 0)} ms 前` : '--'}</span>
          </div>
          <div className="flex items-center justify-between" title="车上控制器每 5 毫秒跑一拍，这里是这 5 毫秒被占掉了多少 —— 不是电脑的负载">
            <span className="text-slate-400">车上控制占用</span>
            <span className="num text-slate-200">{fmt(stats?.busy_pct, 1)} %</span>
          </div>
          <div className="flex items-center justify-between">
            <span className="text-slate-400">固件 / 协议</span>
            <span className="num text-slate-200">
              {link?.fw ? `${link.fw.version} / v${link.fw.proto}` : '--'}
            </span>
          </div>
          <div className="mt-1 flex flex-wrap gap-2">
            <button className="btn-primary" disabled={busy} onClick={() => connect('sim')}>
              <Zap size={14} /> 连模拟器
            </button>
            <button className="btn" disabled={busy} onClick={() => connect('serial', link?.port ?? 'COM26')}>
              <Plug size={14} /> 连串口
            </button>
            <button className="btn-ghost" disabled={busy} onClick={() => store.disconnect()}>
              断开
            </button>
            <button className="btn-ghost" onClick={() => void store.refreshAll()}>
              <RefreshCw size={13} /> 刷新
            </button>
          </div>
          {sse !== 'open' && <div className="text-[12px] text-neon-amber">实时通道未连接，数据不会更新。</div>}
        </div>
      </Card>

      {/* 关键量 */}
      <Card title="关键量" className="xl:col-span-2" right={<span className="num text-[12px] text-slate-500">{fmtClock(t?.t_host)}</span>}>
        <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
          <Stat label="偏航" value={fmt(t?.yaw, 1)} unit="°" tone="cyan" size="lg" />
          <Stat label="俯仰 / 横滚" value={`${fmt(t?.pitch, 1)} / ${fmt(t?.roll, 1)}`} unit="°" />
          <Stat label="左 / 右轮速" value={`${fmt(t?.vl, 0)} / ${fmt(t?.vr, 0)}`} unit="mm/s" />
          <Stat
            label="电池 / 电流"
            value={`${fmt(t ? t.vbat_mv / 1000 : NaN, 2)}`}
            unit={`V · ${fmtInt(t?.ibat_ma)}mA`}
            tone={(t?.vbat_mv ?? 99999) < 10500 ? 'rose' : 'mint'}
            title="V = 伏（电池电压，12V 电池低于 10.5V 就该充了）；mA = 毫安（千分之一安，电流）"
          />
          <Stat label="模式" value={MODE_TEXT[t?.mode ?? 0] ?? '--'} hint={taskText(t?.task_id)} />
          <Stat label="主循环" value={fmtInt(t?.loop_us)} unit="µs" tone={(t?.loop_us ?? 0) > 20000 ? 'amber' : 'slate'} />
          <Stat
            label="PWM 左/右"
            value={`${fmtInt(t?.pwm_l)}/${fmtInt(t?.pwm_r)}`}
            unit={`${fmt(pwmToPct(t?.pwm_l ?? 0), 0)}%`}
            title="PWM = 给电机的占空比刻度（0..7200，越大转得越快；负数是反转）"
          />
          <Stat label="动作 / NVRAM" value={`${t?.action ?? '--'}`} hint={NVRAM_TEXT[t?.nvram ?? 0]} />
        </div>
        <div className="mt-4 grid gap-3 md:grid-cols-3">
          <div>
            <div className="mb-1 text-[12px] text-slate-400">左轮速</div>
            <MiniTrend channel="vl" height={38} />
          </div>
          <div>
            <div className="mb-1 text-[12px] text-slate-400">右轮速</div>
            <MiniTrend channel="vr" height={38} />
          </div>
          <div>
            <div className="mb-1 text-[12px] text-slate-400">偏航</div>
            <MiniTrend channel="yaw" height={38} />
          </div>
        </div>
      </Card>

      {/* 传感器 */}
      <Card title="循迹传感器" className="xl:col-span-2">
        <GrayArray line={t?.line ?? Array(8).fill(0)} gray={t?.gray ?? Array(8).fill(0)} hint />
        <div className="mt-3 grid grid-cols-3 gap-3">
          <Stat label="重心偏差" value={fmt(t?.trace.pos, 2)} />
          <Stat label="循迹 PID 输出" value={fmtInt(t?.trace.pid)} tone="amber" title="PID = 循迹控制器的修正量输出，越大说明它判断车越偏" />
          <Stat label="角度环修正" value={fmtInt(t?.angle.corr)} tone="violet" />
        </div>
      </Card>

      {/* 标志 */}
      <Card title="状态标志">
        <Flags t={t} />
        <div className="mt-3 grid gap-2 text-[12px] text-slate-400">
          <div className="flex justify-between">
            <span>参数来源</span>
            <span className="num text-slate-200">{params?.source ?? '--'}（{params?.items.length ?? 0} 项）</span>
          </div>
          <div className="flex justify-between">
            <span>参数指纹</span>
            <span className="num text-slate-200">0x{(params?.param_crc ?? 0).toString(16).toUpperCase()}</span>
          </div>
          <div className="flex justify-between">
            <span>固件时间</span>
            <span className="num text-slate-200">{fmtUptime(t?.t)}</span>
          </div>
        </div>
        <div className="mt-3 flex gap-2">
          <button className="btn-danger" onClick={() => void store.cmd('stop')}>
            <Power size={14} /> 停车
          </button>
          <button className="btn-ghost" onClick={() => void store.cmd('clear_fault')}>
            清故障
          </button>
        </div>
      </Card>
    </div>
  )
}
