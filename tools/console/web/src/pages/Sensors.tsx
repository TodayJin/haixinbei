import React from 'react'
import { Badge, Card, GrayArray, Stat, Toggle } from '../components/ui'
import { MiniTrend } from '../components/Trend'
import { store, useStore } from '../lib/store'
import { fmt, fmtInt, grayToInk } from '../lib/format'
import type { CalChannel } from '../lib/types'

function CalTable({ ch }: { ch: CalChannel[] }) {
  return (
    <div className="overflow-x-auto">
      <table className="w-full text-left text-[12px]">
        <thead className="text-slate-500">
          <tr>
            <th className="py-1 pr-3">#</th>
            <th className="py-1 pr-3">黑</th>
            <th className="py-1 pr-3">白</th>
            <th className="py-1 pr-3">阈值</th>
            <th className="py-1 pr-3">抖动</th>
            <th className="py-1 pr-3">极性</th>
            <th className="py-1 pr-3">当前</th>
            <th className="py-1">判定</th>
          </tr>
        </thead>
        <tbody className="num">
          {ch.map((c) => (
            <tr key={c.i} className="border-t border-white/5">
              <td className="py-1 pr-3 text-slate-400">{c.i}</td>
              <td className="py-1 pr-3">{c.blk}</td>
              <td className="py-1 pr-3">{c.wht}</td>
              <td className="py-1 pr-3">{c.thr}</td>
              <td className="py-1 pr-3">{c.jitter}</td>
              <td className="py-1 pr-3">{c.pol ? '反' : '正'}</td>
              <td className="py-1 pr-3">{c.level}</td>
              <td className="py-1">
                <Badge tone={c.ok ? 'mint' : 'rose'}>{c.ok ? '正常' : c.reason ? `异常 ${c.reason}` : '异常'}</Badge>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

export function SensorsPage() {
  const { telem, cal, settings } = useStore()
  const t = telem
  const line = t?.line ?? Array(8).fill(0)
  const gray = t?.gray ?? Array(8).fill(0)
  const lit = line.reduce((a, b) => a + (b ? 1 : 0), 0)

  return (
    <div className="grid gap-3 xl:grid-cols-[minmax(0,1fr)_420px]">
      <div className="grid gap-3">
        <Card
          title="灰度阵列"
          right={
            <div className="flex items-center gap-2">
              <Badge tone={t?.flags.lost ? 'rose' : 'mint'}>{t?.flags.lost ? '丢线' : `压线 ${lit} 路`}</Badge>
              <Badge tone="slate">原始值 0..255</Badge>
            </div>
          }
        >
          <GrayArray line={line} gray={gray} height={140} hint />
          <div className="mt-3 grid grid-cols-2 gap-3 md:grid-cols-4">
            <Stat label="重心偏差" value={fmt(t?.trace.pos, 3)} hint="相对 4.5（车正中）" />
            <Stat label="循迹 PID 输出" value={fmtInt(t?.trace.pid)} tone="amber" />
            <Stat label="黑度高" value={fmt(Math.max(...gray.map(grayToInk)) * 255, 0)} />
            <Stat label="路口标志" value={t?.flags.slope ? '有' : '无'} />
          </div>
        </Card>

        <Card title="趋势">
          <div className="grid gap-3 md:grid-cols-2">
            <div>
              <div className="mb-1 text-[12px] text-slate-400">重心偏差</div>
              <MiniTrend channel="trace_pos" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">循迹 PID 输出</div>
              <MiniTrend channel="trace_pid" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">灰度峰值</div>
              <MiniTrend channel="gray_max" height={44} />
            </div>
            <div>
              <div className="mb-1 text-[12px] text-slate-400">主循环耗时</div>
              <MiniTrend channel="loop_us" height={44} />
            </div>
          </div>
        </Card>
      </div>

      <div className="grid gap-3">
        <Card
          title="标定数据"
          right={
            <Badge tone={cal?.quality === 0 ? 'mint' : 'amber'}>
              第 {cal?.active_set ?? 0} 套 · 质量 {cal?.quality ?? '--'}
            </Badge>
          }
        >
          {cal?.ch?.length ? <CalTable ch={cal.ch} /> : <div className="text-[13px] text-slate-500">暂无标定数据（后端没返回 /api/cal）。</div>}
        </Card>
        <Card title="显示设置">
          <Toggle
            on={settings.scopeChannels.includes('gray_max')}
            onChange={(v) =>
              store.patchSettings({
                scopeChannels: v
                  ? [...settings.scopeChannels, 'gray_max']
                  : settings.scopeChannels.filter((k) => k !== 'gray_max'),
              })
            }
            label="把「灰度峰值」加进波形页"
          />
          <p className="mt-2 text-[12px] leading-relaxed text-slate-500">
            屏幕上的灰度是 8 路（车头朝前，左边是 0 号）。压线越多、数组越长，说明线越宽或者车斜着压线。
          </p>
        </Card>
      </div>
    </div>
  )
}
