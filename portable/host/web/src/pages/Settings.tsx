import React from 'react'
import { Download, Plug, RefreshCw, Save, Trash2, Upload } from 'lucide-react'
import { Badge, Card, NumberField, StatusDot, Toggle } from '../components/ui'
import { store, useMeta } from '../lib/store'
import { bytesToHuman, fmt, fmtInt } from '../lib/format'

export function SettingsPage() {
  const { link, settings, sse, stats, ports, presets } = useMeta()
  const [port, setPort] = React.useState(link?.port ?? 'COM26')
  const [baud, setBaud] = React.useState(String(link?.baud ?? 921600))
  const [busy, setBusy] = React.useState(false)
  // 环形缓冲能装多久取决于**实际**遥测帧率（车上固件默认 100Hz，不是老文案里的 50Hz）：
  // 优先用桥数出来的**实测**值，没收到 stats 事件就退回车上报的设定值，再没有按 100 估。
  const measuredHz = stats?.telem_hz ?? 0
  const hzEff = measuredHz > 0 ? measuredHz : (link?.telem_hz ?? 100)

  // 只在"真串口"这条链路上把口名同步进输入框：不然会拿着一个不是串口的字符串去连
  // （出过一次事故，见 bridge/server.py 里 _serial_port_ok 的注释）。
  React.useEffect(() => {
    if (link?.mode === 'serial' && link.port) setPort(link.port)
    if (link?.baud) setBaud(String(link.baud))
  }, [link?.mode, link?.port, link?.baud])

  // 下拉里没有当前这个口时回落到列表第一个：不然会拿着一个这台电脑上根本没有的口名去连
  React.useEffect(() => {
    if (ports.length && !ports.includes(port)) setPort(ports[0])
  }, [ports, port])

  React.useEffect(() => {
    void store.listPorts()
  }, [])

  const doConnect = async (mode: 'sim' | 'serial') => {
    setBusy(true)
    await store.connect(mode, mode === 'serial' ? port : undefined, mode === 'serial' ? Number(baud) : undefined)
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
              <div title="网页和本地桥之间的实时推送通道：已连 = 正常">
                实时推送：{sse === 'open' ? '已连' : sse === 'connecting' ? '连接中' : '断开'}
              </div>
              <div title="车上主循环里那拍界面刷新（app_ui_tick）的耗时占 5ms 控制拍的比例，来自固件遥测 loop_us/5000。它不是控制中断的占用（控制中断实测只有 ~9µs），也不是电脑的负载；随刷屏在 0.2%~81% 之间跳，平均约 17%">
                车上界面刷新：{fmt(stats?.busy_pct, 1)}%
              </div>
              <div title="实测 = 桥按 1s 窗口数出来的帧率；设定 = 车上固件报的目标值（CMD_SET_TELEM_HZ）。两个不一样就说明链路没跑到设定值">
                遥测帧率：<span className="num">{fmt(stats?.telem_hz, 1)}</span>
                <span className="text-slate-500"> / 设定 </span>
                <span className="num">{link?.telem_hz ?? '--'}</span> Hz
              </div>
              <div title="桥从车上收到的帧数 / 往车上发出的帧数">
                收到 / 发出帧：<span className="num">{fmtInt(link?.stats.rx)}/{fmtInt(link?.stats.tx)}</span>
              </div>
              <div title="校验错 = 校验不过的帧数；重新同步 = 丢半截帧后重新对上帧头的次数。正常都该是 0">
                校验错 / 重新同步：<span className="num">{fmtInt(link?.stats.crc)}/{fmtInt(link?.stats.resync)}</span>
              </div>
            </div>
            <p className="text-[12px] leading-relaxed text-slate-500">
              波特率的唯一真值是固件 `firmware/Core/Src/board.c` 里的 `DL_BAUD`（2026-10-05
              起是 921600 8N1）—— 桥的 `--baud`、这个输入框、文档都得跟它一致。车上的第二颗
              探针（`OB-RA4M2-Full`，COM27）VCOM 逐档实测到 3 Mbaud 都通；旧那颗 V9
              （PID_0105）的上限才是 230400，对它会先拦下来（`bridge/serial_safety.py`）。
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
              约 {bytesToHuman(settings.ringPoints * (8 + 18 * 4))}（时间戳 8B + 每通道 4B）。
              {fmt(hzEff, 0)}Hz 下 {fmtInt(settings.ringPoints)} 点 ≈ {fmt(settings.ringPoints / hzEff / 60, 1)} 分钟。
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
            <div className="flex items-center gap-2">
              <Toggle
                on={settings.perfLite}
                onChange={(v) => store.patchSettings({ perfLite: v })}
                label="低特效模式（默认开）"
              />
            </div>
            <div className="text-[12px] leading-relaxed text-slate-500">
              推荐保持开启：面板走不透明分层主题（不糊、不脉冲），滚动与静止都不掉帧。
              关掉会回到玻璃拟态：卡片半透明 + 背景模糊 + 状态点脉冲；滚动时仍会自动临时降特效
              （松手 0.14s 后恢复）。
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
            <div title="车上那张参数表的版本号。网页算出来的和它不一致，说明两边参数不同步">参数指纹 <span className="num text-slate-200">0x{(link?.fw?.param_crc ?? 0).toString(16).toUpperCase()}</span></div>
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

        <Card title="页面上的缩写、数字都是什么意思">
          <dl className="grid gap-x-3 gap-y-1.5 text-[12px] leading-relaxed sm:grid-cols-[auto_1fr]">
            {(
              [
                ['实时推送（SSE）', '网页和本地桥之间的实时数据通道，技术名 Server-Sent Events。它断开只表示网页收不到实时数据，不代表车掉线 —— 车在不在线看顶栏那个口袋的圆点'],
                ['收到 / 丢弃 / 校验错', '顶栏三个数字：收到 = 从车上读到的完整帧数；丢弃 = 半截或认不出来的帧；校验错 = 校验没过的帧。后两个平时都该是 0'],
                ['串口号（COM26）', '当前连的是车上哪个口。模拟器 = 假车，COMx = 真车的串口'],
                ['参数指纹 0xXXXX', '车上那张参数表的版本号。网页算出来的和它不一致，说明两边参数不一样（要在「参数」页固化成一致）'],
                ['车上控制占用 x%', '车上控制器每 5 毫秒跑一拍，这是被占掉的比例；跟电脑快慢无关'],
                ['PWM', '给电机的占空比刻度，0~7200：越大转得越快，负数是反转。它不是转速单位'],
                ['PID', '控制器算出来的修正量（循迹 PID 输出），越大说明它判断车越偏'],
                ['校验和 / CRC16', '一种校验算法（CRC16/CCITT）。烧到车上再读回来，两边这个数一样，就说明字节一个没坏'],
                ['镜长 / 槽位 #0~#7', '一份任务程序叫一份「镜像」，长度就是它占多少字节；车上 Flash 里能放 8 份，编号就是槽位'],
              ] as const
            ).map(([term, desc]) => (
              <React.Fragment key={term}>
                <dt className="whitespace-nowrap text-slate-200">{term}</dt>
                <dd className="text-slate-500">{desc}</dd>
              </React.Fragment>
            ))}
          </dl>
        </Card>
      </div>
    </div>
  )
}
