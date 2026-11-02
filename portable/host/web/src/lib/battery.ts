/**
 * @file  battery.ts
 * @brief 3S 锂电电压 → 剩余电量百分比（**与固件同一张表**）
 *
 * 表是 `firmware/App/Src/app_ui.c` 里的 `kBv[21] / kBp[21]`（OLED 主页那块电池用的
 * 就是它，`bat_pct_from_mv()` 在线性插值）。用户 m03190 的口径：这块电池包充满只有
 * 12.2V 左右，所以电压轴是公开 3S 曲线 [12.6, 9.0] 线性压到 **[12.2, 9.0]**：
 * ≥12.2V 一律 100%，9.0V（3.0V/节）是 0%。
 * 两边不许各改各的：`battery.test.ts` 会解析那份 C 源码逐值比对。
 */

/** 电压锚点（毫伏），严格递减 */
export const BAT_V_MV: readonly number[] = [
  12200, 12070, 11960, 11880, 11720, 11610, 11530, 11430, 11320, 11190, 11080,
  10970, 10870, 10760, 10650, 10550, 10440, 10310, 10120, 9800, 9000,
]

/** 对应的剩余量（%），与 {@link BAT_V_MV} 一一对应 */
export const BAT_PCT: readonly number[] = [
  100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50,
  45, 40, 35, 30, 25, 20, 15, 10, 5, 0,
]

/** 满电锚点（毫伏）：≥ 它一律 100% —— 用户说的「超过 12.2 基本就是满电」 */
export const BAT_FULL_MV = BAT_V_MV[0]

/** 空电锚点（毫伏）：< 它一律 0% */
export const BAT_EMPTY_MV = BAT_V_MV[BAT_V_MV.length - 1]

/**
 * 电压（毫伏）→ 剩余量（0..100）。与固件 `bat_pct_from_mv()` 同算法：
 * 表外钳两端，表内逐段线性插值、整数除法向零取整。
 * 没有遥测（null / NaN）时返回 `null`，让界面自己决定显示什么。
 */
export function batteryPct(vbat_mv: number | null | undefined): number | null {
  if (vbat_mv === null || vbat_mv === undefined || !Number.isFinite(vbat_mv)) return null
  if (vbat_mv >= BAT_FULL_MV) return 100
  for (let i = 1; i < BAT_V_MV.length; i++) {
    if (vbat_mv >= BAT_V_MV[i]) {
      const span = BAT_V_MV[i - 1] - BAT_V_MV[i]
      const dp = BAT_PCT[i - 1] - BAT_PCT[i]
      return BAT_PCT[i] + Math.trunc((dp * (vbat_mv - BAT_V_MV[i])) / span)
    }
  }
  return 0
}
