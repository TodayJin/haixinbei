/**
 * IMU 页 3D 车模型的数学部分：纯函数，能单测（这才是"模型对不对"唯一能自证的地方，
 * 本机跑不了浏览器截图）。DOM 那边只管拼盒子，见 components/Car3D.tsx。
 *
 * 坐标系用 CSS 的约定：x 向右、y 向下、z 朝屏幕外。模型姿态的正方向跟固件的
 * 「用户约定」对齐（firmware/App/Src/app_ui.c:666）：
 *   +偏航 yaw   = 车头向左转（偏航角增大 = 向左转）
 *   +俯仰 pitch = 车头抬起（抬头）
 *   +横滚 roll  = 车身向左倾（+x 那一侧 = 车的左侧，下沉）
 *
 * 车的左右怎么定的：车头朝 +z（默认视角下正对观察者）时，车的左侧在 +x
 * （迎面看别人的车，车的左手边在你的右手边）。所以：
 *   左转 = 车头从 +z 转向 +x ⇒ CSS rotateY(+yaw)（绕 +y 转的矩阵把 (0,0,1) 送到 (sin,0,cos)）
 *   左倾 = +x 侧下沉        ⇒ CSS rotateZ(+roll)
 *   抬头 = 车头抬起来        ⇒ CSS rotateX(+pitch)
 * 三个符号只在 `carTransform()` 里出现一次：真车上发现哪个方向反了，改这里一行。
 *
 * 注意遥测里的 pitch/roll 是 JY61P 的**原始帧**，固件给 OLED 画图时另做了一次轴序交换 +
 * 取反（app_ui.c 的 s_gyro_axis_mode 八档，默认 M3，那是"传感器在车上怎么装的"的问题）。
 * 上位机这一页不做那个交换：模型和读数用同一把尺子，才不会出现"数字说横滚、车却在点头"。
 */

export interface Attitude {
  yaw: number
  pitch: number
  roll: number
}

export interface ViewAngle {
  /** 方位角（度）：0 = 从车头正面看，正 = 相机往车的左侧转 */
  az: number
  /** 俯角（度）：0 = 平视，越大越像俯视图 */
  el: number
}

export const DEFAULT_VIEW: ViewAngle = { az: 26, el: 30 }

/** 俯角夹取范围：别让相机钻进地面（0°）或正好在正上方（90°，那样偏航就看不出来了） */
export const EL_MIN = 6
export const EL_MAX = 78

/** 拖动 1 像素转多少度 */
export const DRAG_AZ_PER_PX = 0.35
export const DRAG_EL_PER_PX = 0.3

/** 非数字（NaN/Infinity/undefined）一律当 0：遥测断流时模型别散架 */
const num = (v: number | null | undefined): number =>
  typeof v === 'number' && Number.isFinite(v) ? v : 0

/** 取到 0.1°：每帧拼 transform 字符串时不带浮点噪音 */
export function round1(v: number | null | undefined): number {
  return Math.round(num(v) * 10) / 10
}

export function clampView(v: ViewAngle): ViewAngle {
  const az = num(v?.az)
  const el = num(v?.el)
  return {
    az: Math.max(-180, Math.min(180, az)),
    el: Math.max(EL_MIN, Math.min(EL_MAX, el)),
  }
}

/** 拖动增量（像素，向右/向下为正）-> 新视角 */
export function orbitView(v: ViewAngle, dxPx: number, dyPx: number): ViewAngle {
  const c = clampView(v)
  return clampView({
    az: c.az + num(dxPx) * DRAG_AZ_PER_PX,
    el: c.el + num(dyPx) * DRAG_EL_PER_PX,
  })
}

/** 相机：先绕世界竖直轴转 az，再按 el 俯视（负角 = 从上方看下去） */
export function stageTransform(v: ViewAngle): string {
  const c = clampView(v)
  return `rotateY(${c.az.toFixed(2)}deg) rotateX(${(-c.el).toFixed(2)}deg)`
}

/**
 * 车姿态 -> CSS 变换。顺序和固件三维方框的 `R = Rz(y) Ry(p) Rx(r)` 同序：
 * 偏航在最外层（先横滚、再俯仰、最后偏航），大角度下车才不会拧成麻花。
 */
export function carTransform(a: Attitude): string {
  return (
    `rotateY(${round1(a?.yaw).toFixed(1)}deg) ` +
    `rotateX(${round1(a?.pitch).toFixed(1)}deg) ` +
    `rotateZ(${round1(a?.roll).toFixed(1)}deg)`
  )
}

/** 罗盘角度：负角绕回 0..360，读数不出现 -350° 这种 */
export function compassDeg(yaw: number | null | undefined): number {
  const d = num(yaw) % 360
  return d < 0 ? d + 360 : d
}

export function compassText(yaw: number | null | undefined, digits = 1): string {
  return `${compassDeg(yaw).toFixed(digits)}°`
}
