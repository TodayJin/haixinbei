/**
 * @file  battery.test.ts
 * @brief 电池表：锚点行为 + **与固件 C 源码逐值对拍**（防止两边各改各的）
 *
 * 关键的一条是 `解析 firmware/App/Src/app_ui.c 的 kBv/kBp`：上位机的百分比必须与
 * OLED 上那块电池的数字同源。谁单边改了电压轴（比如把满电锚点又挪回去），这里就红。
 */
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { BAT_EMPTY_MV, BAT_FULL_MV, BAT_PCT, BAT_V_MV, batteryPct } from './battery'

const APP_UI_C = fileURLToPath(new URL('../../../../../firmware/App/Src/app_ui.c', import.meta.url))

/** 从 C 源码里抠出一个 `static const uintX_t kXxx[N] = { ... };` 的整数列表 */
function parseTable(src: string, name: string): number[] {
  const re = new RegExp(`static const\\s+\\w+\\s+${name}\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([^}]*)\\}`, 's')
  const m = re.exec(src)
  if (!m) throw new Error(`在 app_ui.c 里找不到 ${name} 表`)
  return (m[1].match(/-?\d+/g) ?? []).map(Number)
}

describe('电池电压 → 百分比', () => {
  it('表本身自洽：长度相同、电压严格递减、百分比从 100 到 0', () => {
    expect(BAT_V_MV).toHaveLength(21)
    expect(BAT_PCT).toHaveLength(21)
    for (let i = 1; i < BAT_V_MV.length; i++) expect(BAT_V_MV[i]).toBeLessThan(BAT_V_MV[i - 1])
    expect(BAT_PCT[0]).toBe(100)
    expect(BAT_PCT[BAT_PCT.length - 1]).toBe(0)
    for (let i = 1; i < BAT_PCT.length; i++) expect(BAT_PCT[i]).toBeLessThan(BAT_PCT[i - 1])
  })

  it('锚点行为：≥12.2V 满电、表尾 9.0V 空、更低还是空', () => {
    expect(BAT_FULL_MV).toBe(12200)
    expect(BAT_EMPTY_MV).toBe(9000)
    expect(batteryPct(12200)).toBe(100)
    expect(batteryPct(12600)).toBe(100)
    expect(batteryPct(13000)).toBe(100)
    expect(batteryPct(12111)).toBe(96)
    /* 2026-10-04 真车实测（12V 电源接着车）：固件 RAM 里 s_bat_mv=12094 → s_bat_pct=95，
     * 这里的 95 是同一张表算出来的同一个数（旧表在这一电压上给 80%） */
    expect(batteryPct(12094)).toBe(95)
    expect(batteryPct(9000)).toBe(0)
    expect(batteryPct(8999)).toBe(0)
    expect(batteryPct(0)).toBe(0)
    expect(batteryPct(null)).toBeNull()
    expect(batteryPct(undefined)).toBeNull()
    expect(batteryPct(Number.NaN)).toBeNull()
  })

  it('旧表在 12.1V 处只给 82%，新表给 90% 以上（用户那句「满电却像掉了两成」）', () => {
    const oldV = [12600, 12450, 12330, 12240, 12060, 11940, 11850, 11730, 11610, 11460, 11340,
      11220, 11100, 10980, 10860, 10740, 10620, 10470, 10260, 9900, 9000]
    const lerp = (v: number, V: number[]): number => {
      if (v >= V[0]) return 100
      for (let i = 1; i < V.length; i++) {
        if (v >= V[i]) return BAT_PCT[i] + Math.trunc(((BAT_PCT[i - 1] - BAT_PCT[i]) * (v - V[i])) / (V[i - 1] - V[i]))
      }
      return 0
    }
    expect(lerp(12111, oldV)).toBe(81)
    expect(batteryPct(12111)!).toBeGreaterThanOrEqual(90)
  })

  it('全程单调不增（8.5V 到 12.8V 每 10mV 扫一遍）', () => {
    let prev = 100
    for (let mv = 12800; mv >= 8500; mv -= 10) {
      const pct = batteryPct(mv)!
      expect(pct).toBeLessThanOrEqual(prev)
      expect(pct).toBeGreaterThanOrEqual(0)
      expect(pct).toBeLessThanOrEqual(100)
      prev = pct
    }
    expect(prev).toBe(0)
  })

  it('与固件源码里的 kBv / kBp 逐值一致（谁单边改了表就红）', () => {
    const src = readFileSync(APP_UI_C, 'utf8')
    expect(parseTable(src, 'kBv')).toEqual([...BAT_V_MV])
    expect(parseTable(src, 'kBp')).toEqual([...BAT_PCT])
  })

  it('固件那两行注释里写的重锚公式确实能算出我们的表', () => {
    const src = readFileSync(APP_UI_C, 'utf8')
    expect(src).toContain('用户 m03190')
    const old = [12600, 12450, 12330, 12240, 12060, 11940, 11850, 11730, 11610, 11460, 11340,
      11220, 11100, 10980, 10860, 10740, 10620, 10470, 10260, 9900, 9000]
    // v_new = 9000 + (v_old - 9000) * 3200 / 3600，四舍五入到 10mV
    const want = old.map((v) => 9000 + Math.round(((v - 9000) * 3200) / 3600 / 10) * 10)
    expect(want).toEqual([...BAT_V_MV])
  })
})
