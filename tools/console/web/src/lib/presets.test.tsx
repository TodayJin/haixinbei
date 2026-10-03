/**
 * 回归：预设列表里混进"缺字段的项"时，页面不许整页白屏。
 *
 * 用户 2026-10-03 在浏览器里报「舵机界面空的,设置页面也是空的」：桥把预设目录里的
 * 动作表存档 `actions.json`（只有 `{actions:[[a1,a2]…]}`，没有 kind/keys）当普通预设
 * 吐了出来，这两个页面读 `p.keys.length` 抛 TypeError ⇒ React 卸掉整棵树 ⇒ 一片空白。
 * 桥侧已归一（`bridge/state.py` 的 `_normalize_preset`），这里钉死前端这一侧的兜底。
 */
import { describe, expect, it } from 'vitest'
import type { ReactElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { normalizePresets } from './api'
import { store } from './store'
import { PageBoundary } from '../components/PageBoundary'
import { ServoPage } from '../pages/Servo'
import { SettingsPage } from '../pages/Settings'

describe('normalizePresets', () => {
  it('动作表存档那种缺 kind/keys 的项要补齐成契约形状', () => {
    const out = normalizePresets([{ name: 'actions', updated_ms: 1790990454087, actions: [[0, 0], [60, 120]] }])
    expect(out).toHaveLength(1)
    expect(out[0].name).toBe('actions')
    expect(out[0].keys).toEqual([])          // ← 原来这里是 undefined，页面读 .length 就炸
    expect(out[0].kind).toBe('mixed')
    expect(out[0].updated_ms).toBe(1790990454087)
  })

  it('整体不是数组、keys 不是数组、name 为空都要能兜住', () => {
    expect(normalizePresets(undefined)).toEqual([])
    expect(normalizePresets({ nope: 1 })).toEqual([])
    const [a, b] = normalizePresets([{ keys: 'x' }, { name: 'n', keys: ['params', 7, 'servo'] }])
    expect(a.keys).toEqual([])
    expect(a.name).toBe('preset-1')
    expect(b.keys).toEqual(['params', 'servo'])   // 非字符串的项清掉
  })
})

describe('拿到脏预设也不能白屏', () => {
  it('舵机页 / 设置页照常渲染出内容', async () => {
    const realFetch = globalThis.fetch
    // 只喂 /api/presets：故意返回"缺 keys/kind"的项（就是桥上原来的形状）
    globalThis.fetch = (async (input: RequestInfo | URL) =>
      new Response(JSON.stringify({ ok: true, items: [{ name: 'actions', updated_ms: 1 }] }), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      })) as typeof fetch
    try {
      await store.refreshPresets()
      const cases: Array<[string, () => ReactElement, string]> = [
        ['舵机与动作', ServoPage, '舵机'],
        ['设置', SettingsPage, '串口'],
      ]
      for (const [label, Page, marker] of cases) {
        const html = renderToStaticMarkup(<Page />)
        expect(html.length, `${label}页只有 ${html.length} 字节，像是白屏`).toBeGreaterThan(400)
        expect(html, `${label}页缺关键文案`).toContain(marker)
      }
    } finally {
      globalThis.fetch = realFetch
    }
  })
})

describe('PageBoundary', () => {
  it('把渲染异常变成一张能看见的错误卡', () => {
    const b = new PageBoundary({ children: null })
    b.state = { error: new TypeError('cannot read property length of undefined') }
    const html = renderToStaticMarkup(b.render() as ReactElement)
    expect(html).toContain('渲染失败')
    expect(html).toContain('TypeError')
    expect(html).toContain('cannot read property length of undefined')
  })
})
