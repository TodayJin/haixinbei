/**
 * 3D 车模型（IMU 页）：纯 CSS 3D，零依赖 —— 没有模型文件，也没有 three.js。
 * 车壳/车顶/四个轮子/灯都是 `Box` 拼出来的长方体（6 个面各一个 div），
 * 姿态换算全在 lib/attitude3d.ts（有单测，本机跑不了浏览器截图，只能靠那个自证）。
 *
 * 坐标（CSS 约定）：x 右、y 下、z 朝屏幕外；车头 = +z、车的左侧 = +x、地面 = y0 平面。
 * 相机 = `.car3d-stage` 上的 `rotateY(az) rotateX(-el)`，拖鼠标/手指改 az/el。
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

        {/* 车：车头 +z、车左 +x；+偏航 = 左转、+俯仰 = 抬头、+横滚 = 向左倾 */}
        <div className="car3d-car" style={{ transform: carTransform({ yaw: yaw ?? 0, pitch: pitch ?? 0, roll: roll ?? 0 }) }}>
          <Box w={86} h={26} d={170} y={-37} cls="car3d-body" />
          <Box w={76} h={20} d={76} y={-60} z={-14} cls="car3d-cabin" />
          <Box w={16} h={2} d={150} y={-51} cls="car3d-stripe" />
          <Box w={14} h={6} d={3} x={-25} y={-41} z={86} cls="car3d-lamp" />
          <Box w={14} h={6} d={3} x={25} y={-41} z={86} cls="car3d-lamp" />
          <Box w={10} h={5} d={3} x={-28} y={-41} z={-86} cls="car3d-tail" />
          <Box w={10} h={5} d={3} x={28} y={-41} z={-86} cls="car3d-tail" />
          <Box w={12} h={32} d={32} x={-49} y={-16} z={55} cls="car3d-tyre" />
          <Box w={12} h={32} d={32} x={49} y={-16} z={55} cls="car3d-tyre" />
          <Box w={12} h={32} d={32} x={-49} y={-16} z={-55} cls="car3d-tyre" />
          <Box w={12} h={32} d={32} x={49} y={-16} z={-55} cls="car3d-tyre" />
        </div>
      </div>

      <div className="car3d-hint">拖动转视角 · 双击复位 · 0° 车头 / 90° 车左（+ 偏航 = 左转）</div>
    </div>
  )
}
