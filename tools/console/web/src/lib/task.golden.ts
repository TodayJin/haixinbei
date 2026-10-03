/**
 * @file  task.golden.ts
 * @brief 「任务程序」金样本 —— **由真固件源码生成**，不是手抄的
 *
 * 生成链：`tools/console/tests/c/taskgen.c`（链真 `firmware/App/Svc/task_prog.c`）
 *         → `tools/console/tests/fixtures/task.golden.json`（pytest 每次重编 C 对拍它）
 *         → 本文件（`task.golden.test.ts` 拿它钉住 `task.ts` 的编码器）
 *
 * 于是 TS 的 `encodeTask()` 与 C 的格式层被**同一个字节串**拴在一起：
 * 谁偷偷改了布局，`test_task_bytes.py` 或 `task.golden.test.ts` 至少红一个。
 * 重新生成：`pwsh -File tools/console/tests/c/make_golden.ps1`（**不要手改本文件**）
 */

import { NO_STATE } from './task';

export interface GoldenBlock {
  op: string;
  flags: number;
  p0: number;
  p1: number;
  p2: number;
  p3: number;
}

export interface GoldenState {
  name: string;
  enter: GoldenBlock[];
  body: GoldenBlock[];
}

export const GOLDEN_LEN = 208;
export const GOLDEN_CRC16 = 0x023a;
export const GOLDEN_MAIN = 0;
export const GOLDEN_HEX =
  '544B010300003A02D0000000B00024002E000104BC00560060000104C7000000' +
  '880000040200030000000000000001005019000000000000040003000000F401' +
  '0000060004000000B80B0100070000000000000000000500C800000000000000' +
  '0301D007A6FF0A0000000A000000000000000000060006000A000000FFFF0700' +
  '010000000000000009000100B400000000000900020000000000000006000900' +
  '0000F401FFFF08000000000000000000E5B8B8E68081E5BEAAE8BFB9E8B7AFE5' +
  '8FA3E8BDAC3930E69186E888B5E69CBA';

export const GOLDEN_STATES: GoldenState[] = [
  {
    name: "常态循迹",
    enter: [
      { op: 'SET_MODE', flags: 0, p0: 3, p1: 0, p2: 0, p3: 0 },
    ],
    body: [
      { op: 'SET_SPEED', flags: 0, p0: 6480, p1: 0, p2: 0, p3: 0 },
      { op: 'ACTION', flags: 0, p0: 3, p1: 0, p2: 500, p3: 0 },
      { op: 'WAIT_COND', flags: 0, p0: 4, p1: 0, p2: 3000, p3: 1 },
      { op: 'GOTO', flags: 0, p0: 0, p1: 0, p2: 0, p3: 0 },
    ],
  },
  {
    name: "路口转90",
    enter: [
      { op: 'WAIT_MS', flags: 0, p0: 200, p1: 0, p2: 0, p3: 0 },
    ],
    body: [
      { op: 'TURN_DEG', flags: 1, p0: 2000, p1: -90, p2: 10, p3: 0 },
      { op: 'CROSS_CLEAR', flags: 0, p0: 0, p1: 0, p2: 0, p3: 0 },
      { op: 'WAIT_COND', flags: 0, p0: 6, p1: 10, p2: 0, p3: NO_STATE },
      { op: 'GOTO', flags: 0, p0: 1, p1: 0, p2: 0, p3: 0 },
    ],
  },
  {
    name: "摆舵机",
    enter: [
    ],
    body: [
      { op: 'SET_SERVO', flags: 0, p0: 1, p1: 180, p2: 0, p3: 0 },
      { op: 'SET_SERVO', flags: 0, p0: 2, p1: 0, p2: 0, p3: 0 },
      { op: 'WAIT_COND', flags: 0, p0: 9, p1: 0, p2: 500, p3: NO_STATE },
      { op: 'END', flags: 0, p0: 0, p1: 0, p2: 0, p3: 0 },
    ],
  },
];

/** 金样本场景的状态名（与 C 侧 g_name[] 一字不差） */
export const GOLDEN_NAMES: string[] = ["常态循迹", "路口转90", "摆舵机"];
