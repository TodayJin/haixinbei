/**
 * 3D 车模型（IMU 页）：纯 CSS 3D，零依赖 —— 没有模型文件，也没有 three.js。
 * 车壳都是 `Box` 拼出来的长方体（6 个面各一个 div），姿态换算全在 lib/attitude3d.ts
 * （有单测；本机跑不了浏览器截图，只能靠那个自证）。
 *
 * 车型 = **两轮履带车**（用户 m01390：左右各一条很长的履带，不是四轮小车）：
 *   左右各一条 `car3d-track` 长履带（d=210，比 150 长的车体前后都长出一截），
 *   履带顶上压 5 对 `car3d-cleat` 履带齿，侧面/顶面用 CSS 重复渐变画履带纹；
 *   中间夹扁平车体 `car3d-body`（w60 h26 d150），上面扣一层 `car3d-deck`，顶上是
 *   OLED 小屏 `car3d-screen`；车头挂灰度探头排 `car3d-sensor` + 摄像头 `car3d-lens`
 *   + 两个灯 `car3d-lamp`，车尾两个尾灯 `car3d-tail`，车顶一根天线 `car3d-antenna`。
 *
 * 坐标（CSS 约定）：x 右、y 下、z 朝屏幕外；车头 = +z、车的左侧 = +x、地面 = y0 平面。
 * 相机 = `.car3d-stage` 上的 `rotateY(az) rotateX(-el)`，拖鼠标/手指改 az/el。
 * 姿态口径跟固件 OLED 屏上那个方框一致（默认轴档 M3）—— 为什么是"轴对调 + 符号"，
 * 见 lib/attitude3d.ts 文件头（那边引了 app_ui.c 的行号）。
 */
import React from 'react'
import { DEFAULT_VIEW, carTransform, orbitView, stageTransform, type ViewAngle } from '../lib/attitude3d'

type FaceName = 'front' | 'back' | 'left' | 'right' | 'top' | 'bottom'

/** 一个长方体：`x/y/z` 是它在父坐标系里的中心，`cls` 是面的底色类名（可逐面覆盖） */
function Box({
  w,
  h,
  d,
  x = 0,
  y = 0,
  z = 0,
  cls,
  face,
}: {
  w: number
  h: number
  d: number
  x?: number
  y?: number
  z?: number
  cls: string
  face?: Partial<Record<FaceName, string>>
}) {
  // 六个面的位置：先把面平移出半个厚度（translateZ），再把它转到对应方向。
  // 面的 transform-origin 是自身中心，而面用负 margin 摆成"中心在盒心"，
  // 所以这些旋转都绕盒心转，出来的就是标准长方体。
  const sides: { name: FaceName; t: string; fw: number; fh: number }[] = [
    { name: 'front', t: `translateZ(${d / 2}px)`, fw: w, fh: h },
    { name: 'back', t: `rotateY(180deg) translateZ(${d / 2}px)`, fw: w, fh: h },
    { name: 'right', t: `rotateY(90deg) translateZ(${w / 2}px)`, fw: d, fh: h },
    { name: 'left', t: `rotateY(-90deg) translateZ(${w / 2}px)`, fw: d, fh: h },
    { name: 'top', t: `rotateX(90deg) translateZ(${h / 2}px)`, fw: w, fh: d },
    { name: 'bottom', t: `rotateX(-90deg) translateZ(${h / 2}px)`, fw: w, fh: d },
  ]
  return (
    <div className="car3d-box" style={{ transform: `translate3d(${x}px, ${y}px, ${z}px)` }}>
      {sides.map((s) => (
        <div
          key={s.name}
          className={`car3d-face ${cls}${face?.[s.name] ? ' ' + face[s.name] : ''}`}
          style={{ width: s.fw, height: s.fh, marginLeft: -s.fw / 2, marginTop: -s.fh / 2, transform: s.t }}
        />
      ))}
    </div>
  )
}

/** 地面上的刻度（0° = 车头方向，90° = 车左，逆时针为正 —— 与"+ 偏航 = 左转"一致） */
const TICKS: { label: string; t: string; lt: string }[] = [
  { label: '0°', t: 'translate3d(0px, 0px, 116px) rotateX(90deg)', lt: 'translate3d(0px, 0px, 146px) rotateX(90deg)' },
  { label: '90°', t: 'translate3d(116px, 0px, 0px) rotateX(90deg) rotateZ(90deg)', lt: 'translate3d(146px, 0px, 0px) rotateX(90deg)' },
  { label: '180°', t: 'translate3d(0px, 0px, -116px) rotateX(90deg)', lt: 'translate3d(0px, 0px, -146px) rotateX(90deg)' },
  { label: '270°', t: 'translate3d(-116px, 0px, 0px) rotateX(90deg) rotateZ(90deg)', lt: 'translate3d(-146px, 0px, 0px) rotateX(90deg)' },
]

/* 履带尺寸：长 210 > 车体 150，前后各长出一大截（这是"履带车"最认得出来的特征）。
   履带底面正好落在 y=0 地面上（和旧版四个轮子一样），顶上压履带齿。 */
const TRACK_X = 45
const TRACK_W = 24
const TRACK_H = 34
const TRACK_D = 210
const CLEAT_Y = -36.2
/** 履带齿沿 z 的位置（左右各压 5 条） */
const CLEAT_Z = [-84, -42, 0, 42, 84]

export function Car3D({
  yaw,
  pitch,
  roll,
  height = 246,
}: {
  yaw: number | null | undefined
  pitch: number | null | undefined
  roll: number | null | undefined
  height?: number
}) {
  const [view, setView] = React.useState<ViewAngle>(DEFAULT_VIEW)
  const [dragging, setDragging] = React.useState(false)
  const last = React.useRef<{ x: number; y: number } | null>(null)

  const down = (e: React.PointerEvent<HTMLDivElement>) => {
    last.current = { x: e.clientX, y: e.clientY }
    setDragging(true)
    e.currentTarget.setPointerCapture?.(e.pointerId)
  }
  const move = (e: React.PointerEvent<HTMLDivElement>) => {
    const p = last.current
    if (!p) return
    const dx = e.clientX - p.x
    const dy = e.clientY - p.y
    last.current = { x: e.clientX, y: e.clientY }
    setView((v) => orbitView(v, dx, dy))
  }
  const up = () => {
    last.current = null
    setDragging(false)
  }

  return (
    <div
      className={`car3d-scene${dragging ? ' car3d-dragging' : ''}`}
      style={{ height }}
      onPointerDown={down}
      onPointerMove={move}
      onPointerUp={up}
      onPointerCancel={up}
      onDoubleClick={() => setView(DEFAULT_VIEW)}
      title="拖动转视角，双击回默认视角"
    >
      <div className="car3d-stage" style={{ transform: stageTransform(view) }}>
        {/* 地面（绕 X 转 90° 铺平）＋ 车底阴影 ＋ 罗盘圈 */}
        <div className="car3d-road" />
        <div className="car3d-shadow" />
        <div className="car3d-ring" />
        {TICKS.map((k) => (
          <React.Fragment key={k.label}>
            <div className="car3d-tick" style={{ transform: k.t }} />
            <div className="car3d-tick-label" style={{ transform: k.lt }}>
              {k.label}
            </div>
          </React.Fragment>
        ))}

        {/* 车：车头 +z、车左 +x。姿态口径 = 固件方框（默认档 M3）：俯仰读数进 rotateZ、
            横滚读数取负进 rotateX；+偏航仍是左转。见 lib/attitude3d.ts 文件头。 */}
        <div className="car3d-car" style={{ transform: carTransform({ yaw: yaw ?? 0, pitch: pitch ?? 0, roll: roll ?? 0 }) }}>
          {/* 左右两条长履带：外侧面 car3d-tread-x、顶面 car3d-tread-y 是履带纹 */}
          <Box
            w={TRACK_W}
            h={TRACK_H}
            d={TRACK_D}
            x={-TRACK_X}
            y={-17}
            cls="car3d-track"
            face={{ top: 'car3d-tread-y', left: 'car3d-tread-x' }}
          />
          <Box
            w={TRACK_W}
            h={TRACK_H}
            d={TRACK_D}
            x={TRACK_X}
            y={-17}
            cls="car3d-track"
            face={{ top: 'car3d-tread-y', right: 'car3d-tread-x' }}
          />
          {/* 履带齿：比履带略宽（28 > 24）地横压在履带顶上 */}
          {CLEAT_Z.map((z) => (
            <React.Fragment key={z}>
              <Box w={28} h={3.5} d={12} x={-TRACK_X} y={CLEAT_Y} z={z} cls="car3d-cleat" />
              <Box w={28} h={3.5} d={12} x={TRACK_X} y={CLEAT_Y} z={z} cls="car3d-cleat" />
            </React.Fragment>
          ))}
          {/* 底盘：夹在两条履带中间（60 宽 < 履带内沿 66），上面再扣一层甲板 */}
          <Box w={60} h={26} d={150} y={-33} cls="car3d-body" />
          <Box w={52} h={10} d={84} y={-51} z={-6} cls="car3d-deck" />
          {/* 顶上的 OLED 小屏（顶面发光） */}
          <Box w={30} h={2} d={22} y={-57.2} z={-6} cls="car3d-screen" face={{ top: 'car3d-screen-glow' }} />
          {/* 车头：灰度探头排（正面 8 格亮暗）＋ 摄像头 ＋ 两个前灯 */}
          <Box w={48} h={7} d={5} y={-33} z={77.5} cls="car3d-sensor" face={{ front: 'car3d-sensor-face' }} />
          <Box w={9} h={9} d={5} y={-41.5} z={78} cls="car3d-lens" />
          <Box w={9} h={5} d={3} x={-22} y={-26} z={76.5} cls="car3d-lamp" />
          <Box w={9} h={5} d={3} x={22} y={-26} z={76.5} cls="car3d-lamp" />
          {/* 车尾两个尾灯 */}
          <Box w={9} h={5} d={3} x={-22} y={-28} z={-76.5} cls="car3d-tail" />
          <Box w={9} h={5} d={3} x={22} y={-28} z={-76.5} cls="car3d-tail" />
          {/* 车顶天线（甲板左后方）＋ 顶上的小圆头 */}
          <Box w={2.5} h={32} d={2.5} x={-22} y={-72} z={-34} cls="car3d-antenna" />
          <Box w={5} h={5} d={5} x={-22} y={-90.5} z={-34} cls="car3d-antenna-tip" />
        </div>
      </div>

      <div className="car3d-hint">拖动转视角 · 双击复位 · 0° 车头 / 90° 车左（+ 偏航 = 左转）</div>
    </div>
  )
}
