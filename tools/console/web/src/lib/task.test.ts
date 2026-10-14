/**
 * 任务程序格式（`tools/console/web/src/lib/task.ts`）的测试：
 * crc16、编码的字节口径、编解码往返、以及 validate/decode 的拒绝路径。
 *
 * **跨语言只用一条链**：`task.golden.test.ts` + `tools/console/tests/fixtures/task.golden.json`
 * —— 那份金样本是 `tools/console/tests/c/taskgen.c` 链**真固件** `App/Svc/task_prog.c`
 * 编出来的（`tools/console/tests/test_task_bytes.py` 每次重编比对，`make_golden.ps1` 重新生成）。
 * 这里**不再手抄**一遍十六进制：手抄的表迟早和真值漂开，而且漂了没人知道。
 */
import { describe, expect, it } from 'vitest';

import {
  COND,
  MODE,
  NO_STATE,
  OP,
  TASK_HDR_LEN,
  TASK_VER,
  blk,
  crc16Ccitt,
  decodeTask,
  encodeTask,
  validateTask,
  type TaskProgram,
} from './task';

function basicProgram(): TaskProgram {
  return {
    main: 0,
    states: [
      {
        name: 'trace',
        enter: [],
        body: [
          blk(OP.SET_SPEED, 6480),
          blk(OP.SET_MODE, MODE.TRACE_GYRO),
          blk(OP.WAIT_COND, COND.CROSS, 0, 1000, 2),
          blk(OP.GOTO, 1),
        ],
      },
      {
        name: 'hold',
        enter: [blk(OP.SET_SPEED, 3000)],
        body: [blk(OP.ACTION, 3, 0, 40), blk(OP.END)],
      },
      {
        name: 'bail',
        enter: [],
        body: [blk(OP.SET_MODE, MODE.IDLE), blk(OP.END)],
      },
    ],
  };
}

describe('crc16Ccitt', () => {
  it('标准校验值 "123456789" → 0x29b1（与固件 host_link.c 同构）', () => {
    expect(crc16Ccitt(new TextEncoder().encode('123456789'))).toBe(0x29b1);
  });

  it('空数据 = init 值', () => {
    expect(crc16Ccitt(new Uint8Array(0))).toBe(0xffff);
  });
});

describe('encodeTask 字节口径', () => {
  const { bytes, errors } = encodeTask(basicProgram());

  it('没有校验错误', () => {
    expect(errors).toEqual([]);
  });

  it('总长 = 头部 12 + 状态表 3×8 + 块 9×10 + 名字 5+4+4', () => {
    expect(bytes.length).toBe(TASK_HDR_LEN + 24 + 90 + 13);
    expect(bytes.length).toBe(139);
  });

  it('头部：小端 magic "TK"、版本、状态数、主页、长度字段', () => {
    expect(bytes[0]).toBe(0x54); // 'T'
    expect(bytes[1]).toBe(0x4b); // 'K'
    expect(bytes[2]).toBe(TASK_VER);
    expect(bytes[3]).toBe(3);
    expect(bytes[4]).toBe(0);
    expect(bytes[8] | (bytes[9] << 8)).toBe(bytes.length);
  });

  it('CRC 只覆盖头部之后', () => {
    const crc = bytes[6] | (bytes[7] << 8);
    expect(crc).toBe(crc16Ccitt(bytes.subarray(TASK_HDR_LEN)));
  });

  it('第一个块 = SET_SPEED 6480（p0 小端 50 19）', () => {
    const at = TASK_HDR_LEN + 3 * 8;
    expect(bytes[at]).toBe(OP.SET_SPEED);
    expect(bytes[at + 1]).toBe(0);
    expect(bytes[at + 2]).toBe(0x50);
    expect(bytes[at + 3]).toBe(0x19);
  });
});

describe('encode/decode 往返', () => {
  it('编码再解码得到同一份程序（含中文名字）', () => {
    const p = basicProgram();
    p.states[0].name = '常态循迹';
    p.states[1].name = '过障碍';
    p.states[2].name = '';
    const { bytes } = encodeTask(p);
    const { program, error } = decodeTask(bytes);

    expect(error).toBe('');
    expect(program).toEqual(p);
  });

  it('多个名字的边界靠"下一个偏移"切，不会串到下一个名字里', () => {
    const p: TaskProgram = {
      main: 0,
      states: [
        { name: 'a', enter: [], body: [blk(OP.END)] },
        { name: 'bb', enter: [], body: [blk(OP.END)] },
      ],
    };
    const { program } = decodeTask(encodeTask(p).bytes);
    expect(program?.states.map((s) => s.name)).toEqual(['a', 'bb']);
  });

  it('未使用的字段填 0xffff 也不影响解码（固件只读它用得到的字段）', () => {
    const p: TaskProgram = { main: 0, states: [{ name: '', enter: [], body: [blk(OP.SET_SPEED, 100)] }] };
    const b = encodeTask(p).bytes;
    expect(b[b.length - 1]).toBe(0xff); // p3 的高字节
    expect(decodeTask(b).program?.states[0].body[0].p3).toBe(NO_STATE);
  });
});

describe('decodeTask 的拒绝路径（与固件 TaskProg_Validate 同一套）', () => {
  const good = encodeTask(basicProgram()).bytes;

  it('太短', () => {
    expect(decodeTask(good.subarray(0, 6)).error).toBe('程序太短');
  });

  it('magic 不对', () => {
    const b = Uint8Array.from(good);
    b[0] ^= 0xff;
    expect(decodeTask(b).error).toBe('magic 不对');
  });

  it('版本不支持', () => {
    const b = Uint8Array.from(good);
    b[2] = 9;
    expect(decodeTask(b).error).toContain('版本不支持');
  });

  it('状态数越界', () => {
    const b = Uint8Array.from(good);
    b[3] = 0;
    expect(decodeTask(b).error).toBe('状态数越界');
  });

  it('主页越界', () => {
    const b = Uint8Array.from(good);
    b[4] = 3;
    expect(decodeTask(b).error).toBe('主页状态号越界');
  });

  it('长度字段不符', () => {
    const b = Uint8Array.from(good);
    b[8] = (b[8] - 1) & 0xff;
    expect(decodeTask(b).error).toBe('长度字段与实际不符');
  });

  it('CRC 不对', () => {
    const b = Uint8Array.from(good);
    b[20] ^= 0x01;
    expect(decodeTask(b).error).toBe('CRC 不对');
  });
});

describe('validateTask', () => {
  it('规范程序没有错误也没有警告', () => {
    const r = validateTask(basicProgram());
    expect(r.errors).toEqual([]);
    expect(r.warnings).toEqual([]);
  });

  it('body 末尾不是跳转/结束 = 错误（固件的 E_TAIL）', () => {
    const p: TaskProgram = { main: 0, states: [{ name: 'x', enter: [], body: [blk(OP.WAIT_MS, 500)] }] };
    expect(validateTask(p).errors.join()).toContain('body 最后一块必须是');
  });

  it('带超时跳转的"等待条件"可以收尾', () => {
    const p: TaskProgram = {
      main: 0,
      states: [
        { name: 'x', enter: [], body: [blk(OP.WAIT_COND, COND.CROSS, 0, 500, 1)] },
        { name: 'y', enter: [], body: [blk(OP.END)] },
      ],
    };
    expect(validateTask(p).errors).toEqual([]);
  });

  it('主页越界 / 状态数超上限 / 跳到不存在的状态都是错误', () => {
    expect(validateTask({ main: 2, states: [blkState(OP.END)] }).errors.join()).toContain('主页状态号越界');
    expect(validateTask({ main: 0, states: [] }).errors.join()).toContain('至少要有一个状态');
    // 上限 2026-10-09 起是 32（原来 16，因为旧全程流程要"主页 + 21 段"）—— 造 33 个必须被拦下
    const many: TaskProgram = { main: 0, states: Array.from({ length: 33 }, () => blkState(OP.END)) };
    expect(validateTask(many).errors.join()).toContain('状态数不能超过 32');
    expect(validateTask({ main: 0, states: [blkState(OP.GOTO, 5)] }).errors.join()).toContain('跳到状态 5');
    expect(validateTask({ main: 0, states: [blkState(OP.WAIT_COND, COND.CROSS, 0, 100, 5)] }).errors.join()).toContain(
      '超时改道到状态 5',
    );
  });

  it('能跑但多半不是本意的情况给警告（空 body / 无名）', () => {
    const p: TaskProgram = { main: 0, states: [{ name: '', enter: [], body: [] }] };
    const w = validateTask(p).warnings.join();
    expect(w).toContain('没有 body 块');
    expect(w).toContain('没有名字');
  });

  it('两轮直给的速度超出 ±7200 = 错误', () => {
    const p: TaskProgram = {
      main: 0,
      states: [{ name: 'x', enter: [], body: [blk(OP.SET_MODE, MODE.WHEELS, 9000, 0, NO_STATE, 0x01), blk(OP.END)] }],
    };
    expect(validateTask(p).errors.join()).toContain('两轮速度超出');
  });
});

/* ★审计 #37：两轮直给（SET_MODE 的 p2）的符号口径
   —— 固件按 `(int)(int16_t)b->p2` 解释（task_run.c:318），上位机回读/校验必须同口径。 */
describe('SET_MODE 右轮（p2）按有符号解释', () => {
  const wheels = (p1: number, p2: number): TaskProgram => ({
    main: 0,
    states: [{ name: 'x', enter: [], body: [blk(OP.SET_MODE, MODE.WHEELS, p1, p2, NO_STATE, 0x01), blk(OP.END)] }],
  });

  it('回读：右轮 −3888 不会再变成 61936，也不会被误判"超出 ±7200"', () => {
    const { bytes, errors } = encodeTask(wheels(-3888, -3888));
    expect(errors).toEqual([]);
    const { program, error } = decodeTask(bytes);
    expect(error).toBe('');
    // 改之前：p2 读成 61936 ⇒ 界面上显示 61936、validateTask 报错 ⇒ 烧写按钮被禁用
    expect(program?.states[0].body[0].p1).toBe(-3888);
    expect(program?.states[0].body[0].p2).toBe(-3888);
    expect(validateTask(program as TaskProgram).errors).toEqual([]);
  });

  it('两轮速度上下界都要判：±7201 是错误，±7200 不是', () => {
    expect(validateTask(wheels(0, -7201)).errors.join()).toContain('两轮速度超出');
    expect(validateTask(wheels(0, 7201)).errors.join()).toContain('两轮速度超出');
    expect(validateTask(wheels(-7200, 7200)).errors).toEqual([]);
  });

  it('其余块的 p2 仍是无符号：「舵机动作」之后等 60000ms 不会被读成 −5536', () => {
    const p: TaskProgram = {
      main: 0,
      states: [{ name: 'x', enter: [], body: [blk(OP.ACTION, 3, 0, 60000), blk(OP.END)] }],
    };
    expect(decodeTask(encodeTask(p).bytes).program?.states[0].body[0].p2).toBe(60000);
  });
});

/* ★审计 #38/#39：条件 9「马上通过」+ 超时 = 假延时
   —— 固件当拍就完成（task_run.c:232,395,411），超时那栏永远走不到。 */
describe('「马上通过」配超时 = 假延时', () => {
  const waitCond9 = (p2: number): TaskProgram => ({
    main: 0,
    states: [{ name: 'x', enter: [], body: [blk(OP.WAIT_COND, COND.ALWAYS, 0, p2, NO_STATE), blk(OP.END)] }],
  });

  it('恒真 + 填了超时 ⇒ 警告（固件照跑不拒收，所以不是错误），提示改用「等待」', () => {
    const r = validateTask(waitCond9(450));
    expect(r.errors).toEqual([]);
    expect(r.warnings.some((w) => w.includes('马上通过') && w.includes('450'))).toBe(true);
  });

  it('换成「等待」块（duan-test-2seg.json 的修法）既不报错也不告警', () => {
    const p: TaskProgram = { main: 0, states: [{ name: 'x', enter: [], body: [blk(OP.WAIT_MS, 450), blk(OP.END)] }] };
    expect(validateTask(p)).toEqual({ errors: [], warnings: [] });
  });

  it('p2 = 0（不限时）的「马上通过」不告警 —— 它本来就该当拍通过', () => {
    expect(validateTask(waitCond9(0)).warnings).toEqual([]);
  });
});

function blkState(op: number, p0 = 0, p1 = 0, p2 = 0, p3: number = NO_STATE): { name: string; enter: never[]; body: ReturnType<typeof blk>[] } {
  return { name: 's', enter: [], body: [blk(op, p0, p1, p2, p3)] };
}
