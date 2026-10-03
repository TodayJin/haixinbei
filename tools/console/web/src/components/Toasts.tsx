import React from 'react'
import { store, useStore } from '../lib/store'

const TONE: Record<string, string> = {
  ok: 'ring-neon-mint/30 bg-neon-mint/10 text-neon-mint',
  err: 'ring-neon-rose/30 bg-neon-rose/10 text-neon-rose',
  info: 'ring-white/15 bg-white/5 text-slate-300',
}

/** 右下角提示条：写操作成功/失败都在这里（失败显示后端 error 原文） */
export function Toasts() {
  const { toasts } = useStore()
  return (
    <div className="pointer-events-none fixed bottom-4 right-4 z-50 flex w-[min(92vw,420px)] flex-col gap-2">
      {toasts.map((t) => (
        <button
          key={t.id}
          onClick={() => store.dismissToast(t.id)}
          className={`pointer-events-auto animate-riseIn rounded-xl px-3 py-2 text-left text-[13px] ring-1 backdrop-blur-md ${TONE[t.kind]}`}
        >
          {t.text}
        </button>
      ))}
    </div>
  )
}

/** 顶部错误横幅（最近一次后端错误；点掉即消失） */
export function ErrorBanner() {
  const { lastError, toasts } = useStore()
  const [dismissed, setDismissed] = React.useState<string | null>(null)
  if (!lastError || dismissed === lastError || toasts.some((t) => t.kind === 'err')) return null
  return (
    <div className="mb-3 flex items-center justify-between gap-3 rounded-xl bg-neon-rose/10 px-3 py-2 text-[13px] text-neon-rose ring-1 ring-neon-rose/30">
      <span className="truncate">后端报错：{lastError}</span>
      <button className="btn-ghost !py-0.5 text-[11px]" onClick={() => setDismissed(lastError)}>
        知道了
      </button>
    </div>
  )
}
