/**
 * IMU 页 3D 车模型的数学部分：纯函数，能单测（这才是"模型对不对"唯一能自证的地方，
 * 本机跑不了浏览器截图）。DOM 那边只管拼盒子，见 components/Car3D.tsx。
 *
 * 坐标系用 CSS 的约定：x 向右、y 向下、z 朝屏幕外；车头 = +z、车的左侧 = +x。
 *
 * 姿态口径**跟固件 OLED 屏上那个方框一致**（用户 m01390：「调整得跟固件里那个正方体一致
 * 就可以」）。固件画方框的映射（firmware/App/Src/app_ui.c，默认轴档 M3）：
 *   app_ui.c:179  `static uint8_t s_gyro_axis_mode = 2U;`          // 默认 M3 = bit1
 *   app_ui.c:1417-1434 gyro_view_angles()：bit0=0 不换轴、bit1=1 横滚取反
 *         a = g_live.pitch_x10; b = g_live.roll_x10;   // bit0 = 0：横滚取 SP、俯仰取 SR
 *         if ((s_gyro_axis_mode & 2U) != 0U) a = -a;   // bit1 = 1：画面横滚取反
 *         *r = a; *p = b;                              // ⇒ 画面横滚 r = -pitch、画面俯仰 p = +roll
 *   方框的旋转是 app_ui.c:1449-1457 的 `M = Rz(y)·Ry(p)·Rx(r)`（y = +yaw），
 *   投影 app_ui.c:1378-1379 只取旋转后的 X/Y：屏幕右 = 车体 +X（车头）、屏幕上 = 车体 +Y（车左），
 *   观察者在 +Z ⇒ **画面横滚 r 绕车头轴、画面俯仰 p 绕车左轴**。
 * 所以固件把遥测里的 pitch 喂给了横滚轴、roll 喂给了俯仰轴（= 传感器在车上装歪 90°那件事），
 * 并且横滚取反：r = -telem.pitch、p = +telem.roll、y = +telem.yaw。
 *
 * 换到 CSS 左手系（x 右、y 下、z 朝屏幕外）要一个个按右手定则算，别凭"屏幕右 = 车的右"想
 * （那样会正好镜像）：
 *   CSS rotateY(ψ) = 绕车竖直轴转 +ψ  ⇒ 左转   = rotateY(+yaw)   （与固件的 y 同号）
 *   CSS rotateX(φ) = 绕车左轴转 -φ    ⇒ 抬头   = rotateX(+φ)
 *   CSS rotateZ(θ) = 绕车头轴转 -θ    ⇒ 向左倾 = rotateZ(+θ)
 * 要复刻 M3 就必须 `-φ = p = +roll`、`-θ = r = -pitch`，于是：
 *   carTransform = rotateY(yaw) rotateX(-roll) rotateZ(+pitch)
 * 注意这是**轴对调 + 符号**（pitch 去 rotateZ、roll 去 rotateX），纯改符号对不上方框。
 * 三个旋转的顺序也跟固件同序（偏航在外、横滚在内），大角度下不会拧成麻花。
 *
 * 这里只定"模型口径"，**没有**动 Imu 页那两个读数：读数仍是遥测原始帧的 pitch/roll
 * （跟波形页同一把尺子）。固件屏上右侧那三行数字走的是另一套取反习惯
 * （app_ui.c:678-683 gyro_angles()：roll=-pitch、pitch=-roll），默认档下它跟方框的
 * 俯仰符号本来就相反（固件接口规格.md §3.5 有记录）。
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
 *
 * 轴映射见文件头：固件默认档 M3 把遥测的 pitch 喂给画面横滚（取反）、roll 喂给画面俯仰，
 * 所以 pitch 进 rotateZ、roll 取负进 rotateX —— 是**轴对调 + 符号**，不是只翻一个符号。
 */
export function carTransform(a: Attitude): string {
  return (
    `rotateY(${round1(a?.yaw).toFixed(1)}deg) ` +
    `rotateX(${(-round1(a?.roll)).toFixed(1)}deg) ` +
    `rotateZ(${round1(a?.pitch).toFixed(1)}deg)`
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
