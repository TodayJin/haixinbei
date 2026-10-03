/**
 * 极简哈希路由（不引 react-router）。
 * 页面 id 与 API.md §5 的 9 个页面一一对应。
 */

import { useSyncExternalStore } from 'react'

export type PageId =
  | 'overview'
  | 'drive'
  | 'imu'
  | 'sensors'
  | 'servo'
  | 'params'
  | 'scope'
  | 'control'
  | 'settings'

export interface PageDef {
  id: PageId
  title: string
  /** 侧栏一行说明 */
  desc: string
}

export const PAGES: PageDef[] = [
  { id: 'overview', title: '总览', desc: '链路 / 状态 / 关键量' },
  { id: 'drive', title: '驾驶', desc: '轮速与快捷指令' },
  { id: 'imu', title: 'IMU', desc: '姿态仪表与曲线' },
  { id: 'sensors', title: '传感器', desc: '8 路灰度与循迹' },
  { id: 'servo', title: '舵机与动作', desc: '14 个动作的角度' },
  { id: 'params', title: '调参', desc: '三环参数与下发' },
  { id: 'scope', title: '波形', desc: '多通道独立量程' },
  { id: 'control', title: '控制与日志', desc: '命令 / 记录 / 回放' },
  { id: 'settings', title: '设置', desc: '缓冲 / 预设 / 关于' },
]

const listeners = new Set<() => void>()
let current: PageId = readHash()

function readHash(): PageId {
  const raw = (typeof location !== 'undefined' ? location.hash : '').replace(/^#\/?/, '')
  const hit = PAGES.find((p) => p.id === raw)
  return (hit ? hit.id : 'overview') as PageId
}

function onHashChange(): void {
  const next = readHash()
  if (next !== current) {
    current = next
    listeners.forEach((fn) => fn())
  }
}

if (typeof window !== 'undefined') {
  window.addEventListener('hashchange', onHashChange)
}

export function navigateTo(id: PageId): void {
  if (typeof location !== 'undefined') {
    location.hash = `#/${id}`
  } else {
    current = id
    listeners.forEach((fn) => fn())
  }
}

function subscribe(fn: () => void): () => void {
  listeners.add(fn)
  return () => listeners.delete(fn)
}

export function usePage(): PageId {
  return useSyncExternalStore(subscribe, () => current, () => current)
}
