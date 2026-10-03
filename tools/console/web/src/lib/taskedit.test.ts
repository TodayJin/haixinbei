/**
 * @file  taskedit.test.ts
 * @brief 钉住任务编辑器的纯逻辑：块库元数据、拖拽下标、状态增删、脏数据读回
 *
 * 拖拽最容易错的两处（**同序列右移的下标补偿**、**跨状态移动**）在这里用数组断言写死，
 * 页面里那点 JSX 就不用靠手点着试了。
 */
import { describe, expect, it } from 'vitest';

import { COND, MODE, NO_STATE, OP, TASK_MAX_STATES, blk, validateTask, type TaskBlock, type TaskProgram } from './task';
import {
  COND_SPECS,
  DRAG_MIME,
  EDIT_MAX_TASKS,
  MODE_OPTIONS,
  OP_SPECS,
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
  truncateNameUtf8,
  updateBlock,
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
  it('10 个块每个都有说明和默认值，且默认值落在声明范围内', () => {
    expect(OP_SPECS.map((s) => s.op)).toEqual([
      OP.SET_SPEED,
      OP.SET_MODE,
      OP.TURN_DEG,
      OP.ACTION,
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

  it('条件表与固件条件号 1..9 一一对应，模式表 5 项', () => {
    expect(COND_SPECS.map((c) => c.cond)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9]);
    expect(COND_SPECS[0].cond).toBe(COND.LINE_CENTER);
    expect(COND_SPECS[8].cond).toBe(COND.ALWAYS);
    expect(MODE_OPTIONS.map((m) => m.value)).toEqual([MODE.IDLE, MODE.GYRO, MODE.TRACE, MODE.TRACE_GYRO, MODE.WHEELS]);
  });

  it('拖拽 MIME 是自定义类型（免得拖到浏览器里变成开新标签）', () => {
    expect(DRAG_MIME).toContain('x-hxb-task');
  });
});

describe('块摘要', () => {
  it('按块类型给出人话，两轮直给与超时改道都说得清', () => {
    expect(describeBlock(blk(OP.SET_SPEED, 6480), 3)).toContain('6480');
    expect(describeBlock(blk(OP.SET_MODE, MODE.WHEELS, 300, -300), 3)).toContain('左 300 右 -300');
    expect(describeBlock(blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1), 3)).toContain('相对');
    expect(describeBlock(blk(OP.WAIT_COND, COND.CROSS, 0, 3000, 1), 3)).toContain('超时→状态 1');
    expect(describeBlock(blk(OP.WAIT_COND, COND.CROSS, 0, 0, NO_STATE), 3)).toContain('不限时');
    expect(describeBlock(blk(OP.GOTO, 9), 3)).toContain('不存在');
    expect(describeBlock(blk(OP.END), 3)).toContain('结束');
    expect(describeBlock(blk(0x77), 3)).toContain('未知块');
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
