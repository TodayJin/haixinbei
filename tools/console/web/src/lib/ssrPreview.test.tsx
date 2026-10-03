/**
 * SSR 预览生成器（不是单元测试，是**产物生成器**）—— 因为本沙箱里浏览器起不来
 * （Chrome/Edge headless 都失败，见 README「截图」一节），这里改用另一条能给"人眼"
 * 提供证据的路子：
 *
 *   1. 连上 mock 假后端，按契约喂真实结构的 params/actions/cal/presets；
 *   2. 从 `/api/stream` 收 2.5 秒 telem，逐帧 `store.feedTelem()`（和 SSE 同一条路径）；
 *   3. 用 `react-dom/server` 把 9 个页面渲染成静态 HTML，配上 `dist/` 里真实的 CSS；
 *   4. 写到 `<仓库根>/out/console/ssr/<页面>.html` —— 在真浏览器里打开就是成品长相，
 *      也能直接丢给无头浏览器截图（命令见 README）。
 *
 * `node mock/server.mjs` 没起就整体 skip（不让 CI 变红）：
 *   node mock/server.mjs &
 *   npx vitest run src/lib/ssrPreview.test.ts
 */
import { describe, expect, it } from 'vitest'
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { renderToStaticMarkup } from 'react-dom/server'
import { store } from './store'
import type { Telem } from './types'
import { OverviewPage } from '../pages/Overview'
import { DrivePage } from '../pages/Drive'
import { ImuPage } from '../pages/Imu'
import { SensorsPage } from '../pages/Sensors'
import { ServoPage } from '../pages/Servo'
import { ParamsPage } from '../pages/Params'
import { TasksPage } from '../pages/Tasks'
import { ScopePage } from '../pages/Scope'
import { ControlPage } from '../pages/Control'
import { SettingsPage } from '../pages/Settings'

const BASE = process.env.CONSOLE_BASE || 'http://127.0.0.1:8770'
/** 本文件在 <仓库根>/tools/console/web/src/lib ⇒ 上溯 5 级就是仓库根 */
const HERE = dirname(fileURLToPath(import.meta.url))
const OUT_DIR = resolve(HERE, '..', '..', '..', '..', '..', 'out', 'console', 'ssr')
const TITLES: Record<string, string> = {
  overview: '总览',
  drive: '操控',
  imu: 'IMU',
  sensors: '传感器',
  servo: '舵机与动作',
  params: '调参',
  tasks: '任务',
  scope: '波形',
  control: '控制与日志',
  settings: '设置',
}

async function collectTelem(ms = 2500): Promise<Telem[]> {
  const res = await fetch(`${BASE}/api/stream`, { signal: AbortSignal.timeout(ms + 1500) })
  const reader = res.body?.getReader()
  if (!reader) return []
  const dec = new TextDecoder()
  const out: Telem[] = []
  let buf = ''
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    const { value, done } = await reader.read()
    if (done) break
    buf += dec.decode(value, { stream: true })
    const chunks = buf.split('\n\n')
    buf = chunks.pop() ?? ''
    for (const c of chunks) {
      const ev = /^event: (\S+)/m.exec(c)?.[1]
      const data = /^data: (.*)$/m.exec(c)?.[1]
      if (ev === 'telem' && data) {
        try {
          out.push(JSON.parse(data) as Telem)
        } catch {
          /* 半截帧，忽略 */
        }
      }
    }
  }
  await reader.cancel()
  return out
}

async function readCss(): Promise<string> {
  const assets = join(process.cwd(), 'dist', 'assets')
  const files = await readdir(assets)
  const css = files.find((f) => f.endsWith('.css'))
  if (!css) throw new Error('dist/assets 里没有 CSS，先 npm run build')
  return readFile(join(assets, css), 'utf8')
}

describe('SSR 预览生成（需要 mock 在 127.0.0.1:8770）', () => {
  it('把 10 个页面渲染成 out/console/ssr/*.html', async () => {
    let live = false
    try {
      live = (await fetch(`${BASE}/api/health`, { signal: AbortSignal.timeout(1500) })).ok
    } catch {
      live = false
    }
    if (!live) {
      console.warn(`[ssrPreview] ${BASE} 没起来，跳过（先 node mock/server.mjs）`)
      return
    }

    // 前端代码一律用相对路径 `/api/...`（成品由桥同源托管）——在 Node 里相对 URL
    // 没法 fetch，所以这里只给"预览生成"打一个补丁：凡是 `/api` 开头的都补上 BASE。
    // 生产代码一行不动。
    const realFetch = globalThis.fetch
    globalThis.fetch = ((input: RequestInfo | URL, init?: RequestInit) =>
      typeof input === 'string' && input.startsWith('/api')
        ? realFetch(`${BASE}${input}`, init)
        : realFetch(input as RequestInfo, init)) as typeof fetch

    await store.refreshAll()
    const frames = await collectTelem()
    expect(frames.length).toBeGreaterThan(10)
    for (const t of frames) store.feedTelem(t)

    const css = await readCss()
    await mkdir(OUT_DIR, { recursive: true })

    const pages: Array<[string, () => React.JSX.Element]> = [
      ['overview', OverviewPage],
      ['drive', DrivePage],
      ['imu', ImuPage],
      ['sensors', SensorsPage],
      ['servo', ServoPage],
      ['params', ParamsPage],
      ['tasks', TasksPage],
      ['scope', ScopePage],
      ['control', ControlPage],
      ['settings', SettingsPage],
    ]

    for (const [id, Page] of pages) {
      const body = renderToStaticMarkup(<Page />)
      expect(body.length).toBeGreaterThan(400)
      const html = `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>控制台 · ${TITLES[id]}（SSR 预览）</title>
<style>${css}</style></head>
<body><div id="preview" class="min-h-screen p-4">${body}</div>
<!-- 这是 SSR 静态预览：交互(点击/滑条)不会生效，只看版式与数据。 -->
</body></html>`
      await writeFile(join(OUT_DIR, `${id}.html`), html, 'utf8')
    }
    console.log(`[ssrPreview] 已写出 ${pages.length} 个页面到 ${OUT_DIR}（喂了 ${frames.length} 帧遥测）`)
  }, 30000)
})
