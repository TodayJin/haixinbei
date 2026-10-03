/**
 * CSV 导出（前端生成，不依赖后端）。
 * 纯函数，单测覆盖（`csv.test.ts`）。
 */

import type { Snapshot } from './ring'

/** 单个单元格转义：含逗号/引号/换行就加引号，内部引号翻倍 */
export function csvCell(v: string | number | null | undefined, decimals?: number): string {
  let s: string
  if (v === null || v === undefined) s = ''
  else if (typeof v === 'number') {
    if (!Number.isFinite(v)) s = ''
    else s = decimals === undefined ? String(v) : v.toFixed(decimals)
  } else s = v
  if (/[",\r\n]/.test(s)) s = `"${s.replace(/"/g, '""')}"`
  return s
}

export function csvRow(cells: (string | number | null | undefined)[], decimals?: number): string {
  return cells.map((c) => csvCell(c, decimals)).join(',')
}

export interface CsvColumn {
  /** 表头（含单位） */
  header: string
  /** 采样点在 Snapshot 里的通道下标；-1 表示这一列是时间戳 */
  field: number
  decimals?: number
}

/**
 * 把一段快照导成 CSV。
 * 第一列固定是时间：`t_ms`（绝对毫秒）与 `t_s`（相对第一点的秒）。
 */
export function snapshotToCsv(snap: Snapshot, columns: CsvColumn[], opts?: { t0?: number }): string {
  const t0 = opts?.t0 ?? (snap.count > 0 ? snap.t[0] : 0)
  const lines: string[] = []
  lines.push(csvRow(['t_ms', 't_s', ...columns.map((c) => c.header)]))
  for (let k = 0; k < snap.count; k++) {
    const cells: string[] = [String(Math.round(snap.t[k])), ((snap.t[k] - t0) / 1000).toFixed(3)]
    for (const c of columns) {
      const arr = snap.v[c.field]
      // 每列自己的小数位（t_ms/t_s 已经手动格式化过，不再走 csvCell）
      cells.push(csvCell(arr ? arr[k] : null, c.decimals))
    }
    lines.push(cells.join(','))
  }
  return lines.join('\r\n') + '\r\n'
}

/** 文件名：带时间戳，便于多次录制不覆盖 */
export function csvFileName(prefix: string, atMs: number): string {
  const d = new Date(atMs)
  const p = (n: number) => n.toString().padStart(2, '0')
  return `${prefix}_${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(
    d.getMinutes(),
  )}${p(d.getSeconds())}.csv`
}

/** 触发浏览器下载（测试环境里没有 document，所以调用方自行判空） */
export function downloadText(filename: string, text: string, mime = 'text/csv;charset=utf-8'): void {
  const blob = new Blob(['\ufeff' + text], { type: mime })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  document.body.appendChild(a)
  a.click()
  a.remove()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
