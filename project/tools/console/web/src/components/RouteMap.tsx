import React from 'react'
import { Badge } from './ui'
import { store, useMeta } from '../lib/store'
import type { RouteMark } from '../lib/types'

/** ★2026-10-11 加（用户："通过编码器以及 imu 的数据在上位机上给我一个行进路线的缩略图"）。
 *
 *  数据来自桥的航位推算（`v=(vl+vr)/2`、`x+=v·cos(yaw)·dt`，见 `bridge/state.py` 的 RouteTracker），
 *  **固件一行没改** —— 遥测里本来就有 `vl/vr`（编码器 mm/s）和 `yaw`（JY61P 航向）。
 *
 *  画图规矩（用户 2026-10-11 定的）：
 *   1. **起点贴画板左边**，**起步朝向摆成正右** ⇒ 车往右跑，图是一条"从左往右"的带子；
 *      用**固定的 `yaw0`**（本次清零后第一帧的 yaw）而不是实时 yaw —— 否则车一转弯整张图跟着转，没法看；
 *   2. **等比缩放**（x/y 同一个系数）：按框拉伸会把跑道压扁，看着像"跑偏了"其实是画错了；
 *   3. **y 轴翻转**：数据是数学系（y 向前），SVG 是屏幕系（y 向下）；
 *   4. **最小视野 240mm**：车还没怎么动时别把画面放到看不出一米有多长。
 *
 *  另外：**只有跑任务时才记录**（桥侧做的，用户要求）—— 手动开车/调试时的乱窜不会混进来，
 *  而且任务一开跑就自动清零，每次跑都是干净的一张图。
 */

/** 最小视野（mm）。太小会把几毫米的抖动放大成"剧烈摆动"，误导人。 */
const MIN_SPAN_MM = 240

/** 打点的颜色/含义 —— 只放"能帮你定位问题"的两种。 */
const MARK: Record<RouteMark['k'], string> = { state: '#22d3ee', lost: '#fb7185' }

export function RouteMap({ height = 240 }: { height?: number }) {
  const { route, link } = useMeta()
  // ★2026-10-11：模拟器里这张图**跟真车无关**，必须在界面上说清楚，别再误导人：
  //   ① 模拟器的任务**不跑块**，时间线是编的（每块固定 FAKE_BLOCK_MS）⇒ 你的 WAIT_MS 3000 只算 300ms；
  //   ② 模拟器的 IMU 航向也是编的、还会漂（实测一次跑里从 -64.6° 漂到 -61.6°）⇒ 直行都画成弯的；
  //   ③ 就算前两条都修好也对不上 —— **真车的路线是循迹走出来的**，任务块只是在关键点下命令，
  //      而模拟器里根本没有那条线。
  const pts = route?.pts ?? []
  const yaw0 = route?.yaw0 ?? 0

  /** 把"起步朝向"转到 +x（正右）之后的坐标。 */
  const rot = React.useMemo(() => {
    const r = (-yaw0 * Math.PI) / 180
    const c = Math.cos(r)
    const s = Math.sin(r)
    const f = (x: number, y: number): [number, number] => [x * c - y * s, x * s + y * c]
    return { f }
  }, [yaw0])
  const P = React.useMemo(() => pts.map(([x, y]) => rot.f(x, y)), [pts, rot])

  const geom = React.useMemo(() => {
    // ★起点固定参与：x 上界从 0 起算，所以"起点贴左边"永远成立（车倒车才可能出现负值）
    let x0 = 0
    let x1 = 0
    let y0 = 0
    let y1 = 0
    for (const [x, y] of P) {
      if (x < x0) x0 = x
      if (x > x1) x1 = x
      if (y < y0) y0 = y
      if (y > y1) y1 = y
    }
    return { x0, x1, y0, y1, spanX: Math.max(x1 - x0, MIN_SPAN_MM), spanY: Math.max(y1 - y0, MIN_SPAN_MM) }
  }, [P])

  const W = 420
  const H = height
  const pad = 16
  // ★等比：x/y 同一个缩放系数（取小的那个，保证整条都装得下）
  const s = Math.min((W - pad * 2) / geom.spanX, (H - pad * 2) / geom.spanY)
  /** 纵向中心：轨迹范围的中点（→ 画在 H/2） */
  const cy = (geom.y0 + geom.y1) / 2
  const px = (x: number) => pad + (x - geom.x0) * s
  const py = (y: number) => H / 2 + (cy - y) * s // y 翻转（数学系 → 屏幕系）+ 纵向居中

  const cur = route?.cur ?? [0, 0]
  const curP = rot.f(cur[0], cur[1])
  const head = route?.head ?? 0
  const distM = (route?.dist_mm ?? 0) / 1000
  const barMm = s * 1000 <= W - pad * 2 ? 1000 : 500
  const barPx = barMm * s

  const d = P.length > 1 ? 'M ' + P.map(([x, y]) => `${px(x).toFixed(1)} ${py(y).toFixed(1)}`).join(' L ') : ''

  return (
    <div>
      <div className="mb-2 flex flex-wrap items-center gap-2 text-[12px]">
        <Badge tone={pts.length ? 'mint' : 'slate'}>{pts.length ? `${pts.length} 点` : '等任务开跑'}</Badge>
        <span className="num text-slate-300">里程 {distM.toFixed(2)} m</span>
        <span className="num text-slate-500" title="车头相对「起步朝向」转了多少度（正 = 右转）">
          航向 {(head - yaw0).toFixed(0)}°
        </span>
        <span className="num text-slate-500" title="轨迹外接矩形：左右 × 前后">
          范围 {((geom.x1 - geom.x0) / 10).toFixed(0)}×{((geom.y1 - geom.y0) / 10).toFixed(0)} cm
        </span>
        <button
          className="btn-ghost ml-auto !px-2 !py-0.5 text-[11px]"
          onClick={() => void store.resetRoute()}
          title="把轨迹清零（任务一开跑也会自动清）"
        >
          清零
        </button>
      </div>

      <svg viewBox={`0 0 ${W} ${H}`} className="w-full rounded-md bg-ink-900/60 ring-1 ring-white/10" style={{ height: H }}>
        {/* 网格：每格 = 比例尺那条，以起点为原点 */}
        {Array.from({ length: 21 }, (_, i) => i - 4).map((k) => (
          <React.Fragment key={k}>
            <line
              x1={px(0) + k * barPx}
              x2={px(0) + k * barPx}
              y1={0}
              y2={H}
              stroke="#ffffff"
              strokeOpacity={k === 0 ? 0.14 : 0.05}
            />
            <line
              x1={0}
              x2={W}
              y1={H / 2 + k * barPx}
              y2={H / 2 + k * barPx}
              stroke="#ffffff"
              strokeOpacity={k === 0 ? 0.14 : 0.05}
            />
          </React.Fragment>
        ))}

        {/* 起跑线（起点处的竖直虚线）*/}
        <line x1={px(0)} x2={px(0)} y1={pad / 2} y2={H - pad / 2} stroke="#34d399" strokeOpacity={0.35} strokeDasharray="3 3" />

        {d && <path d={d} fill="none" stroke="#22d3ee" strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" opacity={0.9} />}

        {/* 打点：状态切换（青，带状态号）/ 丢线（红） */}
        {(route?.marks ?? []).map((m) => {
          const [mx, my] = rot.f(m.x, m.y)
          return (
            <g key={`${m.k}-${m.i}`}>
              <circle cx={px(mx)} cy={py(my)} r={m.k === 'lost' ? 3.2 : 2.4} fill={MARK[m.k]} fillOpacity={m.k === 'lost' ? 0.95 : 0.7} />
              {m.k === 'state' && (
                <text x={px(mx) + 4} y={py(my) - 3} fontSize={9} fill="#22d3ee" fillOpacity={0.85}>
                  {m.v}
                </text>
              )}
            </g>
          )
        })}

        {/* 起点 */}
        <circle cx={px(0)} cy={py(0)} r={3.4} fill="#34d399" />
        <text x={px(0) + 6} y={py(0) + 13} fontSize={9} fill="#34d399">
          起点
        </text>

        {/* 当前位置 + 车头箭头：起步朝向 = 正右 ⇒ 旋转量是"相对起步转过的角度"，SVG 的 y 翻了取负 */}
        {P.length > 0 && (
          <g transform={`translate(${px(curP[0])} ${py(curP[1])}) rotate(${-(head - yaw0)})`}>
            <path d="M 7 0 L -5 -4.5 L -2.5 0 L -5 4.5 Z" fill="#fbbf24" stroke="#0b1220" strokeWidth={0.8} />
          </g>
        )}

        {pts.length === 0 && (
          <text x={W / 2} y={H / 2} fontSize={12} fill="#64748b" textAnchor="middle">
            还没跑任务 —— 按「开始」跑起来就会画（手动开车不画）
          </text>
        )}

        {/* 比例尺 */}
        <g transform={`translate(${pad} ${H - 12})`}>
          <line x1={0} x2={barPx} y1={0} y2={0} stroke="#94a3b8" strokeWidth={2} />
          <line x1={0} x2={0} y1={-4} y2={4} stroke="#94a3b8" strokeWidth={2} />
          <line x1={barPx} x2={barPx} y1={-4} y2={4} stroke="#94a3b8" strokeWidth={2} />
          <text x={barPx + 6} y={4} fontSize={10} fill="#94a3b8">
            {barMm === 1000 ? '1 m' : '0.5 m'}
          </text>
        </g>
      </svg>

      {link?.mode === 'sim' && (
        <div className="mt-1.5 rounded-md border border-neon-amber/30 bg-neon-amber/10 px-2 py-1.5 text-[11px] leading-relaxed text-neon-amber">
          <b>⚠ 现在连的是模拟器，这条轨迹跟真车无关</b>，只能用来验证界面画得对不对：
          模拟器的任务<b>不跑块</b>、时长是编的（每块固定几百 ms），IMU 航向也是编的还会漂
          ⇒ 直行都能画成弯的。
          <br />
          更要紧的是：<b>真车的路线是"循迹"走出来的</b>（车跟着地上的线跑），任务块只在关键点下命令 ——
          模拟器里没有那条线，所以它<b>永远不可能</b>画出真车的路线。要看真路线，只能上车跑。
        </div>
      )}
      <p className="mt-1.5 text-[11px] leading-relaxed text-slate-500">
        由<b>编码器轮速</b>（mm/s）和 <b>IMU 航向</b>积分出来的俯视轨迹：<b>起点在左边、起步朝向朝右</b>，
        所以车是"从左往右"跑的。青点 = 状态切换（数字是状态号），红点 = 丢线。
        <b>只在跑任务时记录</b>，任务开跑自动清零；停车时积分严格为 0，所以不会漂。
      </p>
    </div>
  )
}
