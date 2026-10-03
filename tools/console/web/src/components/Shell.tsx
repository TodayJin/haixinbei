import React from 'react'
import {
  Activity,
  Cable,
  Cpu,
  Gamepad2,
  Gauge,
  LineChart,
  Radio,
  Settings as SettingsIcon,
  SlidersHorizontal,
  Wrench,
  type LucideIcon,
} from 'lucide-react'
import { PAGES, navigateTo, usePage, type PageId } from '../lib/router'
import { store, useStore } from '../lib/store'
import { Badge, StatusDot } from './ui'
import { ErrorBanner } from './Toasts'
import { PageBoundary } from './PageBoundary'
import { fmt } from '../lib/format'
import { MODE_TEXT, RENWU_TEXT } from '../lib/types'

const ICONS: Record<PageId, LucideIcon> = {
  overview: Gauge,
  drive: Gamepad2,
  imu: Cpu,
  sensors: Radio,
  servo: Wrench,
  params: SlidersHorizontal,
  scope: LineChart,
  control: Activity,
  settings: SettingsIcon,
}

function LinkBadge() {
  const { link, sse, telem } = useStore()
  const mode = link?.mode ?? 'off'
  const connected = !!link?.connected
  const tone = connected ? 'mint' : sse === 'open' ? 'slate' : 'rose'
  const text =
    mode === 'sim'
      ? '模拟器'
      : mode === 'replay'
        ? '回放'
        : mode === 'serial'
          ? link?.port ?? '串口'
          : '未连接'
  return (
    <div className="flex items-center gap-3">
      <Badge tone={tone}>
        <StatusDot on={connected} tone={connected ? 'mint' : sse === 'open' ? 'slate' : 'rose'} pulse={connected} />
        {text}
      </Badge>
      {link && (
        <span className="num text-[12px] text-slate-400">
          rx {fmt(link.stats.rx, 0)} / drop {fmt(link.stats.drops, 0)} / crc {fmt(link.stats.crc, 0)}
        </span>
      )}
      <span className="num text-[12px] text-slate-400">
        {telem ? `${fmt(telem.mode >= 0 ? telem.mode : 0, 0)} ${MODE_TEXT[telem.mode] ?? ''}` : ''}
        {telem?.renwu ? ` · ${RENWU_TEXT[telem.renwu] ?? ''}` : ''}
      </span>
      <Badge tone={sse === 'open' ? 'cyan' : sse === 'connecting' ? 'amber' : 'rose'}>
        SSE {sse === 'open' ? '已连' : sse === 'connecting' ? '连接中' : '断开'}
      </Badge>
    </div>
  )
}

export function Shell({ children }: { children: React.ReactNode }) {
  const page = usePage()
  const { settings, telem, sse, params } = useStore()
  const [collapsed, setCollapsed] = React.useState(false)

  React.useEffect(() => {
    store.start()
    void store.refreshAll()
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') store.emergencyStop()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  const cur = PAGES.find((p) => p.id === page)
  const dirtyParams = params?.items.filter((i) => i.dirty).length ?? 0

  return (
    <div className="flex min-h-screen">
      {/* 侧栏 */}
      <aside
        className={`${collapsed ? 'w-[68px]' : 'w-[228px]'} sticky top-0 z-30 flex h-screen shrink-0 flex-col gap-1 overflow-y-auto border-r border-white/10 bg-ink-800/70 p-3 backdrop-blur-xl transition-[width] duration-150`}
      >
        <div className="mb-2 flex items-center gap-2 px-1">
          <div className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-gradient-to-br from-neon-cyan/30 to-neon-violet/30 ring-1 ring-white/15">
            <Cable size={18} className="text-neon-cyan" />
          </div>
          {!collapsed && (
            <div className="leading-tight">
              <div className="text-[13px] font-semibold text-slate-100">全地形机器人</div>
              <div className="text-[11px] text-slate-500">控制台 v1</div>
            </div>
          )}
        </div>

        {PAGES.map((p) => {
          const Icon = ICONS[p.id]
          const active = p.id === page
          return (
            <button
              key={p.id}
              onClick={() => navigateTo(p.id)}
              title={p.desc}
              className={`group flex items-center gap-3 rounded-xl px-3 py-2 text-left text-[13px] transition-colors duration-150 ${
                active
                  ? 'bg-gradient-to-r from-neon-cyan/20 to-transparent text-neon-cyan ring-1 ring-neon-cyan/25'
                  : 'text-slate-400 hover:bg-white/5 hover:text-slate-200'
              }`}
            >
              <Icon size={17} className="shrink-0" />
              {!collapsed && (
                <span className="flex-1 truncate">
                  {p.title}
                  {p.id === 'params' && dirtyParams > 0 && (
                    <span className="ml-1 text-[11px] text-neon-amber">{dirtyParams}*</span>
                  )}
                </span>
              )}
            </button>
          )
        })}

        <div className="mt-auto space-y-2 pt-3">
          {!collapsed && (
            <button className="btn-ghost w-full" onClick={() => store.emergencyStop()}>
              急停（Esc）
            </button>
          )}
          <button className="btn-ghost w-full" onClick={() => setCollapsed((v) => !v)}>
            {collapsed ? '»' : '« 收起'}
          </button>
        </div>
      </aside>

      {/* 主区 */}
      <main className="min-w-0 flex-1">
        <header className="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 border-b border-white/10 bg-ink-900/70 px-5 py-3 backdrop-blur-xl">
          <div className="min-w-0">
            <h1 className="text-[17px] font-semibold text-slate-100">{cur?.title ?? '总览'}</h1>
            <p className="truncate text-[12px] text-slate-500">{cur?.desc}</p>
          </div>
          <LinkBadge />
        </header>

        <div className="p-4 lg:p-5">
          {settings.safetyBanner && page === 'drive' && (
            <div className="mb-3 flex items-center gap-3 rounded-xl bg-neon-amber/10 px-3 py-2 text-[13px] text-neon-amber ring-1 ring-neon-amber/30">
              <span className="pulse-dot h-2 w-2 rounded-full bg-neon-amber" />
              <span className="flex-1">
                真车模式下这些按钮会<strong>真的让车轮/舵机转动</strong> —— 先把车架空、确认四周无人再试。
              </span>
              <button className="btn-ghost !py-0.5 text-[11px]" onClick={() => store.patchSettings({ safetyBanner: false })}>
                不再提示
              </button>
            </div>
          )}
          <ErrorBanner />
          {sse === 'closed' && (
            <div className="mb-3 rounded-xl bg-neon-rose/10 px-3 py-2 text-[13px] text-neon-rose ring-1 ring-neon-rose/30">
              与本地桥的实时通道断了（后端没起来？）。正在自动重连…
            </div>
          )}
          {/* key={page}：一个页面炸了，切到别的页面要能恢复（否则错误状态会黏住） */}
          <PageBoundary key={page} title={cur?.title}>
            {children}
          </PageBoundary>
        </div>
      </main>
    </div>
  )
}
