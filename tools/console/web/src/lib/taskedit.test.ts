/**
 * @file  taskedit.test.ts
 * @brief 钉住任务编辑器的纯逻辑：块库元数据、拖拽下标、状态增删、脏数据读回
 *
 * 拖拽最容易错的两处（**同序列右移的下标补偿**、**跨状态移动**）在这里用数组断言写死，
 * 页面里那点 JSX 就不用靠手点着试了。
 */
import { describe, expect, it } from 'vitest';

import { COND, MODE, NO_STATE, OP, TASK_MAX_STATES, blk, validateTask, waitCondJumpTarget, withWaitCondJumpTarget, type TaskBlock, type TaskProgram } from './task';
import {
  COND_SPECS,
  DRAG_MIME,
  EDIT_MAX_TASKS,
  MODE_OPTIONS,
  OP_SPECS,
  WAIT_COND_HOLD_MAX_MS,
  WAIT_COND_HOLD_UNIT_MS,
  addState,
  addTaskDef,
  blockCount,
  cloneState,
  compileInfo,
  decodeDrag,
  dedupeIds,
  defaultTaskFile,
  defaultsFor,
  describeBlock,
  dropPayload,
  duplicateBlock,
  encodeDrag,
  importTaskFile,
  insertBlock,
  loadTaskFile,
  modeChoices,
  modeLabel,
  moveBlock,
  nameBytes,
  newTaskDef,
  nextTaskId,
  removeBlock,
  removeState,
  renameState,
  sanitizeBlock,
  sanitizeProgram,
  seqRows,
  serializeTaskFile,
  setMain,
  stateChoices,
  stateLabel,
  truncateNameUtf8,
  updateBlock,
  waitCondHoldMs,
  withWaitCondHoldMs,
  type Where,
} from './taskedit';

/** 三状态测试程序：0 常态循迹（主页）→ 1 路口 → 2 摆舵机 */
function threeStates(): TaskProgram {
  return {
    main: 0,
    states: [
      { name: '常态循迹', enter: [blk(OP.SET_MODE, MODE.TRACE)], body: [blk(OP.SET_SPEED, 6480), blk(OP.GOTO, 0)] },
      {
        name: '路口',
        enter: [blk(OP.WAIT_MS, 200)],
        body: [blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1), blk(OP.GOTO, 1)],
      },
      { name: '摆舵机', enter: [], body: [blk(OP.SET_SERVO, 1, 180), blk(OP.END)] },
    ],
  };
}

const where = (state: number, seq: 'enter' | 'body', index: number): Where => ({ state, seq, index });
const ops = (list: TaskBlock[]) => list.map((b) => b.op);

describe('块库元数据', () => {
  it('11 个块每个都有说明和默认值，且默认值落在声明范围内', () => {
    expect(OP_SPECS.map((s) => s.op)).toEqual([
      OP.SET_SPEED,
      OP.SET_MODE,
      OP.TURN_DEG,
      OP.ACTION,
      OP.CAL_SET,
      OP.SET_SERVO,
      OP.WAIT_MS,
      OP.WAIT_COND,
      OP.CROSS_CLEAR,
      OP.GOTO,
      OP.END,
    ]);
    for (const spec of OP_SPECS) {
      expect(spec.label.length, spec.name).toBeGreaterThan(0);
      expect(spec.doc.length, spec.name).toBeGreaterThan(0);
      const b = defaultsFor(spec.op);
      expect(b.op, spec.name).toBe(spec.op);
      for (const f of spec.fields) {
        if (f.showIf && !f.showIf(b)) continue;
        const v = b[f.key];
        expect(v, `${spec.name}.${f.key}=${v} 应在 ${f.lo}..${f.hi}`).toBeGreaterThanOrEqual(f.lo);
        expect(v, `${spec.name}.${f.key}=${v} 应在 ${f.lo}..${f.hi}`).toBeLessThanOrEqual(f.hi);
      }
    }
  });

  it('条件表与固件条件号 1..11 一一对应，模式下拉 = 停车/角度环/循迹/两轮直给', () => {
    expect(COND_SPECS.map((c) => c.cond)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
    expect(COND_SPECS[0].cond).toBe(COND.LINE_CENTER);
    expect(COND_SPECS[8].cond).toBe(COND.ALWAYS);
    expect(COND_SPECS[9].cond).toBe(COND.CH_BLACK);
    expect(COND_SPECS[10].cond).toBe(COND.CH_WHITE);
    expect(MODE_OPTIONS.map((m) => m.value)).toEqual([MODE.IDLE, MODE.GYRO, MODE.TRACE_GYRO, MODE.WHEELS]);
  });

  it('老文件里的模式 2 仍能解析：下拉临时补一条带「（旧）」的项，摘要给得出人话', () => {
    // 当前值在标准四项里 ⇒ 就四项，一件不多
    expect(modeChoices(MODE.TRACE_GYRO).map((m) => m.value)).toEqual([MODE.IDLE, MODE.GYRO, MODE.TRACE_GYRO, MODE.WHEELS]);
    // 老文件里的 2 ⇒ 补一条，排序仍在后面（不改动文件内容）
    expect(modeChoices(MODE.TRACE).map((m) => m.value)).toEqual([MODE.IDLE, MODE.GYRO, MODE.TRACE_GYRO, MODE.WHEELS, MODE.TRACE]);
    expect(modeChoices(MODE.TRACE).at(-1)?.label).toContain('（旧）');
    // 两轮直给在标准项里（倒车要用），不再标「（旧）」
    expect(modeLabel(MODE.WHEELS)).toBe('两轮直给');
    // 人话：标准项说闭环形态，旧值说「（旧）」，未知值退回数字
    expect(modeLabel(MODE.TRACE_GYRO)).toBe('循迹');
    expect(modeLabel(MODE.GYRO)).toBe('角度环');
    expect(modeLabel(MODE.TRACE)).toContain('（旧）');
    expect(modeLabel(7)).toContain('7');
    // 新拖进来的「切换模式」默认是循迹（三层串级）
    expect(defaultsFor(OP.SET_MODE).p0).toBe(MODE.TRACE_GYRO);
  });

  it('「跳到状态」的目标只列真实状态（没有"继续往下跑"这种空操作），「等条件」的超时后才留着它', () => {
    const goto = OP_SPECS.find((s) => s.op === OP.GOTO)!;
    const wait = OP_SPECS.find((s) => s.op === OP.WAIT_COND)!;
    expect(goto.fields.find((f) => f.key === 'p0')?.allowNoState).toBeUndefined();
    expect(wait.fields.find((f) => f.key === 'p3')?.allowNoState).toBe(true);

    // 跳转块：3 个状态 ⇒ 就三条，一件多余都没有
    expect(stateChoices(0, 3, false).map((o) => o.value)).toEqual([0, 1, 2]);
    expect(stateChoices(0, 3, false).map((o) => o.label)).toEqual(['状态 0', '状态 1', '状态 2']);
    // 超时后：多一条 0xFFFF「继续往下跑」（= 超时也不改道）
    expect(stateChoices(NO_STATE, 3, true).map((o) => o.value)).toEqual([NO_STATE, 0, 1, 2]);
    expect(stateChoices(NO_STATE, 3, true)[0].label).toBe('继续往下跑');
    // 老文件把跳转目标存成 0xFFFF / 状态被删掉 ⇒ 临时补一条标出来的项，选框不会空
    expect(stateChoices(NO_STATE, 3, false).at(-1)?.label).toContain('（旧）');
    expect(stateChoices(9, 3, false).at(-1)?.label).toContain('不存在');
    // 删状态时不会把 0xFFFF 越修越偏（0xFFFE/0xFFFF 这种占位原样留着）
    const prog: TaskProgram = {
      main: 0,
      states: [
        { name: 'a', enter: [], body: [blk(OP.GOTO, NO_STATE)] },
        { name: 'b', enter: [], body: [] },
      ],
    };
    expect(removeState(prog, 1).states[0].body[0].p0).toBe(NO_STATE);
  });

  it('拖拽 MIME 是自定义类型（免得拖到浏览器里变成开新标签）', () => {
    expect(DRAG_MIME).toContain('x-hxb-task');
  });
});

describe('块摘要', () => {
  it('按块类型给出人话，两轮直给与超时改道都说得清', () => {
    expect(describeBlock(blk(OP.SET_SPEED, 6480), 3)).toContain('6480');
    expect(describeBlock(blk(OP.SET_SPEED, 6480), 3)).toContain('90%'); // 顺带给个能对上感觉的百分比
    expect(describeBlock(blk(OP.SET_MODE, MODE.WHEELS, 300, -300), 3)).toContain('左 300 右 -300');
    expect(describeBlock(blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1), 3)).toContain('从当前朝向');
    expect(describeBlock(blk(OP.WAIT_COND, COND.CROSS, 0, 3000, 1), 3)).toContain('超时后跳到 状态 1');
    expect(describeBlock(blk(OP.WAIT_COND, COND.CROSS, 0, 0, NO_STATE), 3)).toContain('不限时');
    // 条件说的是车底下发生了什么，不是「条件 4」
    expect(describeBlock(blk(OP.WAIT_COND, COND.CROSS, 0, 0, NO_STATE), 3)).toContain('压到横线');
    expect(describeBlock(blk(OP.GOTO, 9), 3)).toContain('不存在');
    expect(describeBlock(blk(OP.END), 3)).toContain('结束');
    expect(describeBlock(blk(0x77), 3)).toContain('不认识');
    // 给了状态名，跳转目标就用名字说话（用户 m25776：光看编号看不懂要跳到哪儿）
    expect(describeBlock(blk(OP.GOTO, 1), 3, ['进门', '台阶2', '木刺'])).toContain('状态 1 台阶2');
    expect(stateChoices(1, 3, false, ['进门', '台阶2', '木刺'])[1].label).toBe('状态 1 台阶2');
    // 用户 m26176：状态明明有名字，下拉里却只有编号 ⇒ 传了名字就必须带名字
    const named = ['进门', '台阶2', '木刺']
    for (const o of stateChoices(0, 3, false, named)) {
      expect(o.label, `下拉项 ${o.value} 没带名字：${o.label}`).toContain(named[o.value])
    }
    // 名字是空的（用户自己没起）⇒ 直说「没名字」，别让人以为是界面漏了（编号后面到底有没有名字，一眼要能分清）
    expect(stateLabel(0, ['', '台阶2'])).toBe('状态 0（没名字）')
    expect(describeBlock(blk(OP.GOTO, 0), 2, ['', '台阶2'])).toContain('状态 0（没名字）')
    // 不传名字的纯计算场合保持原样（老调用点/老测试）
    expect(stateLabel(0)).toBe('状态 0')
  });

  it('界面文案里不许出现固件内部名字（用户 m25776 就是被这些看不懂才要求重写）', () => {
    const bad = /(\bp[0-3]\b|flags\.|0x[0-9a-fA-F]|tracing_cross|Motion_GetFault|\bbit[0-7]\b)/;
    for (const s of OP_SPECS) {
      expect(bad.test(s.doc), `${s.label} 的说明里有内部名字：${s.doc}`).toBe(false);
      for (const f of s.fields) {
        expect(f.hint.length, `${s.label}.${f.label} 没有说明`).toBeGreaterThan(4);
        expect(bad.test(f.hint), `${s.label}.${f.label} 的说明里有内部名字：${f.hint}`).toBe(false);
      }
    }
    for (const c of COND_SPECS) {
      expect(c.label.length).toBeGreaterThan(2);
      expect(bad.test(c.hint), `${c.label} 的说明里有内部名字：${c.hint}`).toBe(false);
    }
    // 「丢线」用户明确说看不懂 ⇒ 界面上统一叫「探头全白」
    expect(COND_SPECS.find((c) => c.cond === COND.LINE_LOST)?.label).toBe('探头全白');
  });
});

describe('「等条件」的「需持续」（flags 低 7 位 ×100ms；与「满足后跳转」共用同一个字节）', () => {
  it('① 老任务（flags = 0）⇒ 需持续 = 0 = 「不要求持续」，写回也一个字节都不动', () => {
    const old = blk(OP.WAIT_COND, COND.CH_BLACK, 0xff, 8000, 5); // flags 默认 0（老镜像就长这样）
    expect(old.flags).toBe(0);
    expect(waitCondHoldMs(old.flags)).toBe(0); // ★界面那一格显示的就是这个 0
    // 界面「原样显示 → 用户没改 → 不改数据」：写回同一个 0，flags 还是 0
    expect(withWaitCondHoldMs(old.flags, waitCondHoldMs(old.flags)).flags).toBe(0);
    expect(withWaitCondHoldMs(old.flags, 0).flags).toBe(0);
    // 摘要里不许多出一句"需持续"，老任务看起来跟以前一模一样
    expect(describeBlock(old, 15)).not.toContain('需持续');
    expect(describeBlock(old, 15)).toContain('最多 8000ms');
  });

  it('② 填 1000ms ⇒ flags = 10（口径 = 低 7 位 × 100ms，与固件 TASK_WAIT_COND_HOLD_MSK 一致）', () => {
    const r = withWaitCondHoldMs(0, 1000);
    expect(r).toMatchObject({ flags: 10, blocked: false, clamped: false, storedMs: 1000 });
    expect(waitCondHoldMs(r.flags)).toBe(1000);
    expect(WAIT_COND_HOLD_UNIT_MS).toBe(100);
    expect(describeBlock(blk(OP.WAIT_COND, COND.CH_BLACK, 0xff, 6000, 5, 10), 15)).toContain('需持续 1000ms');
  });

  it('③ 填 13000ms（超范围）被拦：夹到 12700ms 并报 clamped = true（界面据此弹人话提示）', () => {
    const hi = withWaitCondHoldMs(0, 13000);
    expect(hi.clamped).toBe(true);
    expect(hi.storedMs).toBe(WAIT_COND_HOLD_MAX_MS);
    expect(hi.flags).toBe(127);
    expect(waitCondHoldMs(hi.flags)).toBe(12700);
    // 负数同理（夹到 0）
    const lo = withWaitCondHoldMs(0, -100);
    expect(lo.clamped).toBe(true);
    expect(lo.flags).toBe(0);
    // 正好在边界上不算超范围
    expect(withWaitCondHoldMs(0, WAIT_COND_HOLD_MAX_MS).clamped).toBe(false);
    expect(withWaitCondHoldMs(0, 0).clamped).toBe(false);
  });

  it('按 100ms 一格取整（填 1050 存成 1100），且量化不等于超范围', () => {
    expect(withWaitCondHoldMs(0, 1050).flags).toBe(11);
    expect(withWaitCondHoldMs(0, 1050).storedMs).toBe(1100);
    expect(withWaitCondHoldMs(0, 1050).clamped).toBe(false);
    expect(waitCondHoldMs(withWaitCondHoldMs(0, 60).flags)).toBe(100);
  });

  it('★bit7（满足后跳转）与「需持续」互斥：占着就写不进（原样返回），腾出来就能写', () => {
    const jumped = withWaitCondJumpTarget(0, 5); // 0x85 = bit7 + 目标状态 5
    expect(jumped).toBe(0x85);
    const blocked = withWaitCondHoldMs(jumped, 1000);
    expect(blocked.blocked).toBe(true);
    expect(blocked.flags).toBe(0x85); // ★原样返回 —— 绝不把跳转目标冲掉
    expect(waitCondHoldMs(jumped)).toBe(0); // 那几位是跳转目标，不是"需持续"
    expect(waitCondJumpTarget(jumped)).toBe(5);
    // 界面上把「满足后」选回「继续往下跑」⇒ 低 7 位腾出来，这一格就能填了
    const freed = withWaitCondJumpTarget(jumped, NO_STATE);
    expect(freed).toBe(0);
    expect(withWaitCondHoldMs(freed, 1000).flags).toBe(10);
    // 反向：先写需持续、再选跳转 ⇒ 跳转接管低 7 位（一块上只有一处能留）
    const holdFirst = withWaitCondHoldMs(0, 1000).flags;
    expect(holdFirst).toBe(10);
    expect(withWaitCondJumpTarget(holdFirst, 7)).toBe(0x87);
    expect(waitCondHoldMs(0x87)).toBe(0);
  });

  it('块库元数据：这一格是 kind = hold、0..12700ms，和「满足后」两行同时在字段表里', () => {
    const wait = OP_SPECS.find((s) => s.op === OP.WAIT_COND)!;
    const hold = wait.fields.find((f) => f.kind === 'hold')!;
    expect(hold.key).toBe('flags'); // 存的就是 flags（key 用 'flags'，页面里 b[f.key] 的类型才对得上）
    expect(hold.label).toBe('需持续');
    expect([hold.lo, hold.hi]).toEqual([0, WAIT_COND_HOLD_MAX_MS]);
    expect(hold.unit).toBe('ms');
    expect(hold.showIf).toBeUndefined(); // 恒显示：填不了的时候是"禁用 + 说明"，不是凭空消失
    expect(wait.fields.filter((f) => f.key === 'flags').map((f) => f.kind)).toEqual(['condjump', 'hold']);
    // 两行的 React key 不会撞（Tasks.tsx 的 fieldRow 用 `${kind}:${key}`）
    expect(new Set(wait.fields.map((f) => `${f.kind}:${f.key}`)).size).toBe(wait.fields.length);
  });

  it('缺省块（拖进来的「等条件」）不受影响：flags = 0 ⇒ 老行为', () => {
    expect(defaultsFor(OP.WAIT_COND).flags).toBe(0);
    expect(waitCondHoldMs(defaultsFor(OP.WAIT_COND).flags)).toBe(0);
  });
});

describe('拖拽的下标语义', () => {
  it('插入不改原文（纯函数），位置超界夹到末尾', () => {
    const p = threeStates();
    const q = insertBlock(p, where(2, 'body', 1), blk(OP.WAIT_MS, 100));
    expect(p.states[2].body.map((b) => b.op)).toEqual([OP.SET_SERVO, OP.END]);
    expect(q.states[2].body.map((b) => b.op)).toEqual([OP.SET_SERVO, OP.WAIT_MS, OP.END]);
    expect(q.states[2].body[1].p0).toBe(100);
    expect(insertBlock(p, where(2, 'body', 99), blk(OP.WAIT_MS, 1)).states[2].body).toHaveLength(3);
    expect(insertBlock(p, where(9, 'body', 0), blk(OP.END))).toBe(p);
  });

  it('往回拖：把最后一块拖到最前', () => {
    const p = threeStates();
    const q = moveBlock(p, where(0, 'body', 1), where(0, 'body', 0));
    expect(ops(q.states[0].body)).toEqual([OP.GOTO, OP.SET_SPEED]);
  });

  it('往右拖要补偿下标：[A,B,C] 的第 0 块拖到末尾是 [B,C,A] 而不是 [B,A,C]', () => {
    const p: TaskProgram = {
      main: 0,
      states: [{ name: 's', enter: [], body: [blk(OP.SET_SPEED, 1), blk(OP.WAIT_MS, 2), blk(OP.WAIT_MS, 3)] }],
    };
    // 拖到"第 3 块之前"（insert position 3 = 末尾）
    const q = moveBlock(p, where(0, 'body', 0), where(0, 'body', 3));
    expect(q.states[0].body.map((b) => b.p0)).toEqual([2, 3, 1]);
    // 拖到第 2 块之前：等于右移一格
    const r = moveBlock(p, where(0, 'body', 0), where(0, 'body', 2));
    expect(r.states[0].body.map((b) => b.p0)).toEqual([2, 1, 3]);
    // 原地放下不动
    const s = moveBlock(p, where(0, 'body', 1), where(0, 'body', 1));
    expect(s.states[0].body.map((b) => b.p0)).toEqual([1, 2, 3]);
  });

  it('跨状态移动：enter → 别的状态 body，源少了目的多了', () => {
    const p = threeStates();
    const q = moveBlock(p, where(1, 'enter', 0), where(2, 'body', 0));
    expect(q.states[1].enter).toHaveLength(0);
    expect(ops(q.states[2].body)).toEqual([OP.WAIT_MS, OP.SET_SERVO, OP.END]);
    expect(q.states[2].body[0].p0).toBe(200);
    expect(ops(p.states[2].body)).toEqual([OP.SET_SERVO, OP.END]);
  });

  it('拖不存在的块不动它', () => {
    const p = threeStates();
    expect(moveBlock(p, where(0, 'enter', 5), where(0, 'body', 0))).toBe(p);
    expect(removeBlock(p, where(0, 'body', 5))).toBe(p);
  });
});

describe('块的增删改', () => {
  it('从块库拖进来 = 用默认值插入；拖已有块 = 移动', () => {
    const p = threeStates();
    const q = dropPayload(p, where(0, 'body', 0), { kind: 'new', op: OP.WAIT_MS });
    expect(q.states[0].body[0].op).toBe(OP.WAIT_MS);
    expect(q.states[0].body[0].p0).toBe(defaultsFor(OP.WAIT_MS).p0);
    const r = dropPayload(p, where(2, 'body', 0), { kind: 'move', from: where(0, 'body', 0) });
    expect(ops(r.states[2].body)).toEqual([OP.SET_SPEED, OP.SET_SERVO, OP.END]);
  });

  it('改字段、复制一块、删一块', () => {
    const p = threeStates();
    const q = updateBlock(p, where(0, 'body', 0), { p0: 3000, flags: 2 });
    expect(q.states[0].body[0]).toMatchObject({ op: OP.SET_SPEED, p0: 3000, flags: 2 });
    const r = duplicateBlock(p, where(0, 'body', 0));
    expect(ops(r.states[0].body)).toEqual([OP.SET_SPEED, OP.SET_SPEED, OP.GOTO]);
    const s = removeBlock(p, where(0, 'enter', 0));
    expect(s.states[0].enter).toHaveLength(0);
    expect(p.states[0].enter).toHaveLength(1);
  });

  it('seqRows 的编号就是数组下标，isLast 只标最后一行', () => {
    const rows = seqRows(threeStates(), 0, 'body');
    expect(rows.map((r) => r.index)).toEqual([0, 1]);
    expect(rows.map((r) => r.isLast)).toEqual([false, true]);
    expect(seqRows(threeStates(), 9, 'body')).toEqual([]);
    expect(blockCount(threeStates())).toBe(8);
  });
});

describe('状态增删', () => {
  it('最多 16 个状态，复制状态是深拷贝', () => {
    let p = threeStates();
    for (let i = 0; i < 40; i += 1) p = addState(p, `状态${i}`);
    expect(p.states).toHaveLength(TASK_MAX_STATES);

    const c = cloneState(threeStates(), 0);
    expect(c.states[1].name).toBe('常态循迹-副本');
    c.states[1].body[0].p0 = 1;
    expect(c.states[0].body[0].p0).toBe(6480);
  });

  it('改名字按字节截断（中文 3 字节，32 字节只放得下 10 个字）', () => {
    const q = renameState(threeStates(), 0, '很长的状态名字很长的状态名字很长的状态名字');
    expect(nameBytes(q.states[0].name)).toBeLessThanOrEqual(32);
    expect(q.states[0].name).toBe('很长的状态名字很长的')
    expect(nameBytes(q.states[0].name)).toBe(30);
    expect(validateTask(q).errors.filter((e) => e.includes('名字'))).toEqual([]);
  });

  it('设主页', () => {
    expect(setMain(threeStates(), 2).main).toBe(2);
    expect(setMain(threeStates(), 9).main).toBe(0);
  });

  it('删状态时把指向它的跳转收拾干净', () => {
    const p: TaskProgram = {
      main: 1,
      states: [
        { name: 'a', enter: [], body: [blk(OP.GOTO, 2)] },
        { name: 'b', enter: [], body: [blk(OP.WAIT_COND, COND.CROSS, 0, 0, 2), blk(OP.GOTO, 1)] },
        { name: 'c', enter: [], body: [blk(OP.GOTO, 1)] },
      ],
    };
    const q = removeState(p, 1);
    expect(q.states.map((s) => s.name)).toEqual(['a', 'c']);
    // 指到被删状态(1)的 → 落到原来的 2 号（删后变 1 号）
    expect(q.states[0].body[0].p0).toBe(1);
    expect(q.states[1].body[0].op).toBe(OP.GOTO);
    expect(q.states[1].body[0].p0).toBe(1);
    // 主页被删 → 顺移到原来 2 号（删后 1 号）
    expect(q.main).toBe(1);
    // 最后一个状态删不掉
    expect(removeState({ main: 0, states: [{ name: 'x', enter: [], body: [] }] }, 0).states).toHaveLength(1);
  });

  it('删掉非主页状态时主页跟着前移', () => {
    const p = threeStates();
    p.main = 2;
    expect(removeState(p, 0).main).toBe(1);
  });
});

describe('拖拽载荷编解码', () => {
  it('往返', () => {
    expect(decodeDrag(encodeDrag({ kind: 'new', op: OP.WAIT_MS }))).toEqual({ kind: 'new', op: OP.WAIT_MS });
    expect(decodeDrag(encodeDrag({ kind: 'move', from: where(1, 'enter', 2) }))).toEqual({
      kind: 'move',
      from: { state: 1, seq: 'enter', index: 2 },
    });
  });

  it('坏数据一律 null（不能因为拖了一下别的程序就崩）', () => {
    expect(decodeDrag('')).toBeNull();
    expect(decodeDrag('{')).toBeNull();
    expect(decodeDrag('null')).toBeNull();
    expect(decodeDrag(JSON.stringify({ kind: 'new', op: 0x99 }))).toBeNull();
    expect(decodeDrag(JSON.stringify({ kind: 'move', from: { state: 0, seq: 'x', index: 0 } }))).toBeNull();
    expect(decodeDrag(JSON.stringify({ kind: 'what' }))).toBeNull();
  });
});

describe('名字与存盘', () => {
  it('truncateNameUtf8 按字节算', () => {
    const s = '啊'.repeat(20); // 60 字节
    expect(nameBytes(truncateNameUtf8(s))).toBe(30);
    expect(truncateNameUtf8('abc')).toBe('abc');
    expect(nameBytes(truncateNameUtf8('a'.repeat(99)))).toBe(32);
  });

  it('默认任务本身就是能烧的（校验零错）', () => {
    const f = defaultTaskFile();
    expect(f.tasks).toHaveLength(1);
    expect(f.tasks[0].name).toBe('常态循迹');
    expect(validateTask(f.tasks[0].prog).errors).toEqual([]);
    expect(validateTask(newTaskDef(0, '常态循迹').prog).errors).toEqual([]);
  });

  it('存盘→读回一致', () => {
    const f = defaultTaskFile();
    f.tasks.push(newTaskDef(1, '测试任务'));
    f.tasks[1].prog.states[0].body[0].p0 = 1234;
    const back = loadTaskFile(serializeTaskFile(f));
    expect(back.tasks.map((t) => t.name)).toEqual(['常态循迹', '测试任务']);
    expect(back.tasks[1].prog.states[0].body[0].p0).toBe(1234);
  });

  it('localStorage 里是垃圾/空的就给一份能用的默认值', () => {
    expect(loadTaskFile(null).tasks).toHaveLength(1);
    expect(loadTaskFile('不是 JSON').tasks).toHaveLength(1);
    expect(loadTaskFile('{}').tasks).toHaveLength(1);
    expect(loadTaskFile('{"tasks":[]}').tasks).toHaveLength(1);
  });

  it('读回时把脏数据修干净：未知块换掉、主页夹进范围、任务数封顶、编号去重', () => {
    const dirty = {
      tasks: Array.from({ length: 20 }, (_, i) => ({
        id: 0,
        name: `t${i}`,
        prog: { main: 99, states: [{ name: 's', enter: [{ op: 0x99 }], body: [{ op: OP.END }] }] },
      })),
    };
    const f = loadTaskFile(JSON.stringify(dirty));
    expect(f.tasks).toHaveLength(EDIT_MAX_TASKS);
    expect(new Set(f.tasks.map((t) => t.id)).size).toBe(EDIT_MAX_TASKS);
    for (const t of f.tasks) {
      expect(t.prog.main).toBe(0);
      expect(t.prog.states[0].enter[0].op).toBe(OP.WAIT_MS); // 未知块换成"等待"
      expect(validateTask(t.prog).errors).toEqual([]);
    }
  });

  it('sanitize 单块/单程序', () => {
    expect(sanitizeBlock({ op: OP.SET_SPEED, p0: 'x' }).p0).toBe(0);
    // 这里只保证**格式**上的范围（u16/i16）；每个块自己的语义范围由 validateTask 管
    expect(sanitizeBlock({ op: OP.SET_SPEED, p0: 99999 }).p0).toBe(65535);
    expect(sanitizeBlock({ op: OP.WAIT_MS, p1: -99999 }).p1).toBe(-32768);
    expect(sanitizeBlock({ op: OP.SET_SPEED, p3: undefined }).p3).toBe(NO_STATE);
    expect(sanitizeBlock({ op: OP.SET_SPEED, flags: -5 }).flags).toBe(0);
    expect(sanitizeProgram(null).states).toHaveLength(1);
    expect(sanitizeProgram({ main: 5, states: [] }).main).toBe(0);
    expect(dedupeIds([{ id: 1, name: 'a', prog: threeStates() }, { id: 1, name: 'b', prog: threeStates() }]).map((t) => t.id)).toEqual([1, 0]);
  });

  it('导入外部 JSON：认文件也认裸数组，坏了不抛异常', () => {
    const one = { tasks: [{ id: 3, name: '别人给的', prog: threeStates() }] };
    const a = importTaskFile(JSON.stringify(one));
    expect(a.tasks).toHaveLength(1);
    expect(a.tasks[0].name).toBe('别人给的');
    const b = importTaskFile(JSON.stringify([{ id: 0, name: 'n', prog: threeStates() }]));
    expect(b.tasks).toHaveLength(1);
    const c = importTaskFile('{{{');
    expect(c).toEqual({ tasks: [], skipped: 0 });
    const d = importTaskFile(JSON.stringify({ tasks: [{ idle: 1 }, { id: 0, name: 'ok', prog: threeStates() }] }));
    expect(d.tasks).toHaveLength(1);
    expect(d.skipped).toBe(1);
  });

  it('编号分配：空表→0、跳过已用、满了→-1', () => {
    expect(nextTaskId([])).toBe(0);
    expect(nextTaskId([{ id: 0, name: '', prog: threeStates() }])).toBe(1);
    expect(nextTaskId(Array.from({ length: EDIT_MAX_TASKS }, (_, i) => ({ id: i, name: '', prog: threeStates() })))).toBe(-1);
  });

  it('追加任务：自动发号、改名不改原程序、满表原样返回、名字按字节截断', () => {
    const base = newTaskDef(0, '甲');
    const one = addTaskDef([base], base, '乙');
    expect(one.map((t) => t.id)).toEqual([0, 1]);
    expect(one[1].name).toBe('乙');
    expect(one[1].prog.states.length).toBe(base.prog.states.length);
    const full = Array.from({ length: EDIT_MAX_TASKS }, (_, i) => ({ id: i, name: '', prog: threeStates() }));
    expect(addTaskDef(full, base)).toBe(full);
    expect(nameBytes(addTaskDef([], base, '很长'.repeat(20))[0].name)).toBeLessThanOrEqual(32);
  });
});

describe('编译预览', () => {
  it('长度/CRC 和 task.ts 的编码器一致，默认任务零错', () => {
    const p = threeStates();
    const info = compileInfo(p);
    expect(info.len).toBeGreaterThan(12);
    expect(info.errors).toEqual([]);
    expect(info.warnings).toEqual([]);
    expect(info.budgetPct).toBeGreaterThan(0);
    expect(info.budgetPct).toBeLessThan(100);
  });

  it('错误照传（body 没有收尾块），没名字的状态给警告', () => {
    const bad: TaskProgram = { main: 0, states: [{ name: '', enter: [], body: [blk(OP.SET_SPEED, 100)] }] };
    const info = compileInfo(bad);
    expect(info.errors.some((e) => e.includes('最后一块'))).toBe(true);
    expect(info.warnings.some((w) => w.includes('没有名字'))).toBe(true);
    expect(info.warnings.some((w) => w.includes('没有 body'))).toBe(false);
  });
});
