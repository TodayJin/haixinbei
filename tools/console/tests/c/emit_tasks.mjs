/**
 * @file  tools/console/tests/c/emit_tasks.mjs
 * @brief 用**上位机的编码器**编出若干任务镜像 + 一份清单，交给真固件的校验器（taskcheck.exe）读。
 *
 * 用法：node emit_tasks.mjs <打包好的 taskentry.mjs> <输出目录>
 * 输出：<目录>/*.bin 与 <目录>/manifest.json
 *
 * 只做"造样本"，判断留给你（pytest 会拿 taskcheck 的输出逐条比）。
 */
import { writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const [bundlePath, outDir] = process.argv.slice(2);
if (!bundlePath || !outDir) {
  console.error('用法：node emit_tasks.mjs <taskentry.mjs> <outDir>');
  process.exit(2);
}

const T = await import(pathToFileURL(path.resolve(bundlePath)).href);
const { OP, MODE, COND, NO_STATE, blk, encodeTask, validateTask, crc16Ccitt } = T;

/** 三状态：中文名、负角度、带超时改道的等条件、END、舵机 —— 覆盖每种字段极性。 */
function threeStates() {
  return {
    main: 0,
    states: [
      {
        name: '常态循迹',
        enter: [blk(OP.SET_MODE, MODE.TRACE_GYRO)],
        body: [blk(OP.SET_SPEED, 6480), blk(OP.WAIT_COND, COND.CROSS, 0, 3000, 1), blk(OP.GOTO, 0)],
      },
      {
        name: '路口转90',
        enter: [blk(OP.WAIT_MS, 200)],
        body: [blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1), blk(OP.CROSS_CLEAR), blk(OP.GOTO, 1)],
      },
      {
        name: '摆舵机',
        enter: [],
        body: [blk(OP.SET_SERVO, 1, 180), blk(OP.SET_SERVO, 2, 0), blk(OP.WAIT_COND, COND.ALWAYS, 0, 500, NO_STATE), blk(OP.END)],
      },
    ],
  };
}

/** 16 个状态（上限）：每个都以 GOTO 收尾，且互相引用。 */
function maxStates() {
  const states = [];
  for (let i = 0; i < 16; i += 1) {
    states.push({
      name: `状态${i}`,
      enter: i % 3 === 0 ? [blk(OP.WAIT_MS, 20 * (i + 1))] : [],
      body: [blk(OP.SET_MODE, [MODE.IDLE, MODE.GYRO, MODE.TRACE, MODE.TRACE_GYRO][i % 4]), blk(OP.GOTO, (i + 1) % 16)],
    });
  }
  return { main: 7, states };
}

/** body 没有收尾块 —— 上位机校验该报"最后一块"，车上校验该回 TASK_E_TAIL(9)。 */
function noTail() {
  return { main: 0, states: [{ name: '缺尾', enter: [], body: [blk(OP.SET_SPEED, 3000)] }] };
}

/** 名字按 UTF-8 字节截到 32 —— 车上按偏移读名字，不能被多字节切坏。 */
function longName() {
  const name = T.truncateNameUtf8('很长很长的状态名字很长很长的状态名字', 32);
  return { main: 0, states: [{ name, enter: [], body: [blk(OP.END)] }] };
}

/** 每个任务上限内的最大块数：60 块 + 尾部 GOTO（64 是上限）。 */
function manyBlocks() {
  const body = [];
  for (let i = 0; i < 63; i += 1) body.push(blk(OP.WAIT_MS, 20));
  body.push(blk(OP.GOTO, 0));
  return { main: 0, states: [{ name: '长序列', enter: [], body }] };
}

const cases = [
  ['editor_default', T.newTaskDef(0, '常态循迹').prog],
  ['editor_file', T.defaultTaskFile().tasks[0].prog],
  ['three_states', threeStates()],
  ['max_states', maxStates()],
  ['many_blocks', manyBlocks()],
  ['no_tail', noTail()],
  ['long_name', longName()],
];

const manifest = [];
for (const [name, prog] of cases) {
  const { bytes, errors } = encodeTask(prog);
  const v = validateTask(prog);
  const file = `${name}.bin`;
  writeFileSync(path.join(outDir, file), Buffer.from(bytes));
  manifest.push({
    name,
    file,
    len: bytes.length,
    crc16: crc16Ccitt(bytes.subarray(12)),
    nStates: prog.states.length,
    encoderErrors: errors,
    validatorErrors: v.errors,
    validatorWarnings: v.warnings,
    names: prog.states.map((s) => s.name),
    bodyTails: prog.states.map((s) => (s.body.length ? s.body[s.body.length - 1].op : 0)),
  });
}

writeFileSync(path.join(outDir, 'manifest.json'), JSON.stringify(manifest, null, 1));

/* 顺手证明"上位机解开车上编出来的字节"这条反方向：把 encode 的结果解回来，字段要一模一样。 */
const roundtrip = [];
for (const [name, prog] of cases) {
  const dec = T.decodeTask(encodeTask(prog).bytes);
  roundtrip.push({ name, ok: !!dec.program && dec.error === '' });
}
writeFileSync(path.join(outDir, 'roundtrip.json'), JSON.stringify(roundtrip, null, 1));

console.log(`写出 ${manifest.length} 个镜像到 ${outDir}`);
