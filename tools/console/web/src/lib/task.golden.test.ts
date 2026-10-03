/**
 * @file  task.golden.test.ts
 * @brief 用**真固件源码生成的**金样本钉住 `task.ts` 的编码器（跨语言字节对拍）
 *
 * 这条用例不依赖任何人手抄的十六进制：`task.golden.ts` 里的字节是
 * `tools/console/tests/c/taskgen.c` 链上真 `firmware/App/Svc/task_prog.c` 编出来的，
 * 而 `tools/console/tests/test_task_bytes.py` 每次都会重编那个 C 程序、确认金样本没被改。
 * 于是 TS 的 `encodeTask()` 与 C 的格式层被同一个字节串拴住：
 * 谁改了布局（字段顺序、偏移算错、CRC 少算一个字节），至少有一条会红。
 */
import { describe, expect, it } from 'vitest';

import {
  COND,
  MODE,
  NO_STATE,
  OP,
  decodeTask,
  encodeTask,
  hexOf,
  validateTask,
  type TaskBlock,
  type TaskProgram,
} from './task';
import { GOLDEN_CRC16, GOLDEN_HEX, GOLDEN_LEN, GOLDEN_MAIN, GOLDEN_NAMES, GOLDEN_STATES } from './task.golden';

const OP_BY_NAME: Record<string, number> = {
  SET_SPEED: OP.SET_SPEED,
  SET_MODE: OP.SET_MODE,
  TURN_DEG: OP.TURN_DEG,
  ACTION: OP.ACTION,
  WAIT_MS: OP.WAIT_MS,
  WAIT_COND: OP.WAIT_COND,
  GOTO: OP.GOTO,
  END: OP.END,
  SET_SERVO: OP.SET_SERVO,
  CROSS_CLEAR: OP.CROSS_CLEAR,
};

/** 金样本场景 → `TaskProgram`（字段**显式**给全，不用 `blk()` 的 p3 缺省，才能与 C 逐字节比） */
export function goldenProgram(): TaskProgram {
  const blocks = (list: typeof GOLDEN_STATES[number]['body']): TaskBlock[] =>
    list.map((b) => ({
      op: OP_BY_NAME[b.op],
      flags: b.flags,
      p0: b.p0,
      p1: b.p1,
      p2: b.p2,
      p3: b.p3,
    }));
  return {
    main: GOLDEN_MAIN,
    states: GOLDEN_STATES.map((s) => ({ name: s.name, enter: blocks(s.enter), body: blocks(s.body) })),
  };
}

describe('task.ts 与真固件格式层逐字节对齐', () => {
  it('金样本自己先是自洽的（长度/CRC/头部）', () => {
    const bytes = Uint8Array.from(
      (GOLDEN_HEX.match(/../g) ?? []).map((h) => Number.parseInt(h, 16)),
    );
    expect(bytes.length).toBe(GOLDEN_LEN);
    expect(String.fromCharCode(bytes[0], bytes[1])).toBe('TK');
    expect(bytes[2]).toBe(1);
    expect(bytes[3]).toBe(GOLDEN_STATES.length);
    expect(bytes[4]).toBe(GOLDEN_MAIN);
    expect(GOLDEN_HEX).toHaveLength(GOLDEN_LEN * 2);
  });

  it('encodeTask(golden) === 真 C 编出来的字节', () => {
    const { bytes, errors } = encodeTask(goldenProgram());
    expect(errors).toEqual([]);
    expect(bytes.length).toBe(GOLDEN_LEN);
    expect(hexOf(bytes).toUpperCase()).toBe(GOLDEN_HEX);
  });

  it('CRC16 覆盖头部之后的全部字节（与 C 的 TaskProg_Crc16 同段同值）', () => {
    const { bytes } = encodeTask(goldenProgram());
    const crc = (bytes[6] | (bytes[7] << 8)) & 0xffff;
    expect(crc).toBe(GOLDEN_CRC16);
    // 改了覆盖区里的任何一个字节，CRC 就不一样
    const tampered = Uint8Array.from(bytes);
    tampered[GOLDEN_LEN - 1] ^= 0x01;
    expect((tampered[6] | (tampered[7] << 8)) & 0xffff).toBe(GOLDEN_CRC16);
    expect(decodeTask(tampered).error).toBe('CRC 不对');
  });

  it('回读（decodeTask）还原出同一份程序：名字、块、字段', () => {
    const { bytes } = encodeTask(goldenProgram());
    const { program, error } = decodeTask(bytes);
    expect(error).toBe('');
    expect(program).not.toBeNull();
    const p = program as TaskProgram;
    expect(p.main).toBe(GOLDEN_MAIN);
    expect(p.states.map((s) => s.name)).toEqual(GOLDEN_NAMES);
    expect(p.states.map((s) => s.enter.length)).toEqual(GOLDEN_STATES.map((s) => s.enter.length));
    expect(p.states.map((s) => s.body.length)).toEqual(GOLDEN_STATES.map((s) => s.body.length));
    // 逐字段比对（含负角度、UTF-8 名字、0xffff 的“继续”）
    GOLDEN_STATES.forEach((s, i) => {
      (['enter', 'body'] as const).forEach((which) => {
        s[which].forEach((want, k) => {
          const got = p.states[i][which][k];
          expect(got.op).toBe(OP_BY_NAME[want.op]);
          expect(got.flags).toBe(want.flags);
          expect(got.p0).toBe(want.p0);
          expect(got.p1).toBe(want.p1);
          expect(got.p2).toBe(want.p2);
          expect(got.p3).toBe(want.p3);
        });
      });
    });
    expect(p.states[1].body[0].p1).toBe(-90);
    expect(p.states[0].body[2].p3).toBe(1);
    expect(p.states[2].body[2].p3).toBe(NO_STATE);
    expect(p.states[2].body[3].op).toBe(OP.END);
  });

  it('金样本场景在编辑器侧的校验里没有错误（它本来就该是能烧的）', () => {
    expect(validateTask(goldenProgram())).toEqual({ errors: [], warnings: [] });
  });

  it('编出来的字节流以“能改变执行流”的块收尾（固件 TASK_E_TAIL 那条规则）', () => {
    const p = goldenProgram();
    p.states.forEach((st, i) => {
      const tail = st.body[st.body.length - 1];
      const ok = tail.op === OP.GOTO || tail.op === OP.END || (tail.op === OP.WAIT_COND && tail.p3 !== NO_STATE);
      expect(ok, `状态 ${i} 的 body 末尾不是跳转`).toBe(true);
    });
  });

  it('名字区没有 NUL、靠偏移切分（两个名字紧挨着也能读对）', () => {
    const { bytes } = encodeTask(goldenProgram());
    // 第一个名字的起点：状态表之后
    const first = bytes[12] | (bytes[13] << 8);
    expect(first).toBeGreaterThan(12);
    expect(bytes[first]).not.toBe(0);
    const tail = bytes.subarray(first);
    expect(tail.includes(0)).toBe(false); // 名字区是裸 UTF-8，没有终止符
  });

  it('常量表与固件枚举一致（改一个都说明有人在单边改格式）', () => {
    expect(OP.SET_SPEED).toBe(0x01);
    expect(OP.SET_MODE).toBe(0x02);
    expect(OP.TURN_DEG).toBe(0x03);
    expect(OP.ACTION).toBe(0x04);
    expect(OP.WAIT_MS).toBe(0x05);
    expect(OP.WAIT_COND).toBe(0x06);
    expect(OP.GOTO).toBe(0x07);
    expect(OP.END).toBe(0x08);
    expect(OP.SET_SERVO).toBe(0x09);
    expect(OP.CROSS_CLEAR).toBe(0x0a);
    expect(MODE.TRACE_GYRO).toBe(3);
    expect(MODE.WHEELS).toBe(4);
    expect(COND.CROSS).toBe(4);
    expect(COND.ALWAYS).toBe(9);
    expect(NO_STATE).toBe(0xffff);
  });
});
