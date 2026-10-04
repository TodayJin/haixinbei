/**
 * 回归测试：「界面卡不卡」的关键设计不能再退化。
 *
 * 背景：用户 m20705 说上位机「感觉卡卡的」，于是 2026-10-03 做了三件事：
 *   1. `useMeta()`：只显示参数/设置/日志的页面订阅"不含 telem"的快照，
 *      不再被 100Hz 遥测拖着整页重渲染；
 *   2. 热路径合并到 HOT_MS(50ms) = 20Hz；
 *   3. `SeriesRing.snapshotField()`：波形一条泳道只拷**一个**通道，
 *      不再每次 `snapshot()` 把 21 个通道全拷一遍（21 倍的无用拷贝）。
 *
 * 2026-10-04 又补了三条（用户 m22973 再提「感觉有点卡」+「IMU 波形一直在闪」）：
 *   4. `SeriesRing.view()`：一趟同时算抽稀线 + 整窗 min/max/last，相位锚在绝对下标
 *      （`decimationStep()` 带死区），泳道点数上限定死 —— 每 tick 拷贝点从 4000~12000
 *      降到 236~1023；
 *   5. `nextRange()`：迷你波形纵轴做峰值包络 + 滞回（放大即时、收缩慢），
 *      不再每 tick 按当前窗口重新量程（那就是"闪来闪去"）；
 *   6. 页面壳/提示条改 `useMeta()`，读数改 `useHotSelect()` 窄订阅，IMU 页拆成叶子组件。
 *      账本在 `hotpath.test.tsx`，纵轴/抽稀的单元测试在 `sparkline.test.ts`。
 *
 * 2026-10-04（用户 m03559「我上下滚动就能感觉到这个画面是卡卡的」）再加三条：
 *   7. 滚动期间自动降特效：`html.is-scrolling` 一挂，玻璃模糊 + 脉冲动效全关（`lib/fx.ts`
 *      的 140ms 空闲摘类）。实测同一套 bundle、同样 2.5s 滚动：滚动期间强开模糊的合成器
 *      总耗时 1296ms vs 关掉 263ms（每帧 2.17 → 0.54ms），主线程 styleN 也从"每帧一次"归零；
 *   8. 灰度柱的高度**不再做 transition**（`transition-[height]` → `transition-colors`）：
 *      20Hz 遥测不停重启 150ms 的布局属性过渡 ⇒ 过渡永远在跑 ⇒ 每帧重排（layoutN 650→51、
 *      光栅化 8 倍、主线程 24%→14%）；
 *   9. 设置页多一个「低特效模式」开关（`settings.perfLite`），给显卡弱/远程桌面的机器常开。
 *
 * 这里把这几条钉死：谁把 telem 又塞回 meta 快照、或把节流拆掉、或让泳道退回
 * 全通道拷贝、或把柱高过渡/滚动降特效改回去，测试立刻红。真机上的帧率/CRC/延迟另有
 * `tools/console/tests/` 的字节级对拍与真板实测（见 memory/SESSIONS.md）。
 */
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

import { afterEach, describe, expect, it, vi } from 'vitest'
import { store } from './store'
import { CHANNELS } from './channels'
import { LITE_CLASS, SCROLLING_CLASS, SCROLL_IDLE_MS } from './fx'
import type { Telem } from './types'

const T0 = 1_700_000_000_000

/** 造一帧结构完整的遥测（字段名必须和 types.ts 的 Telem 一致） */
function mkTelem(i: number, t = T0 + i * 10): Telem {
  const f = i % 2 === 0
  return {
    t,
    t_host: t,
    yaw: 1 + i * 0.1,
    pitch: 2,
    roll: 3,
    vl: 100 + i,
    vr: 200 - i,
    pwm_l: 10,
    pwm_r: -10,
    vbat_mv: 7400,
    ibat_ma: 300,
    servo1_us: 1500,
    servo2_us: 1500,
    servo1_deg: 0,
    servo2_deg: 0,
    line: [0, 1, 1, 0, 1, 1, 0, 1],
    gray: [1, 2, 3, 4, 5, 6, 7, 8],
    state_id: 0,
    loop_us: 800 + i,
    mode: 1,
    task_id: 0,
    action: 0,
    trace: { pos: 0, pid: 0 },
    angle: { err: 0, rate_ref: 0, corr: 0 },
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

describe('遥测热路径的刷新成本（用户 m20705「感觉卡卡的」）', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  it('200 帧遥测不叫醒 useMeta() 订阅者，整快照订阅者也只被叫起 1~2 次', () => {
    vi.useFakeTimers()
    let full = 0
    let meta = 0
    const offFull = store.subscribe(() => {
      full += 1
    })
    const offMeta = store.subscribeMeta(() => {
      meta += 1
    })
    try {
      for (let i = 0; i < 200; i++) store.feedTelem(mkTelem(i))
      // 关键断言：低频页面完全不吃热路径
      expect(meta).toBe(0)
      // 200 帧挤在 50ms 窗口里 ⇒ 合并成极少数次通知（绝不是 200 次）
      expect(full).toBeGreaterThanOrEqual(1)
      expect(full).toBeLessThanOrEqual(3)

      vi.advanceTimersByTime(60)
      expect(meta).toBe(0)
      expect(full).toBeLessThanOrEqual(4)
      // 但最后一帧确实进去了（合并只是少刷，不是丢数据）
      expect(store.getSnapshot().telem?.t).toBe(T0 + 199 * 10)
    } finally {
      offFull()
      offMeta()
    }
  })

  it('浏览器里热渲染挂在 rAF 上：同一帧内的遥测合并成一次，帧没到就不渲染', () => {
    const frames: FrameRequestCallback[] = []
    vi.stubGlobal('window', {
      requestAnimationFrame: (fn: FrameRequestCallback) => {
        frames.push(fn)
        return frames.length
      },
    })
    vi.useFakeTimers()
    vi.advanceTimersByTime(500) // 冲掉上一个测试遗留的节流窗口与定时器
    frames.length = 0
    let full = 0
    const off = store.subscribe(() => {
      full += 1
    })
    try {
      store.feedTelem(mkTelem(300))
      store.feedTelem(mkTelem(301))
      expect(frames.length).toBe(1) // 两帧遥测只排一次绘制
      expect(full).toBe(0) // 绘制帧还没到：一次渲染都没有
      frames.shift()!(0)
      expect(full).toBe(1) // 一帧到了，合并成一次渲染

      // 窗口内的尾帧：定时器到点后也走 rAF，而不是同步渲染
      store.feedTelem(mkTelem(302))
      vi.advanceTimersByTime(60)
      expect(full).toBe(1)
      expect(frames.length).toBe(1)
      frames.shift()!(0)
      expect(full).toBe(2)
      // 合并只是少刷，最后一帧不能丢
      expect(store.getSnapshot().telem?.t).toBe(T0 + 302 * 10)
    } finally {
      off()
      vi.unstubAllGlobals()
    }
  })

  it('meta 快照在热路径上保持同一引用（React 直接 bail out，不重渲染）', () => {
    vi.useFakeTimers()
    const before = store.getMetaSnapshot()
    store.feedTelem(mkTelem(1))
    vi.advanceTimersByTime(60)
    expect(store.getMetaSnapshot()).toBe(before)
    // 而且它里面本来就没有 telem / ringFill / version
    const keys = Object.keys(before)
    expect(keys).not.toContain('telem')
    expect(keys).not.toContain('ringFill')
    expect(keys).not.toContain('version')
  })

  it('波形单通道快照只拷一个通道，值与全量快照的同一通道一致', () => {
    const ring = store.ring
    ring.clear()
    expect(CHANNELS.length).toBeGreaterThan(1)
    for (let i = 0; i < 40; i++) {
      ring.push(T0 + i * 10, Array.from({ length: CHANNELS.length }, (_, f) => i * 100 + f))
    }
    const now = T0 + 39 * 10
    const all = ring.snapshot(10_000, now)
    const one = ring.snapshotField(10_000, 3, now)

    expect(all.count).toBe(40)
    expect(all.v.length).toBe(CHANNELS.length) // 全量：21 个通道
    expect(one.count).toBe(40)
    expect(Array.from(one.v)).toEqual(Array.from(all.v[3])) // 单通道：只拷 1 个
    expect(Array.from(one.t)).toEqual(Array.from(all.t))
    expect(Array.from(one.v).slice(0, 3)).toEqual([3, 103, 203])

    // 越界要钳位，不能崩（波形页允许改通道索引）
    expect(ring.snapshotField(10_000, 999, now).count).toBe(40)
    expect(ring.snapshotField(10_000, -5, now).v[0]).toBe(all.v[0][0])
  })
})

describe('滚动流畅度（用户 m03559「上下滚动卡卡的」）', () => {
  const WEB = new URL('../../', import.meta.url)
  const src = (p: string) => readFileSync(fileURLToPath(new URL(p, WEB)), 'utf8')

  it('滚动时降特效的两条路还接在页面壳上（is-scrolling 与 perf-lite）', () => {
    const shell = src('src/components/Shell.tsx')
    expect(shell).toContain('installScrollFx(window, document)')
    expect(shell).toContain('applyLiteFx(document.documentElement.classList, settings.perfLite)')
    const css = src('src/index.css')
    for (const sel of [`html.${SCROLLING_CLASS} header`, `html.${LITE_CLASS} header`, `html.${SCROLLING_CLASS} .glass`]) {
      expect(css).toContain(sel)
    }
    // 去掉模糊后 header 必须**不透明**（半透明会透出底下的参数行：0.94 都看得见）
    expect(css).toMatch(/html\.is-scrolling header,[\s\S]{0,80}background-color: rgb\(7 10 22\);/)
    expect(css).not.toContain('rgb(7 10 22 / 0.94)')
    expect(SCROLL_IDLE_MS).toBeGreaterThanOrEqual(80)
    expect(SCROLL_IDLE_MS).toBeLessThanOrEqual(400)
  })

  it('灰度柱的高度不带 transition（布局属性过渡 = 每帧重排）', () => {
    const ui = src('src/components/ui.tsx')
    // 只抓 className 里的实际用法（文档注释里提到 transition-[height] 是允许的）
    expect(ui).not.toMatch(/className=\{`[^`]*transition-\[(height|max-height)\]/)
    // 两条柱状图都还在（别为了省事把组件删了）
    expect(ui.match(/transition-colors duration-150/g)?.length ?? 0).toBeGreaterThanOrEqual(2)
  })
})
