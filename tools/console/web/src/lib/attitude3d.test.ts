/**
 * 3D 车模型口径的单测（`lib/attitude3d.ts`）。
 *
 * 本机跑不了浏览器，3D 长什么样只能靠用户肉眼；能自证的只有这里：
 * 把"哪个方向是正"钉死成字符串断言，符号写反了测试就红。
 *   1. 车的左侧 = +x（车头朝 +z 时）⇒ **左转 = rotateY(正)**、**左倾 = rotateZ(正)**、
 *      抬头 = rotateX(正)。这三个符号写反过（当初按"屏幕右 = 车的右"想，正好是镜像的）。
 *   2. 方位角进 rotateY、俯角以**负角**进 rotateX（负角才是从上方看下去）。
 *   3. 遥测断流（NaN/undefined）时模型不许散架，一律当 0。
 */
import { describe, expect, it } from 'vitest'
import {
  DEFAULT_VIEW,
  DRAG_AZ_PER_PX,
  DRAG_EL_PER_PX,
  EL_MAX,
  EL_MIN,
  carTransform,
  clampView,
  compassDeg,
  compassText,
  orbitView,
  round1,
  stageTransform,
} from './attitude3d'

const att = (o: Partial<{ yaw: number; pitch: number; roll: number }> = {}) => ({
  yaw: 0,
  pitch: 0,
  roll: 0,
  ...o,
})

describe('车姿态 -> CSS 变换', () => {
  it('零姿态就是三个零旋转', () => {
    expect(carTransform(att())).toBe('rotateY(0.0deg) rotateX(0.0deg) rotateZ(0.0deg)')
  })

  it('+偏航 = 左转 = rotateY 取正（车的左侧在 +x）', () => {
    expect(carTransform(att({ yaw: 90 }))).toBe('rotateY(90.0deg) rotateX(0.0deg) rotateZ(0.0deg)')
    // 右转（偏航减小）就是负角
    expect(carTransform(att({ yaw: -45 }))).toContain('rotateY(-45.0deg)')
  })

  it('+俯仰 = 抬头 = rotateX 取正；+横滚 = 向左倾 = rotateZ 取正', () => {
    expect(carTransform(att({ pitch: 12.34 }))).toContain('rotateX(12.3deg)')
    expect(carTransform(att({ roll: -8 }))).toContain('rotateZ(-8.0deg)')
    expect(carTransform(att({ yaw: 10, pitch: 5, roll: 3 }))).toBe(
      'rotateY(10.0deg) rotateX(5.0deg) rotateZ(3.0deg)',
    )
  })

  it('偏航在最外层（先横滚、再俯仰、最后偏航），大角度也不会拧成麻花', () => {
    // 变换列表从左到右 = 矩阵从外到内，所以偏航必须排第一
    expect(carTransform(att({ yaw: 170, pitch: 40, roll: -25 }))).toBe(
      'rotateY(170.0deg) rotateX(40.0deg) rotateZ(-25.0deg)',
    )
  })

  it('遥测断流（NaN/undefined）当 0，字符串永远是三个旋转', () => {
    const s = carTransform({ yaw: NaN, pitch: undefined as unknown as number, roll: Infinity })
    expect(s).toBe('rotateY(0.0deg) rotateX(0.0deg) rotateZ(0.0deg)')
    expect(s.match(/rotate[XYZ]\(/g)).toHaveLength(3)
  })

  it('round1 只保留 0.1°', () => {
    expect(round1(12.34)).toBe(12.3)
    expect(round1(-0.04)).toBe(-0)
    expect(round1(NaN)).toBe(0)
  })
})

describe('相机视角', () => {
  it('方位角进 rotateY，俯角以负角进 rotateX', () => {
    expect(stageTransform({ az: 26, el: 30 })).toBe('rotateY(26.00deg) rotateX(-30.00deg)')
  })

  it('俯角夹在 6..78（别钻进地面、也别正好在正上方）', () => {
    expect(clampView({ az: 0, el: -40 }).el).toBe(EL_MIN)
    expect(clampView({ az: 0, el: 200 }).el).toBe(EL_MAX)
    expect(clampView({ az: 400, el: 30 }).az).toBe(180)
    expect(clampView({ az: -400, el: 30 }).az).toBe(-180)
    expect(clampView({ az: NaN, el: NaN })).toEqual({ az: 0, el: EL_MIN })
    expect(stageTransform({ az: 0, el: 999 })).toBe(`rotateY(0.00deg) rotateX(${(-EL_MAX).toFixed(2)}deg)`)
  })

  it('拖动：向右/向下为正，按像素换算', () => {
    expect(orbitView(DEFAULT_VIEW, 0, 0)).toEqual(DEFAULT_VIEW)
    expect(orbitView(DEFAULT_VIEW, 100, 0).az).toBeCloseTo(DEFAULT_VIEW.az + 100 * DRAG_AZ_PER_PX, 6)
    expect(orbitView(DEFAULT_VIEW, 0, 100).el).toBeCloseTo(DEFAULT_VIEW.el + 100 * DRAG_EL_PER_PX, 6)
    // 拖到极限就停在边界，不越界
    expect(orbitView(DEFAULT_VIEW, 10000, 10000)).toEqual({ az: 180, el: EL_MAX })
    expect(orbitView(DEFAULT_VIEW, -10000, -10000)).toEqual({ az: -180, el: EL_MIN })
    // 视角参数本身是 NaN 时按默认俯角兜底，不返回 NaN
    expect(orbitView({ az: NaN, el: NaN }, 10, 10)).toEqual({ az: 10 * DRAG_AZ_PER_PX, el: EL_MIN + 10 * DRAG_EL_PER_PX })
  })
})

describe('罗盘读数', () => {
  it('负角绕回 0..360', () => {
    expect(compassDeg(-90)).toBe(270)
    expect(compassDeg(450)).toBe(90)
    expect(compassDeg(-350)).toBe(10)
    expect(compassText(0)).toBe('0.0°')
    expect(compassText(-0.25)).toBe('359.8°')
    expect(compassText(NaN)).toBe('0.0°')
  })
})
