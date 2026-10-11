import React from 'react'
import { Download, Play, RotateCcw, Save, Upload } from 'lucide-react'
import { Badge, Card, NumberField, Slider, Stat, StatusDot } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { store, useStore } from '../lib/store'
import { fmt, fmtInt } from '../lib/format'
import { NVRAM_TEXT, type ActionItem } from '../lib/types'

function ServoCard({ ch }: { ch: 1 | 2 }) {
  const { telem } = useStore()
  const [angle, setAngle] = React.useState(0)
  const [dev, setDev] = React.useState(0)
  const us = ch === 1 ? telem?.servo1_us : telem?.servo2_us
  const deg = ch === 1 ? telem?.servo1_deg : telem?.servo2_deg

  return (
    <Card
      title={`舵机 ${ch}`}
      right={
        <Badge tone="slate">
          <StatusDot on={!!telem} tone="mint" />
          {fmt(us, 0)} µs · {fmt(deg, 1)}°
        </Badge>
      }
    >
      <div className="mb-2 flex items-end justify-between">
        <Stat label="目标角度" value={fmt(angle, 0)} unit="°" size="lg" tone="cyan" />
        <div className="flex items-center gap-2">
          <NumberField value={angle} onChange={setAngle} min={0} max={180} step={1} decimals={0} unit="°" />
          <button className="btn" onClick={() => void store.setServo(ch, angle)}>
            下发
          </button>
        </div>
      </div>
      <Slider value={angle} min={0} max={180} step={1} onChange={(v) => { setAngle(v); void store.setServo(ch, v) }} />
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <button className="btn-ghost" onClick={() => { setAngle(90); void store.setServo(ch, 90) }}>90°</button>
        <button className="btn-ghost" onClick={() => { setAngle(0); void store.setServo(ch, 0) }}>0°（收回）</button>
        <button className="btn-ghost" onClick={() => void store.centerServo()}>两路回中</button>
        <div className="ml-auto flex items-center gap-1">
          <span className="text-[11px] text-slate-500">中位偏差</span>
          <NumberField value={dev} onChange={setDev} min={-100} max={100} step={1} unit="µs" />
          <button className="btn-ghost" onClick={() => void store.setDeviation(ch, dev)}>应用</button>
        </div>
      </div>
      <div className="mt-3">
        <div className="mb-1 text-[12px] text-slate-400">角度趋势</div>
        <MiniTrend channel={ch === 1 ? 'servo1' : 'servo2'} height={36} />
      </div>
    </Card>
  )
}

function ActionRow({ a }: { a: ActionItem }) {
  const [a1, setA1] = React.useState(a.a1)
  const [a2, setA2] = React.useState(a.a2)
  const { telem } = useStore()
  const active = telem?.action === a.n

  React.useEffect(() => {
    setA1(a.a1)
    setA2(a.a2)
  }, [a.a1, a.a2])

  return (
    <tr className={`border-t border-white/5 ${active ? 'bg-neon-cyan/5' : ''}`}>
      <td className="py-1 pr-2">
        <span className={`num inline-block w-6 text-right ${active ? 'text-neon-cyan' : 'text-slate-400'}`}>{a.n}</span>
      </td>
      <td className="py-1 pr-2">
        <span className="text-slate-200">{a.name}</span>
        {a.compound && <Badge tone="amber">复合</Badge>}
        {a.dirty && <Badge tone="rose">未保存</Badge>}
      </td>
      {/* ★改完**立刻下发到车的 RAM**（用户 2026-10-10 报的 bug："改了这个动作，
          保存之后就又回到原值了"）。原因：这两个 NumberField 原来只写本地 state，
          必须再手按「下发」才到车；而「固化到车」固化的是**车里的值**，结尾又会
          refreshActions() 从车重读 ⇒ 没按过「下发」的话，界面**弹回原值**。
          NumberField 是**失焦时 commit**（不是每敲一个键），所以这里不会刷屏。 */}
      <td className="py-1 pr-2">
        <NumberField
          value={a1}
          onChange={(v) => {
            setA1(v)
            void store.setAction(a.n, v, a2)
          }}
          min={0}
          max={180}
          step={5}
          decimals={0}
        />
      </td>
      <td className="py-1 pr-2">
        <NumberField
          value={a2}
          onChange={(v) => {
            setA2(v)
            void store.setAction(a.n, a1, v)
          }}
          min={0}
          max={180}
          step={5}
          decimals={0}
        />
      </td>
      <td className="py-1 pr-2 text-right text-[11px] text-slate-500 num">
        {a.preset_a1}/{a.preset_a2}
      </td>
      <td className="py-1">
        <div className="flex flex-wrap justify-end gap-1">
          <button
            className="btn-ghost !px-2 !py-0.5 text-[11px]"
            onClick={() => void store.setAction(a.n, a1, a2)}
            title="把这一行的角度重新下发到车的 RAM（改完数字失焦就会自动下发，这个是重发/兜底）"
          >
            下发
          </button>
          <button
            className="btn-ghost !px-2 !py-0.5 text-[11px]"
            onClick={async () => {
              await store.setAction(a.n, a1, a2)
              await store.playAction(a.n)
            }}
            title="下发后完整播放一次（复合动作会走中间步骤）"
          >
            <Play size={11} /> 播放
          </button>
          {a.compound && (
            <span className="self-center text-[10px] text-slate-500" title="复合动作的中间步骤由固件写死，这里只改起止角">
              中间步骤固定
            </span>
          )}
        </div>
      </td>
    </tr>
  )
}

export function ServoPage() {
  const { actions, presets, telem } = useStore()
  const [presetName, setPresetName] = React.useState('')
  const items = actions?.items ?? []
  const dirty = items.filter((i) => i.dirty).length

  return (
    <div className="grid gap-3 xl:grid-cols-2">
      <div className="grid gap-3">
        <ServoCard ch={1} />
        <ServoCard ch={2} />
        <Card title="当前状态">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="正在执行动作" value={`${telem?.action ?? '--'}`} tone="cyan" />
            <Stat label="1 号脉宽" value={fmtInt(telem?.servo1_us)} unit="µs" />
            <Stat label="2 号脉宽" value={fmtInt(telem?.servo2_us)} unit="µs" />
            <Stat label="动作表来源" value={actions?.ok ? '固件' : '本地'} tone={actions?.ok ? 'mint' : 'amber'} />
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card
          title="动作表（20 个动作）"
          right={
            <div className="flex items-center gap-1">
              {dirty > 0 && <Badge tone="rose">{dirty} 项改了</Badge>}
              {/* ★把"车到底写了没有"摆出来（用户 2026-10-10："舵机页面保存不了"）。
                  固件对 T_ACTION_SAVE **一律回 ACK OK**，真正写不写由 nvram_poll() 决定：
                    · 「车在动绝不擦写」—— 速度环使能 / 角度环开着 / 运动模式非 IDLE 就**拒**；
                    · 内容和 Flash 里那份已经一样时也什么都不做（照样回 OK）。
                  ⇒ **光看 ACK 判断不了成没成**。取值来自遥测（TELEM2 第 56 字节）：
                    0 空闲 / 1 待保存 / 2 保存中 / 3 已保存 / 4 失败。
                  和「操控页」「总览页」用的是同一个 NVRAM_TEXT、同一个来源，口径一致。 */}
              <Badge
                tone={(telem?.nvram ?? 0) === 3 ? 'mint' : (telem?.nvram ?? 0) === 4 ? 'rose' : 'slate'}
                title={
                  (telem?.nvram ?? 0) === 1
                    ? '已收到请求，**还没写进 Flash**（车在动时固件会一直拒；先停车/等电机停）'
                    : (telem?.nvram ?? 0) === 3
                      ? '已经写进车的 Flash 了（掉电不丢）'
                      : (telem?.nvram ?? 0) === 4
                        ? '写 Flash 失败了'
                        : '车的 NVRAM 保存状态'
                }
              >
                NVRAM {NVRAM_TEXT[telem?.nvram ?? 0] ?? '--'}
              </Badge>
              <button
                className="btn-ghost"
                onClick={() => void store.saveActions()}
                title="让车把整张动作表写进自己的 Flash（掉电不丢）。★固件只在车不动时才真写 —— 按完看左边的 NVRAM 徽章：走到「已保存」才算成"
              >
                <Save size={13} /> 固化到车
              </button>
            </div>
          }
        >
          <div className="overflow-x-auto">
            <table className="w-full text-left text-[12px]">
              <thead className="text-slate-500">
                <tr>
                  <th className="py-1 pr-2">号</th>
                  <th className="py-1 pr-2">动作</th>
                  <th className="py-1 pr-2">舵机1</th>
                  <th className="py-1 pr-2">舵机2</th>
                  <th className="py-1 pr-2 text-right">出厂</th>
                  <th className="py-1 text-right">操作</th>
                </tr>
              </thead>
              <tbody>
                {items.map((a) => (
                  <ActionRow key={a.n} a={a} />
                ))}
              </tbody>
            </table>
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            <button className="btn" onClick={() => void store.refreshActions()}>
              <Upload size={13} /> 从车重读动作表
            </button>
            <button className="btn-ghost" onClick={() => void store.refreshParams()}>
              从车重读参数
            </button>
            <button
              className="btn-ghost"
              onClick={() => void store.paramsDefaults()}
              title="把**参数表**的默认值拉回来（不含动作表）—— 参数的事在「参数」页；这里留着是老界面的遗留入口"
            >
              <RotateCcw size={13} /> 参数回默认
            </button>
          </div>
        </Card>

        <Card title="预设（存在这台电脑上）">
          <div className="flex flex-wrap items-center gap-2">
            <input
              className="field w-40"
              placeholder="预设名，如 高台-能过"
              value={presetName}
              onChange={(e) => setPresetName(e.target.value)}
            />
            <button
              className="btn-primary"
              disabled={!presetName.trim()}
              onClick={() => void store.savePreset(presetName.trim(), items.map((i) => `action.${i.n}`))}
            >
              <Download size={13} /> 存当前动作表
            </button>
          </div>
          <div className="mt-3 grid gap-2">
            {presets.length === 0 && <div className="text-[12px] text-slate-500">还没有预设。</div>}
            {presets.map((p) => (
              <div key={p.name} className="flex items-center gap-2 rounded-lg bg-white/5 px-2 py-1.5">
                <span className="text-[13px] text-slate-200">{p.name}</span>
                <Badge tone="slate">{p.kind}</Badge>
                <span className="truncate text-[11px] text-slate-500">{p.keys.join(', ')}</span>
                <div className="ml-auto flex gap-1">
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.applyPreset(p.name, ['actions', 'params', 'servo'])}>
                    应用到车
                  </button>
                  <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void store.deletePreset(p.name)}>
                    删
                  </button>
                </div>
              </div>
            ))}
          </div>
        </Card>
      </div>
    </div>
  )
}
