/**
 * 遥测通道定义：波形页、CSV 导出、总览页的迷你趋势图都从这里取。
 * 新增通道只改这一个文件（`get` 从 §4 的 telem 里取值）。
 */

import type { Telem } from './types'

export type ChannelGroup = '姿态' | '速度' | '控制' | '循迹' | '舵机' | '电源' | '系统'

export interface ChannelDef {
  key: string
  label: string
  unit: string
  color: string
  decimals: number
  group: ChannelGroup
  get: (t: Telem) => number
}

export const CHANNELS: ChannelDef[] = [
  // ★2026-10-15：画**连续域**航向（t.yawCont，store.onTelem 解卷绕填充）⇒ 不再在 ±180 处瞬跳。
  { key: 'yaw', label: '偏航', unit: '°', color: '#22d3ee', decimals: 1, group: '姿态', get: (t) => t.yawCont ?? t.yaw },
  // 姿态两路：**名字是车体轴，不是遥测帧里的同名字段**。车上那颗 JY61P 装成了横的 ——
  // 传感器 pitch 轴量的是车身横滚、roll 轴量的是车身俯仰（`固件接口规格.md` §3.5 的
  // M1..M8 表，默认档 M3：画面横滚 = −遥测 pitch、画面俯仰 = +遥测 roll）。
  // 这里按 `lib/attitude3d.ts` 的同一套映射取值（控制台 3D 模型 / 车上屏方框口径），
  // 符号不动：+横滚 = 车向左倾、+俯仰 = 低头 —— 图表和 3D 模型必须同口径。
  // 用户 m30822「波形界面的横滚和俯仰是相反的」= 原来直接取了同名字段，两路互换。
  { key: 'pitch', label: '俯仰', unit: '°', color: '#a78bfa', decimals: 1, group: '姿态', get: (t) => t.roll },
  { key: 'roll', label: '横滚', unit: '°', color: '#34d399', decimals: 1, group: '姿态', get: (t) => t.pitch },
  { key: 'vl', label: '左轮速', unit: 'mm/s', color: '#38bdf8', decimals: 0, group: '速度', get: (t) => t.vl },
  { key: 'vr', label: '右轮速', unit: 'mm/s', color: '#f472b6', decimals: 0, group: '速度', get: (t) => t.vr },
  { key: 'pwm_l', label: '左 PWM', unit: '', color: '#fbbf24', decimals: 0, group: '控制', get: (t) => t.pwm_l },
  { key: 'pwm_r', label: '右 PWM', unit: '', color: '#fb7185', decimals: 0, group: '控制', get: (t) => t.pwm_r },
  { key: 'trace_pos', label: '循迹偏差', unit: '', color: '#a3e635', decimals: 2, group: '循迹', get: (t) => t.trace.pos },
  { key: 'trace_pid', label: '循迹 PID 输出', unit: '', color: '#facc15', decimals: 0, group: '循迹', get: (t) => t.trace.pid },
  { key: 'angle_err', label: '角度环误差', unit: '°', color: '#c084fc', decimals: 2, group: '控制', get: (t) => t.angle.err },
  { key: 'rate_ref', label: '期望角速度', unit: '°/s', color: '#2dd4bf', decimals: 1, group: '控制', get: (t) => t.angle.rate_ref },
  { key: 'angle_corr', label: '角度环修正', unit: '', color: '#f0abfc', decimals: 0, group: '控制', get: (t) => t.angle.corr },
  { key: 'servo1', label: '舵机1', unit: '°', color: '#22d3ee', decimals: 1, group: '舵机', get: (t) => t.servo1_deg },
  { key: 'servo2', label: '舵机2', unit: '°', color: '#a78bfa', decimals: 1, group: '舵机', get: (t) => t.servo2_deg },
  { key: 'vbat', label: '电池电压', unit: 'V', color: '#34d399', decimals: 2, group: '电源', get: (t) => t.vbat_mv / 1000 },
  { key: 'ibat', label: '电流', unit: 'mA', color: '#fb7185', decimals: 0, group: '电源', get: (t) => t.ibat_ma },
  { key: 'loop_us', label: '主循环耗时', unit: 'µs', color: '#94a3b8', decimals: 0, group: '系统', get: (t) => t.loop_us },
  { key: 'gray_max', label: '灰度峰值', unit: '', color: '#e2e8f0', decimals: 0, group: '循迹', get: (t) => Math.max(...t.gray) },
]

export const CHANNEL_BY_KEY: Record<string, ChannelDef> = Object.fromEntries(
  CHANNELS.map((c) => [c.key, c]),
)

export const DEFAULT_SCOPE_CHANNELS = ['yaw', 'pitch', 'roll', 'pwm_l', 'pwm_r']

export function channelIndex(key: string): number {
  return CHANNELS.findIndex((c) => c.key === key)
}
