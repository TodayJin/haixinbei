import { describe, expect, it } from 'vitest'
import {
  bytesToHuman,
  clamp,
  degToRad,
  fmt,
  fmtInt,
  fmtSigned,
  fmtUptime,
  grayToInk,
  mmpsToKmh,
  normDeg,
  pulseToDeg,
  pwmToPct,
  radToDeg,
  relSec,
} from './format'

describe('数字格式化', () => {
  it('clamp', () => {
    expect(clamp(5, 0, 3)).toBe(3)
    expect(clamp(-1, 0, 3)).toBe(0)
    expect(clamp(1.5, 0, 3)).toBe(1.5)
  })

  it('fmt / fmtSigned / fmtInt 对非法值给 --', () => {
    expect(fmt(1.234, 2)).toBe('1.23')
    expect(fmt(Number.NaN)).toBe('--')
    expect(fmt(null)).toBe('--')
    expect(fmtSigned(2.5, 1)).toBe('+2.5')
    expect(fmtSigned(-2.5, 1)).toBe('-2.5')
    expect(fmtSigned(0, 1)).toBe('0.0')
    expect(fmtInt(2.6)).toBe('3')
    expect(fmtInt(undefined)).toBe('--')
  })

  it('fmtUptime', () => {
    expect(fmtUptime(12_000)).toBe('12 秒')
    expect(fmtUptime(3 * 60_000 + 5_000)).toBe('3 分 5 秒')
    expect(fmtUptime(2 * 3_600_000 + 60_000)).toBe('2 时 1 分')
    expect(fmtUptime(90_000_000)).toBe('1 天 1 时')
    expect(fmtUptime(-5)).toBe('--')
  })
})

describe('单位换算', () => {
  it('PWM 刻度 -> 百分比', () => {
    expect(pwmToPct(7200)).toBe(100)
    expect(pwmToPct(-3600)).toBe(-50)
    expect(pwmToPct(99999)).toBe(100) // 夹住
    expect(pwmToPct(7200, 0)).toBe(0)
  })

  it('舵机脉宽 -> 角度（与固件 servo.c 的 500..2500us ↔ 0..180° 一致）', () => {
    expect(pulseToDeg(500)).toBe(0)
    expect(pulseToDeg(1500)).toBe(90)
    expect(pulseToDeg(2500)).toBe(180)
    expect(pulseToDeg(4000)).toBe(180)
  })

  it('角度归一化', () => {
    expect(normDeg(190)).toBe(-170)
    expect(normDeg(-190)).toBe(170)
    expect(normDeg(360)).toBe(0)
    expect(normDeg(Number.NaN)).toBe(0)
  })

  it('弧度/度、灰度黑度、mm/s -> km/h、相对秒', () => {
    expect(degToRad(180)).toBeCloseTo(Math.PI, 10)
    expect(radToDeg(Math.PI)).toBeCloseTo(180, 10)
    expect(grayToInk(255)).toBe(1)
    expect(grayToInk(0)).toBe(0)
    expect(grayToInk(300)).toBe(1)
    expect(mmpsToKmh(1000)).toBeCloseTo(3.6, 10)
    expect(relSec(2500, 1000)).toBe(1.5)
  })

  it('bytesToHuman', () => {
    expect(bytesToHuman(512)).toBe('512 B')
    expect(bytesToHuman(2048)).toBe('2.0 KB')
    expect(bytesToHuman(3 * 1024 * 1024)).toBe('3.00 MB')
  })
})
