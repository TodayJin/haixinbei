/**
 * 热路径量测夹具（**量的是真数字，不是估算**）—— 回答用户 m22973
 * 「IMU 那些波形一直在闪来闪去……上位机感觉有点卡，到底是为什么」。
 *
 * 为什么用 SSR 量：本工程的测试环境是 `environment: 'node'`，没有 jsdom / puppeteer，
 * 运行时依赖只有 react/react-dom/uplot/lucide-react。`renderToStaticMarkup` 量的是
 * **一次页面渲染的 JS 成本**（组件体 + JSX + 取数 + 折线点串），uPlot 的真实绘制量不到
 * （没有 DOM，泳道的 effect 根本不跑）。所以这里同时报四个数字，互相补齐：
 *
 *   1. `ms`：`renderToStaticMarkup` 的中位耗时（30 轮，去掉 3 轮热身）——"这一页渲染一次多贵"；
 *   2. `pts`：输出 HTML 里 `<polyline>/<polygon points="...">` 的总点数——"一页塞了多少个折线点"；
 *   3. `热取数/元取数`：一次渲染里 `store.getSnapshot` / `getMetaSnapshot` 被调了几次 ——
 *      热取数 ≈ 有几个组件吃整份快照（**热 tick 一到，这些组件全部重渲染**，这就是"卡"的乘数），
 *      元取数那些只吃低频 meta（不该跟着遥测抖）；
 *   4. `拷贝点/扫描点`：一次渲染里从环形缓冲**拷出来**／**扫过**的点数。`view()` 每帧只拷
 *      抽稀后的 ≤maxPoints 个点、但要扫全窗算极值（只读、不分配）；`snapshot*()` 是整窗全拷。
 *      这一列就是"每 tick 重来一遍"的那份。
 *
 * 前两项量"贵不贵"，后两项量"每 tick 重来多少次"。修完再跑一遍就是改前/改后对照。
 * ⚠ SSR 只量**一次渲染**的成本，量不到"窄订阅值没变就 bail out"省下的重渲染 ——
 *   那部分靠 `sparkline.test.ts` 的相位/量程回归 + 页面壳不再订阅热 tick（grep 可查）保证。
 */
import { afterAll, describe, expect, it } from 'vitest'
import { renderToStaticMarkup } from 'react-dom/server'
import { store } from './store'
import { CHANNELS } from './channels'
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

const T0 = 1_700_000_000_000
const HZ = 100
const SECONDS = 20
const FRAMES = HZ * SECONDS
const ROUNDS = 30
const WARMUP = 3

/** 真实结构的遥测：姿态小幅摆动、PWM 大刻度、灰度 8 路、其余字段与 types.ts 对齐 */
function mkTelem(i: number): Telem {
  const t = T0 + i * (1000 / HZ)
  const f = i % 2 === 0
  return {
    t,
    t_host: t,
    yaw: 12 * Math.sin(i / 47) + 0.3 * Math.sin(i / 3),
    pitch: 4 * Math.sin(i / 31) - 1,
    roll: 2.5 * Math.cos(i / 23),
    vl: 1400 + 60 * Math.sin(i / 11),
    vr: 1350 + 55 * Math.cos(i / 13),
    pwm_l: 3200 + 400 * Math.sin(i / 9),
    pwm_r: 3100 + 380 * Math.cos(i / 9),
    vbat_mv: 7400 - (i % 300),
    ibat_ma: 300 + (i % 120),
    servo1_us: 1500,
    servo2_us: 1500,
    servo1_deg: 10 * Math.sin(i / 60),
    servo2_deg: -8 * Math.cos(i / 60),
    line: [0, 1, 1, 0, 1, 1, 0, 1],
    gray: [10, 20, 30, 40, 50, 60, 70, 80].map((v) => v + (i % 17)),
    state_id: 0,
    loop_us: 800 + (i % 200),
    mode: 1,
    task_id: 0,
    action: 0,
    trace: { pos: 12 * Math.sin(i / 40), pid: 200 * Math.cos(i / 40) },
    angle: { err: 3 * Math.sin(i / 27), rate_ref: 40 * Math.sin(i / 19), corr: 900 * Math.cos(i / 21) },
    flags: {
      lost: false,
      slope: false,
      gyro_ok: true,
      motor: f,
      host_drive: f,
      logging: false,
      flash: false,
      fault: false,
    },
    nvram: 0,
  }
}

/** 灌满 20s × 100Hz 的环形缓冲（走和 SSE 完全同一条路径） */
function fillRing(): void {
  store.ring.clear()
  for (let i = 0; i < FRAMES; i++) store.feedTelem(mkTelem(i))
}

function median(xs: number[]): number {
  const s = [...xs].sort((a, b) => a - b)
  const m = s.length >> 1
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2
}

/** 数输出里 SVG 折线点总数：`points="x,y x,y …"` 的坐标对数 */
function countSvgPoints(html: string): number {
  let n = 0
  for (const m of html.matchAll(/points="([^"]*)"/g)) {
    const s = m[1].trim()
    if (!s) continue
    n += s.split(/\s+/).length
  }
  return n
}

/** 显示宽度（中文按 2 格）—— 只为了把表打齐 */
function pad(s: string, n: number): string {
  let w = 0
  for (const ch of s) w += ch.codePointAt(0)! > 0x2000 ? 2 : 1
  return s + ' '.repeat(Math.max(1, n - w))
}

interface Probed {
  ms: number
  pts: number
  /** 一次渲染里 `store.getSnapshot` 被调了几次（整份快照或挑标量都算） */
  subs: number
  /** 一次渲染里 `store.getVersion` 被调了几次 = 有几个组件订"热 tick 号"（波形那类） */
  hotTicks: number
  metaSubs: number
  /** 每 tick **拷出来**的点数（`view()` 的抽稀结果 / `snapshot*()` 的整窗） */
  copied: number
  /** 每 tick **扫过**的点数（`view()` 为了整窗极值扫全窗，只是读、不分配） */
  scanned: number
  /** 角上数字的 O(1) 取数次数（`lastValue()`） */
  lastCalls: number
  nan: boolean
}

function measure(Page: () => React.JSX.Element): Probed {
  const times: number[] = []
  for (let r = 0; r < ROUNDS + WARMUP; r++) {
    const t0 = performance.now()
    renderToStaticMarkup(<Page />)
    const dt = performance.now() - t0
    if (r >= WARMUP) times.push(dt)
  }

  // 打点渲染：单跑一轮，数"每个热 tick 要重来多少次"
  let subs = 0
  let hotTicks = 0
  let metaSubs = 0
  let copied = 0
  let scanned = 0
  let lastCalls = 0
  const realGet = store.getSnapshot
  const realGetVersion = store.getVersion
  const realGetMeta = store.getMetaSnapshot
  const realField = store.ring.snapshotField
  const realAll = store.ring.snapshot
  const realView = store.ring.view
  const realLast = store.ring.lastValue
  store.getSnapshot = () => {
    subs++
    return realGet()
  }
  store.getVersion = () => {
    hotTicks++
    return realGetVersion()
  }
  store.getMetaSnapshot = () => {
    metaSubs++
    return realGetMeta()
  }
  store.ring.snapshotField = (windowMs, fieldIndex, nowMs) => {
    const r = realField.call(store.ring, windowMs, fieldIndex, nowMs)
    copied += r.count
    scanned += r.count
    return r
  }
  store.ring.snapshot = (windowMs, nowMs) => {
    const r = realAll.call(store.ring, windowMs, nowMs)
    copied += r.count * CHANNELS.length
    scanned += r.count * CHANNELS.length
    return r
  }
  store.ring.view = (windowMs, fieldIndex, maxPoints, stepHint, nowMs) => {
    const r = realView.call(store.ring, windowMs, fieldIndex, maxPoints, stepHint, nowMs)
    copied += r.count
    scanned += r.windowCount
    return r
  }
  store.ring.lastValue = (fieldIndex, windowMs, nowMs) => {
    lastCalls++
    return realLast.call(store.ring, fieldIndex, windowMs, nowMs)
  }
  let html = ''
  try {
    html = renderToStaticMarkup(<Page />)
  } finally {
    store.getSnapshot = realGet
    store.getVersion = realGetVersion
    store.getMetaSnapshot = realGetMeta
    store.ring.snapshotField = realField
    store.ring.snapshot = realAll
    store.ring.view = realView
    store.ring.lastValue = realLast
  }
  return {
    ms: median(times),
    pts: countSvgPoints(html),
    subs,
    hotTicks,
    metaSubs,
    copied,
    scanned,
    lastCalls,
    nan: html.includes('NaN'),
  }
}

const PAGES: Array<[string, () => React.JSX.Element]> = [
  ['总览', OverviewPage],
  ['操控', DrivePage],
  ['IMU', ImuPage],
  ['传感器', SensorsPage],
  ['舵机与动作', ServoPage],
  ['调参', ParamsPage],
  ['任务', TasksPage],
  ['波形', ScopePage],
  ['控制与日志', ControlPage],
  ['设置', SettingsPage],
]

describe('热路径量测：20s × 100Hz 真实遥测下每页的渲染成本', () => {
  it('打印每页 ms / 折线点数 / 热订阅数 / 每 tick 拷贝点数', () => {
    fillRing()
    expect(store.ring.size).toBe(FRAMES)

    const rows: string[] = []
    rows.push('页面            ms(中位)  折线点  取快照  热tick  元取数  拷贝点  扫描点  角上取数')
    let badNan = ''
    for (const [name, Page] of PAGES) {
      const p = measure(Page)
      if (p.nan) badNan = name
      rows.push(
        pad(name, 16) +
          pad(p.ms.toFixed(2), 9) +
          pad(String(p.pts), 8) +
          pad(String(p.subs), 8) +
          pad(String(p.hotTicks), 8) +
          pad(String(p.metaSubs), 8) +
          pad(String(p.copied), 8) +
          pad(String(p.scanned), 8) +
          p.lastCalls,
      )
    }
    console.log('\n[hotpath] ' + rows.join('\n[hotpath] '))
    expect(badNan).toBe('')
    expect(rows.length).toBe(PAGES.length + 1)
  }, 180_000)
})

afterAll(() => {
  store.ring.clear()
})
