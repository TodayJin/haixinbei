import React from 'react'
import { Download, Plug, RefreshCw, Save, Trash2, Upload } from 'lucide-react'
import { Badge, Card, NumberField, StatusDot, Toggle } from '../components/ui'
import { store, useStore } from '../lib/store'
import { bytesToHuman, fmt, fmtInt } from '../lib/format'

export function SettingsPage() {
  const { link, settings, sse, stats, ports, presets, recordings } = useStore()
  const [port, setPort] = React.useState(link?.port ?? 'COM26')
  const [baud, setBaud] = React.useState(String(link?.baud ?? 115200))
  const [busy, setBusy] = React.useState(false)
  const [rec, setRec] = React.useState('')
  const [speed, setSpeed] = React.useState(1)

  React.useEffect(() => {
    if (link?.port) setPort(link.port)
    if (link?.baud) setBaud(String(link.baud))
  }, [link?.port, link?.baud])

  React.useEffect(() => {
    void store.listPorts()
    void store.listRecordings()
  }, [])

  // 列表到手就默认选中最新那个（列表本身是按 mtime 倒序的）
  React.useEffect(() => {
    if (!rec && recordings.length) setRec(recordings[0].path)
  }, [recordings, rec])

  const doConnect = async (mode: 'sim' | 'serial' | 'replay') => {
    setBusy(true)
    if (mode === 'replay') {
      await store.connect('replay', undefined, undefined, rec, speed)
    } else {
      await store.connect(mode, mode === 'serial' ? port : undefined, mode === 'serial' ? Number(baud) : undefined)
    }
    setBusy(false)
  }

  return (
    <div className="grid gap-3 xl:grid-cols-2">
      <div className="grid gap-3">
        <Card
          title="连接"
          right={
            <Badge tone={link?.connected ? 'mint' : 'slate'}>
              <StatusDot on={!!link?.connected} tone="mint" />
              {link?.mode ?? 'off'}
            </Badge>
          }
        >
          <div className="grid gap-3">
            <div className="flex flex-wrap items-end gap-2">
              <div>
                <div className="mb-1 text-[12px] text-slate-400">串口</div>
                <select className="field w-40" value={port} onChange={(e) => setPort(e.target.value)}>
                  {(ports.length ? ports : ['COM26']).map((p) => (
                    <option key={p} value={p}>
                      {p}
                    </option>
                  ))}
                </select>
              </div>
              <div>
                <div className="mb-1 text-[12px] text-slate-400">波特率</div>
                <input className="field w-28" value={baud} onChange={(e) => setBaud(e.target.value)} />
              </div>
              <button className="btn-ghost" onClick={() => void store.listPorts()}>
                <RefreshCw size={13} /> 刷新端口
              </button>
            </div>
            <div className="flex flex-wrap gap-2">
              <button className="btn-primary" disabled={busy} onClick={() => doConnect('sim')}>
                连模拟器
              </button>
              <button className="btn" disabled={busy} onClick={() => doConnect('serial')}>
                <Plug size={13} /> 连串口
              </button>
              <button className="btn-ghost" disabled={busy} onClick={() => void store.disconnect()}>
                断开
              </button>
            </div>
            <div className="grid grid-cols-2 gap-2 text-[12px] text-slate-400">
              <div>SSE：{sse}</div>
              <div>busy：{fmt(stats?.busy_pct, 1)}%</div>
              <div>
                rx/tx：<span className="num">{fmtInt(link?.stats.rx)}/{fmtInt(link?.stats.tx)}</span>
              </div>
              <div>
                crc/resync：<span className="num">{fmtInt(link?.stats.crc)}/{fmtInt(link?.stats.resync)}</span>
              </div>
            </div>
            <p className="text-[12px] leading-relaxed text-slate-500">
              真车要连的是无线下载器那个虚拟串口（默认 115200 8N1）。桥只监听 127.0.0.1，不对外开放。
            </p>
          </div>
        </Card>

        <Card
          title="回放录制"
          right={
            <Badge tone={link?.mode === 'replay' ? 'mint' : 'slate'}>
              <StatusDot on={link?.mode === 'replay'} tone="mint" />
              {link?.mode === 'replay' ? '回放中' : '未使用'}
            </Badge>
          }
        >
          <div className="grid gap-3">
            <div className="flex flex-wrap items-end gap-2">
              <div>
                <div className="mb-1 text-[12px] text-slate-400">录制文件</div>
                <select className="field w-64" value={rec} onChange={(e) => setRec(e.target.value)}>
                  {recordings.length === 0 && <option value="">（data/logs 里还没有 jsonl）</option>}
                  {recordings.map((r) => (
                    <option key={r.path} value={r.path}>
                      {r.name}（{bytesToHuman(r.size)}）
                    </option>
                  ))}
                </select>
              </div>
              <div>
                <div className="mb-1 text-[12px] text-slate-400">倍速</div>
                <NumberField value={speed} min={0.25} max={8} step={0.25} decimals={2} onChange={setSpeed} unit="×" />
              </div>
              <button className="btn-ghost" onClick={() => void store.listRecordings()}>
                <RefreshCw size={13} /> 刷新列表
              </button>
              <button className="btn" disabled={busy || !rec || link?.mode === 'replay'} onClick={() => void doConnect('replay')}>
                播放
              </button>
            </div>
            <p className="text-[12px] leading-relaxed text-slate-500">
              回放把「控制」页点「开始记录」时录下来的遥测（<span className="num">data/logs/rec_*.jsonl</span>
              ）按原时间戳重新喂一遍：波形、仪表、日志都照常走，只是数据来自文件、循环播放。想回到实时就点上面的「连模拟器」或「连串口」。
            </p>
          </div>
        </Card>

        <Card title="缓冲与刷新">
          <div className="grid gap-3">
            <div className="flex items-center justify-between">
              <span className="text-[13px] text-slate-300">环形缓冲点数</span>
              <NumberField
                value={settings.ringPoints}
                min={1000}
                max={300000}
                step={1000}
                onChange={(v) => store.patchSettings({ ringPoints: Math.round(v) })}
                unit="点"
              />
            </div>
            <div className="text-[12px] text-slate-500">
              约 {bytesToHuman(settings.ringPoints * (8 + 18 * 4))}（时间戳 8B + 每通道 4B）。50Hz 下 30000 点 ≈ 10 分钟。
            </div>
            <div className="flex items-center justify-between">
              <span className="text-[13px] text-slate-300">波形窗口</span>
              <div className="seg">
                {[5, 10, 20, 30].map((w) => (
                  <button
                    key={w}
                    className={`seg-item ${settings.windowSec === w ? 'seg-on' : ''}`}
                    onClick={() => store.patchSettings({ windowSec: w as 5 | 10 | 20 | 30 })}
                  >
                    {w}s
                  </button>
                ))}
              </div>
            </div>
            <div className="flex items-center gap-2">
              <Toggle on={settings.safetyBanner} onChange={(v) => store.patchSettings({ safetyBanner: v })} label="操控页显示安全提示" />
            </div>
            <div className="flex gap-2">
              <button className="btn-ghost" onClick={() => store.clearBuffers()}>
                <Trash2 size={13} /> 清空缓冲
              </button>
              <button className="btn-ghost" onClick={() => store.clearLogs()}>
                清空日志
              </button>
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card title="固件与协议">
          <div className="grid grid-cols-2 gap-3 text-[12px] text-slate-400">
            <div>固件名 <span className="num text-slate-200">{link?.fw?.name ?? '--'}</span></div>
            <div>版本 <span className="num text-slate-200">{link?.fw?.version ?? '--'}</span></div>
            <div>协议 <span className="num text-slate-200">v{link?.fw?.proto ?? '--'}</span></div>
            <div>参数指纹 <span className="num text-slate-200">0x{(link?.fw?.param_crc ?? 0).toString(16).toUpperCase()}</span></div>
          </div>
          <div className="mt-3 flex flex-wrap gap-1.5">
            {[['遥测2', 0], ['动作表', 1], ['舵机', 2], ['驾驶', 3], ['标定', 4], ['Flash 保存', 5], ['看门狗', 6], ['任务程序', 7]].map(([label, bit]) => {
              const on = (((link?.fw?.caps ?? 0) >> Number(bit)) & 1) === 1
              return (
                <Badge key={String(label)} tone={on ? 'mint' : 'slate'}>
                  <StatusDot on={on} tone={on ? 'mint' : 'slate'} />
                  {label}
                </Badge>
              )
            })}
          </div>
          <p className="mt-2 text-[12px] text-slate-500">
            能力位是固件自己声明的：没声明的功能，界面照样能点（桥会照发），失败时会把车的原话显示出来。
          </p>
        </Card>

        <Card title="预设与备份">
          <div className="flex flex-wrap gap-2">
            <button className="btn" onClick={() => void store.refreshPresets()}>
              <RefreshCw size={13} /> 刷新预设
            </button>
            <button
              className="btn-ghost"
              onClick={async () => {
                const blob = JSON.stringify(presets, null, 2)
                const url = URL.createObjectURL(new Blob([blob], { type: 'application/json' }))
                const a = document.createElement('a')
                a.href = url
                a.download = 'console-presets.json'
                a.click()
                URL.revokeObjectURL(url)
              }}
            >
              <Download size={13} /> 导出预设 JSON
            </button>
          </div>
          <div className="mt-3 grid gap-2">
            {presets.map((p) => (
              <div key={p.name} className="flex items-center gap-2 rounded-lg bg-white/5 px-2 py-1.5 text-[12px]">
                <span className="text-slate-200">{p.name}</span>
                <Badge tone="slate">{p.kind}</Badge>
                <span className="truncate text-slate-500">{p.keys.length} 项</span>
                <div className="ml-auto flex gap-1">
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.applyPreset(p.name, ['actions', 'params', 'servo'])}>
                    <Upload size={11} /> 下发
                  </button>
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.deletePreset(p.name)}>
                    删
                  </button>
                </div>
              </div>
            ))}
            {presets.length === 0 && <div className="text-[12px] text-slate-500">还没有预设。</div>}
          </div>
        </Card>

        <Card title="关于">
          <ul className="list-disc space-y-1 pl-4 text-[12px] leading-relaxed text-slate-400">
            <li>这是「本地 Web 控制台」：前端由 Python 桥（纯标准库 + pyserial）托管，浏览器只连 127.0.0.1。</li>
            <li>所有写操作都走后端；失败时把车的原话（error 字段）原样显示在右下角。</li>
            <li>比赛规则不允许任何形式的遥控干预，这套只用于调试、标定与赛前检查。</li>
          </ul>
        </Card>
      </div>
    </div>
  )
}
