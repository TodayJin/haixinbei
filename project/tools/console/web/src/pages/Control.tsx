import React from 'react'
import { CircleStop, Play, Power, RefreshCw, RotateCcw, Trash2 } from 'lucide-react'
import { Badge, Card, StatusDot } from '../components/ui'
import { store, useMeta, useStore } from '../lib/store'
import { fmt, fmtClock } from '../lib/format'
import { MODE_TEXT, NVRAM_TEXT, stateText, taskText } from '../lib/types'

const LOG_TONE: Record<string, string> = {
  info: 'text-slate-300',
  warn: 'text-neon-amber',
  error: 'text-neon-rose',
  tx: 'text-neon-cyan',
  rx: 'text-slate-400',
}

/**
 * 日志 + ACK 两块单独抽出来：它们只吃 `useMeta()`（低频），
 * 于是车上 100Hz 的遥测不会拖着几百条日志一起重渲染。
 */
function LogCards() {
  const { logs, acks } = useMeta()
  const [filter, setFilter] = React.useState('')

  const shown = React.useMemo(() => {
    const needle = filter.trim().toLowerCase()
    return logs.filter((l) => !needle || `${l.level} ${l.text}`.toLowerCase().includes(needle))
  }, [logs, filter])

  return (
    <>
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
    </>
  )
}

/** 车上固件把遥测频率钳在 1..200Hz（`firmware/App/Svc/host_link.c` 的
 *  `CMD_SET_TELEM_HZ`：`arg<1 → 1`、`arg>200 → 200`），所以滑块上限就是 200 ——
 *  100Hz 只是**默认值**，不是上限（链路提速后就是留给用户往上调的）。 */
const HZ_MIN = 1
const HZ_MAX = 200

export function ControlPage() {
  const { link, stats, params, telem } = useStore()
  const isTaskRunning = telem?.task_id !== undefined && telem.task_id !== 0xff
  // 初值取车上固件的默认值（`host_link.c:1087 s_stats.telem_hz = 200u`）：以前写 50，后来 100，
  // 没连车时显示的数字跟车上打架。连上后由 link.telem_hz（车上报的**设定值**）覆盖。
  const [hz, setHz] = React.useState(200)

  React.useEffect(() => {
    if (link?.telem_hz) {
      setHz(Math.min(HZ_MAX, Math.max(HZ_MIN, Math.round(link.telem_hz))))
    }
  }, [link?.telem_hz])

  return (
    <div className="grid gap-3 xl:grid-cols-[380px_minmax(0,1fr)]">
      <div className="grid gap-3">
        <Card title="任务命令">
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
          </div>
        </Card>

        <Card title="遥测频率">
          <div className="flex items-center gap-2">
            <input
              type="range"
              min={HZ_MIN}
              max={HZ_MAX}
              value={hz}
              onChange={(e) => setHz(Number(e.target.value))}
              className="h-1.5 w-full cursor-pointer appearance-none rounded-full bg-white/10 accent-neon-cyan"
            />
            <span className="num w-16 text-right text-[13px] text-slate-200">{hz} Hz</span>
          </div>
          <button className="btn mt-2" onClick={() => void store.cmd('set_telem_hz', hz)}>
            下发频率
          </button>
          <div className="mt-2 text-[12px] text-slate-400">
            车上钳在 {HZ_MIN}–{HZ_MAX}Hz（默认 100）。调高后看右边的
            <span className="text-slate-300"> 实测帧率 </span>
            有没有跟上设定值；再盯
            <span className="text-slate-300"> 丢帧 / 校验错 </span>
            不为 0 就说明链路吃不消，降回来。
          </div>
          <div className="mt-3 grid grid-cols-2 gap-3">
            <div className="text-[12px] text-slate-400">
              实测帧率 <span className="num text-slate-200">{fmt(stats?.telem_hz ?? null, 1)} Hz</span>
              <span className="text-slate-500"> / 设定 {link?.telem_hz ?? '--'} Hz</span>
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
              {/* ★审计 #11：固件只有 3 态（0 空闲 / 1 正在写 / 2 已保存，以 nvram.h 为准）
     ⇒ 判绿要判 **2**（原来判 3/4 是 5 态的旧口径，永远不可能变绿） */}
              NVRAM <Badge tone={(telem?.nvram ?? 0) === 2 ? 'mint' : 'slate'}>{NVRAM_TEXT[telem?.nvram ?? 0] ?? '--'}</Badge>
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <LogCards />

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
