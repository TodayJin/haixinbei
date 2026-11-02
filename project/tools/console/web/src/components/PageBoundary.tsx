import React from 'react'
import { AlertTriangle } from 'lucide-react'
import { Card } from './ui'

/**
 * 页面级错误边界：某个页面渲染炸了，不能把整个控制台拖成白屏。
 *
 * 真实踩到过的例子：桥把 preset_dir 里的 `actions.json`（动作表存档，没有 kind/keys
 * 字段）当成普通预设吐给前端，舵机页/设置页读 `p.keys.length` 抛 TypeError ⇒ React
 * 卸载整棵树，浏览器里只剩一片空白，看不出是哪出的问题。现在会显示成一张红卡，
 * 页面上还留下报错原文。
 */
interface Props {
  children: React.ReactNode
  /** 出错时卡片标题里用的页面名 */
  title?: string
}
interface State {
  error: Error | null
}

export class PageBoundary extends React.Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: React.ErrorInfo) {
    // 留在浏览器控制台里，方便直接复制堆栈
    console.error('[console] 页面渲染失败：', error, info.componentStack)
  }

  render() {
    const e = this.state.error
    if (!e) return this.props.children
    return (
      <Card
        title={`${this.props.title ?? '这个页面'}渲染失败`}
        right={
          <button className="btn-ghost" onClick={() => this.setState({ error: null })}>
            重试
          </button>
        }
      >
        <div className="flex items-start gap-3 text-[13px] leading-relaxed">
          <AlertTriangle size={16} className="mt-0.5 shrink-0 text-neon-rose" />
          <div className="min-w-0">
            <p className="text-slate-300">
              页面在浏览器里抛异常了（其它页面还能用）。把这行字连同浏览器控制台的堆栈一起发出来就能定位。
            </p>
            <pre className="mt-2 max-h-40 overflow-auto rounded-lg bg-ink-900/70 p-2 text-[12px] text-neon-rose ring-1 ring-white/10">
              {e.name}: {e.message}
            </pre>
          </div>
        </div>
      </Card>
    )
  }
}
