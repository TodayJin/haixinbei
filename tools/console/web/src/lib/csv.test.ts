import { describe, expect, it } from 'vitest'
import { csvCell, csvFileName, csvRow, snapshotToCsv } from './csv'
import { SeriesRing } from './ring'

describe('csvCell / csvRow', () => {
  it('转义逗号、引号、换行', () => {
    expect(csvCell('a,b')).toBe('"a,b"')
    expect(csvCell('说"你好"')).toBe('"说""你好"""')
    expect(csvCell('第一行\n第二行')).toBe('"第一行\n第二行"')
    expect(csvCell('普通')).toBe('普通')
  })

  it('数字按 decimals 格式化，NaN/Inf 变空', () => {
    expect(csvCell(1.23456, 2)).toBe('1.23')
    expect(csvCell(Number.NaN)).toBe('')
    expect(csvCell(Number.POSITIVE_INFINITY)).toBe('')
    expect(csvCell(null)).toBe('')
    expect(csvCell(undefined)).toBe('')
  })

  it('行用逗号连接', () => {
    expect(csvRow([1, 'a', 2.5])).toBe('1,a,2.5')
  })
})

describe('snapshotToCsv', () => {
  it('首列是 t_ms/t_s，通道列按 columns 取', () => {
    const r = new SeriesRing(8, 2)
    r.push(1000, [10, 20])
    r.push(1500, [11, 21])
    const csv = snapshotToCsv(r.all(), [
      { header: 'yaw', field: 0, decimals: 1 },
      { header: 'pwm', field: 1 },
    ])
    const lines = csv.trim().split('\r\n')
    expect(lines[0]).toBe('t_ms,t_s,yaw,pwm')
    expect(lines[1]).toBe('1000,0.000,10.0,20')
    expect(lines[2]).toBe('1500,0.500,11.0,21')
  })

  it('空快照只要表头', () => {
    const r = new SeriesRing(4, 1)
    const csv = snapshotToCsv(r.all(), [{ header: 'x', field: 0 }])
    expect(csv.trim().split('\r\n')).toEqual(['t_ms,t_s,x'])
  })
})

describe('csvFileName', () => {
  it('带零填充的时间戳', () => {
    const name = csvFileName('telemetry', new Date(2026, 9, 3, 1, 26, 7).getTime())
    expect(name).toBe('telemetry_20261003_012607.csv')
  })
})
