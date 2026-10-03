import React from 'react'
import { CircleStop, Play, Power, Radio, RefreshCw, RotateCcw, Trash2 } from 'lucide-react'
import { Badge, Card, StatusDot } from '../components/ui'
import { store, useStore } from '../lib/store'
import { fmt, fmtClock } from '../lib/format'
import { MODE_TEXT, NVRAM_TEXT, stateText, taskText } from '../lib/types'

const LOG_TONE: Record<string, string> = {
  info: 'text-slate-300',
  warn: 'text-neon-amber',
  error: 'text-neon-rose',
  tx: 'text-neon-cyan',
  rx: 'text-slate-400',
}

export function ControlPage() {
  const { logs, acks, record, link, stats, params, telem } = useStore()
  const recording = record?.recording ?? false
  const isTaskRunning = telem?.task_id !== undefined && telem.task_id !== 0xff
  const [hz, setHz] = React.useState(50)
  const [filter, setFilter] = React.useState('')

  React.useEffect(() => {
    if (link?.telem_hz) setHz(Math.round(link.telem_hz))
  }, [link?.telem_hz])

  const shown = React.useMemo(() => {
    const needle = filter.trim().toLowerCase()
    return logs.filter((l) => !needle || `${l.level} ${l.text}`.toLowerCase().includes(needle))
  }, [logs, filter])

  return (
    <div className="grid gap-3 xl:grid-cols-[380px_minmax(0,1fr)]">
      <div className="grid gap-3">
        <Card title="任务命令" right={<Badge tone={recording ? 'rose' : 'slate'}>车上记录 {recording ? '开' : '关'}</Badge>}>
          <div className="grid grid-cols-2 gap-2">
            <button className="btn-danger" onClick={() => void store.cmd('stop')}>
              <CircleStop size={13} /> 停止
            </button>
            <a className="btn" href="#/tasks">
              <Play size={13} /> 任务编排
            </a>
          </div>
          <p className="mt-2 text-[12px] text-slate-500">
            车上的内置任务流程（任务 A/B/C）已删除。任务在
            <a className="text-neon-cyan hover:underline" href="#/tasks"> 任务编排页 </a>
            拼装后烧进车的 Flash，启停也在那里；这里的「停止」会停住正在跑的任务。
          </p>
          <div className="mt-3 grid grid-cols-2 gap-2">
            <button className="btn-ghost" onClick={() => void store.cmd('clear_fault')}>
              <RefreshCw size={13} /> 清故障
            </button>
            <button className="btn-ghost" onClick={() => void store.cmd('reset')}>
              <RotateCcw size={13} /> 软复位
            </button>
            <button className="btn-ghost" onClick={() => void store.toggleRecord()}>
              <Radio size={13} /> {recording ? '停止记录' : '开始记录'}
            </button>
          </div>
        </Card>

        <Card title="遥测频率">
          <div className="flex items-center gap-2">
            <input
              type="range"
              min={1}
              max={100}
              value={hz}
              onChange={(e) => setHz(Number(e.target.value))}
              className="h-1.5 w-full cursor-pointer appearance-none rounded-full bg-white/10 accent-neon-cyan"
            />
            <span className="num w-16 text-right text-[13px] text-slate-200">{hz} Hz</span>
          </div>
          <button className="btn mt-2" onClick={() => void store.cmd('telem_hz', hz)}>
            下发频率
          </button>
          <div className="mt-3 grid grid-cols-2 gap-3">
            <div className="text-[12px] text-slate-400">
              实测帧率 <span className="num text-slate-200">{fmt(stats?.telem_hz ?? link?.telem_hz, 1)} Hz</span>
            </div>
            <div className="text-[12px] text-slate-400" title="半截或认不出来的帧数，正常是 0">
              丢帧 <span className="num text-slate-200">{fmt(stats?.stats.drops ?? link?.stats.drops, 0)}</span>
            </div>
            <div className="text-[12px] text-slate-400" title="校验不过的帧数，正常是 0">
              校验错 <span className="num text-slate-200">{fmt(stats?.stats.crc ?? link?.stats.crc, 0)}</span>
            </div>
            <div className="text-[12px] text-slate-400" title="车上参数表的版本号，两边不一致说明参数没同步">
              参数指纹 <span className="num text-slate-200">0x{(params?.param_crc ?? 0).toString(16).toUpperCase()}</span>
            </div>
          </div>
        </Card>

        <Card title="车上记录">
          <div className="text-[13px] text-slate-300">
            状态：{recording ? '记录中' : '未记录'}
            {record?.samples !== undefined && <span className="num ml-2 text-slate-400">{record.samples} 帧</span>}
          </div>
          <p className="mt-2 text-[12px] text-slate-500">
            「车上记录」是把黑匣子写进车自己的 Flash；上位机的 CSV 是另一套（在波形页导出），两回事。
          </p>
        </Card>

        <Card title="车辆状态">
          <div className="grid grid-cols-2 gap-3 text-[12px] text-slate-400">
            <div>
              模式 <Badge tone="cyan">{MODE_TEXT[telem?.mode ?? 0] ?? '--'}</Badge>
            </div>
            <div>
              当前任务 <Badge tone={isTaskRunning ? 'mint' : 'slate'}>{taskText(telem?.task_id)}</Badge>
            </div>
            <div>
              解释器状态 <span className="num text-slate-200">{stateText(telem?.state_id)}</span>
            </div>
            <div>
              动作 <span className="num text-slate-200">{telem?.action ?? '--'}</span>
            </div>
            <div className="col-span-2">
              NVRAM <Badge tone={(telem?.nvram ?? 0) === 3 ? 'mint' : (telem?.nvram ?? 0) === 4 ? 'rose' : 'slate'}>{NVRAM_TEXT[telem?.nvram ?? 0] ?? '--'}</Badge>
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card
          title={`日志（${logs.length}）`}
          right={
            <div className="flex items-center gap-2">
              <input className="field w-40" placeholder="过滤…" value={filter} onChange={(e) => setFilter(e.target.value)} />
              <button className="btn-ghost" onClick={() => store.clearLogs()}>
                <Trash2 size={13} /> 清空
              </button>
            </div>
          }
        >
          <div className="max-h-[360px] overflow-y-auto rounded-lg bg-ink-900/60 p-2 font-mono text-[11.5px] leading-relaxed">
            {shown.length === 0 && <div className="text-slate-500">暂无日志。</div>}
            {shown.map((l, i) => (
              <div key={`${l.ts_ms}-${i}`} className="flex gap-2">
                <span className="shrink-0 text-slate-600">{fmtClock(l.ts_ms)}</span>
                <span className={`shrink-0 ${LOG_TONE[l.level] ?? 'text-slate-300'}`}>[{l.level}]</span>
                <span className={LOG_TONE[l.level] ?? 'text-slate-300'}>{l.text}</span>
              </div>
            ))}
          </div>
        </Card>

        <Card title={`ACK（${acks.length}）`}>
          <div className="max-h-[220px] overflow-y-auto rounded-lg bg-ink-900/60 p-2 font-mono text-[11.5px]">
            {acks.length === 0 && <div className="text-slate-500">还没有 ACK。</div>}
            {acks.map((a) => (
              <div key={a.seq} className="flex gap-2">
                <StatusDot on={a.ok} tone={a.ok ? 'mint' : 'rose'} />
                <span className="text-slate-500">#{a.seq}</span>
                <span className={a.ok ? 'text-slate-300' : 'text-neon-rose'}>
                  {a.req} {a.ok ? 'OK' : `status ${a.status} ${a.text}`}
                </span>
              </div>
            ))}
          </div>
        </Card>

        <Card title="本机出口（桥 → 车）">
          <p className="text-[12px] leading-relaxed text-slate-400">
            桥在真车模式下每 0.5s 给车发一次心跳；车里那条看门狗一旦 0.5s 收不到心跳就自动停车。断网/拔线时车轮会自己停，
            不需要你手忙脚乱去按急停。
          </p>
          <button className="btn mt-2" onClick={() => void store.refreshAll()}>
            <Power size={13} /> 重新握手并刷新全部状态
          </button>
        </Card>
      </div>
    </div>
  )
}
