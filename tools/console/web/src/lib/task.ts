/**
 * 任务程序格式（上位机侧镜像）
 *
 * **与 `firmware/App/Svc/task_prog.h` 逐字节对齐**：magic/版本/头部/状态表/定长块/CRC
 * 任何一处改动都必须同时改两边，并让 `task.test.ts` 与 `firmware/test/t_task.c`
 * 里那条**同一个**金样本十六进制串继续相等（两边的测试都在断言它）。
 *
 * 设计文档：`docs/任务编程.md`。这里的函数是**纯函数**（不碰网络/不碰 DOM），
 * 所以拖拽编辑器、烧写、回读校验三处都只用这一份编解码。
 */

export const TASK_MAGIC = 0x4b54; // 小端字节序 'T'(0x54) 'K'(0x4B)
export const TASK_VER = 1;
export const TASK_HDR_LEN = 12;
export const TASK_STATE_LEN = 8;
export const TASK_BLOCK_LEN = 10;
export const TASK_MAX_STATES = 16;
export const TASK_MAX_BLOCKS = 64;
export const TASK_MAX_NAME = 32;

/** 程序里的块 op（数字必须与固件的 enum 一致） */
export const OP = {
  SET_SPEED: 0x01, // p0 = 基准速度（PWM 刻度 0..7200）
  SET_MODE: 0x02, // p0 = 控制模式；flags.bit0 = 两轮直给（p1 左 p2 右）
  TURN_DEG: 0x03, // p1 = 相对角（度）；p2 = 容差（0.1°，0→1.0°）；p0 = 超时 ms
  ACTION: 0x04, // p0 = 动作号 1..14；p2 = 动作后等待 ms
  WAIT_MS: 0x05, // p0 = 毫秒
  WAIT_COND: 0x06, // p0 = 条件号；p1 = 阈值；p2 = 超时 ms；p3 = 超时后跳的状态（0xffff = 继续）
  GOTO: 0x07, // p0 = 目标状态号
  END: 0x08, // 停车、任务结束
  SET_SERVO: 0x09, // p0 = 通道 1/2；p1 = 角度 0..180
  CROSS_CLEAR: 0x0a, // 清横线/路口计数
  CAL_SET: 0x0b, // ★p0 = 用第几套灰度标定：0 = 普通 / 1 = 亚克力
} as const;

export const MODE = {
  IDLE: 0,
  GYRO: 1,
  TRACE: 2,
  TRACE_GYRO: 3, // 三级串级 = 正常循迹
  WHEELS: 4, // 两轮速度直给
} as const;

export const COND = {
  LINE_CENTER: 1, // |重心-4.5| ≤ p1/10（0 → 0.5）
  LINE_LOST: 2, // 8 路灰度全部 < p1（0 → 60）
  LINE_FOUND: 3, // 至少一路 ≥ p1（0 → 60）
  CROSS: 4, // 检测到横线（满足后固件自动清）
  CROSS_COUNT: 5, // 累计横线次数 ≥ p1
  ANGLE_DONE: 6, // |角度环误差| ≤ p1/10（0 → 1.0°）
  DIST_MM: 7, // 累计里程 ≥ p1 毫米
  FAULT: 8, // 运动模块报故障
  ALWAYS: 9, // 恒真
} as const;

export const NO_STATE = 0xffff; // WAIT_COND 的 p3：超时后继续往下走

export interface TaskBlock {
  op: number;
  flags: number;
  p0: number;
  p1: number;
  p2: number;
  p3: number;
}

export interface TaskState {
  name: string;
  enter: TaskBlock[];
  body: TaskBlock[];
}

export interface TaskProgram {
  states: TaskState[];
  main: number; // 主页状态号（常态循迹那个）
}

/** 造一个块：未使用的字段给 0/0xffff（固件只读它用得到的字段，填充值不作规定） */
export function blk(
  op: number,
  p0 = 0,
  p1 = 0,
  p2 = 0,
  p3: number = NO_STATE,
  flags = 0,
): TaskBlock {
  return { op, flags, p0, p1, p2, p3 };
}

export function emptyState(name = ''): TaskState {
  return { name, enter: [], body: [] };
}

/** CRC16/CCITT-FALSE（poly 0x1021、init 0xffff、不反转；"123456789" → 0x29b1） */
export function crc16Ccitt(data: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i += 1) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

const utf8 = (s: string): Uint8Array => new TextEncoder().encode(s);

/** 一个块能改变执行流吗（能收尾 body 序列） */
export function isTailBlock(b: TaskBlock): boolean {
  if (b.op === OP.GOTO || b.op === OP.END) return true;
  return b.op === OP.WAIT_COND && b.p3 !== NO_STATE;
}

export interface ValidateResult {
  errors: string[];
  warnings: string[];
}

/** 编辑器侧校验：错误 = 固件一定会拒（`TaskProg_Validate` 的同一套规则）；警告 = 能跑但多半不是本意 */
export function validateTask(p: TaskProgram): ValidateResult {
  const errors: string[] = [];
  const warnings: string[] = [];

  if (p.states.length === 0) errors.push('至少要有一个状态');
  if (p.states.length > TASK_MAX_STATES) errors.push(`状态数不能超过 ${TASK_MAX_STATES}`);
  if (!(p.main >= 0 && p.main < p.states.length)) errors.push(`主页状态号越界（${p.main}，有效范围 0~${p.states.length - 1}）`);

  p.states.forEach((st, i) => {
    const tag = `状态 ${i}${st.name ? `「${st.name}」` : ''}`;
    if (st.name.length > TASK_MAX_NAME) errors.push(`${tag}：名字超过 ${TASK_MAX_NAME} 字节`);
    if (st.enter.length > TASK_MAX_BLOCKS) errors.push(`${tag}：enter 块数超过 ${TASK_MAX_BLOCKS}`);
    if (st.body.length > TASK_MAX_BLOCKS) errors.push(`${tag}：body 块数超过 ${TASK_MAX_BLOCKS}`);
    if (st.body.length === 0) {
      warnings.push(`${tag}：没有 body 块，进入这个状态就停住了`);
    } else if (!isTailBlock(st.body[st.body.length - 1])) {
      // 固件的 TASK_E_TAIL：状态跑到底不跳转，车会保持最后那个模式一直开下去
      errors.push(`${tag}：body 最后一块必须是「跳到状态 / 结束 / 带超时跳转的等待」`);
    }
    /* enter 序列是可选的（很多状态进门什么都不用做），故意不告警，免得噪声盖住真问题 */
    if (!st.name) warnings.push(`${tag}：没有名字，界面和监视里只能看编号`);
  });

  p.states.forEach((st, i) => {
    const tag = `状态 ${i}${st.name ? `「${st.name}」` : ''}`;
    [...st.enter, ...st.body].forEach((b) => {
      if (b.op === OP.GOTO && !(b.p0 < p.states.length)) {
        errors.push(`${tag}：跳到状态 ${b.p0}，但没有这个状态`);
      }
      if (b.op === OP.WAIT_COND && b.p3 !== NO_STATE && !(b.p3 < p.states.length)) {
        errors.push(`${tag}：超时改道到状态 ${b.p3}，但没有这个状态`);
      }
      if (b.op === OP.SET_MODE && b.p0 === MODE.WHEELS && (b.p1 > 7200 || b.p2 > 7200)) {
        errors.push(`${tag}：两轮速度超出 ±7200`);
      }
    });
  });

  return { errors, warnings };
}

export interface EncodedTask {
  bytes: Uint8Array;
  errors: string[];
}

/**
 * 编码成固件认识的那种字节流。**即使有校验错误也照编**（方便用户存草稿），
 * 但 `errors` 会一并返回；烧写前调用方必须自己检查 `errors.length === 0`。
 */
export function encodeTask(p: TaskProgram): EncodedTask {
  const { errors } = validateTask(p);
  const nStates = p.states.length;
  let off = TASK_HDR_LEN + nStates * TASK_STATE_LEN;

  const eoff: number[] = [];
  const boff: number[] = [];
  const noff: number[] = [];
  const nameBytes: Uint8Array[] = [];

  p.states.forEach((st, i) => {
    eoff[i] = st.enter.length > 0 ? off : 0;
    off += st.enter.length * TASK_BLOCK_LEN;
    boff[i] = st.body.length > 0 ? off : 0;
    off += st.body.length * TASK_BLOCK_LEN;
  });
  p.states.forEach((st, i) => {
    nameBytes[i] = utf8(st.name);
    noff[i] = nameBytes[i].length > 0 ? off : 0;
    off += nameBytes[i].length;
  });

  const total = off;
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  const u16 = (at: number, v: number) => dv.setUint16(at, v & 0xffff, true);
  const i16 = (at: number, v: number) => dv.setInt16(at, v > 32767 ? 32767 : v < -32768 ? -32768 : v, true);

  u16(0, TASK_MAGIC);
  out[2] = TASK_VER;
  out[3] = nStates;
  out[4] = p.main;
  out[5] = 0;
  u16(8, total); // total_len
  u16(10, 0);

  const writeBlock = (at: number, b: TaskBlock) => {
    out[at] = b.op & 0xff;
    out[at + 1] = b.flags & 0xff;
    u16(at + 2, b.p0);
    i16(at + 4, b.p1);
    u16(at + 6, b.p2);
    u16(at + 8, b.p3);
  };

  p.states.forEach((st, i) => {
    const so = TASK_HDR_LEN + i * TASK_STATE_LEN;
    u16(so, noff[i]);
    u16(so + 2, eoff[i]);
    u16(so + 4, boff[i]);
    out[so + 6] = st.enter.length;
    out[so + 7] = st.body.length;
    st.enter.forEach((b, j) => writeBlock(eoff[i] + j * TASK_BLOCK_LEN, b));
    st.body.forEach((b, j) => writeBlock(boff[i] + j * TASK_BLOCK_LEN, b));
    if (nameBytes[i].length > 0) out.set(nameBytes[i], noff[i]);
  });

  u16(6, crc16Ccitt(out.subarray(TASK_HDR_LEN))); // CRC 只覆盖头部之后
  return { bytes: out, errors };
}

export interface DecodedTask {
  program: TaskProgram | null;
  error: string;
}

/** 解码（回读校验用）：与固件 `TaskProg_Validate` 同一套检查 */
export function decodeTask(bytes: Uint8Array): DecodedTask {
  if (bytes.length < TASK_HDR_LEN) return { program: null, error: '程序太短' };
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (dv.getUint16(0, true) !== TASK_MAGIC) return { program: null, error: 'magic 不对' };
  if (bytes[2] !== TASK_VER) return { program: null, error: `版本不支持（${bytes[2]}）` };
  const nStates = bytes[3];
  if (nStates === 0 || nStates > TASK_MAX_STATES) return { program: null, error: '状态数越界' };
  if (bytes[4] >= nStates) return { program: null, error: '主页状态号越界' };
  if (dv.getUint16(8, true) !== bytes.length) return { program: null, error: '长度字段与实际不符' };
  if (TASK_HDR_LEN + nStates * TASK_STATE_LEN > bytes.length) {
    return { program: null, error: '状态表越界' };
  }
  if (dv.getUint16(6, true) !== crc16Ccitt(bytes.subarray(TASK_HDR_LEN))) {
    return { program: null, error: 'CRC 不对' };
  }

  const readBlock = (at: number): TaskBlock => ({
    op: bytes[at],
    flags: bytes[at + 1],
    p0: dv.getUint16(at + 2, true),
    p1: dv.getInt16(at + 4, true),
    p2: dv.getUint16(at + 6, true),
    p3: dv.getUint16(at + 8, true),
  });

  const states: TaskState[] = [];
  const raw: Array<{ nameOff: number; enter: TaskBlock[]; body: TaskBlock[] }> = [];
  const offsets: number[] = [];

  for (let i = 0; i < nStates; i += 1) {
    const so = TASK_HDR_LEN + i * TASK_STATE_LEN;
    const nameOff = dv.getUint16(so, true);
    const enterOff = dv.getUint16(so + 2, true);
    const bodyOff = dv.getUint16(so + 4, true);
    const nEnter = bytes[so + 6];
    const nBody = bytes[so + 7];

    if (
      (nameOff !== 0 && nameOff >= bytes.length) ||
      nEnter > TASK_MAX_BLOCKS ||
      nBody > TASK_MAX_BLOCKS ||
      (nEnter > 0 && (enterOff < TASK_HDR_LEN || enterOff + nEnter * TASK_BLOCK_LEN > bytes.length)) ||
      (nBody > 0 && (bodyOff < TASK_HDR_LEN || bodyOff + nBody * TASK_BLOCK_LEN > bytes.length))
    ) {
      return { program: null, error: '偏移/块数越界' };
    }

    const enter = Array.from({ length: nEnter }, (_, j) => readBlock(enterOff + j * TASK_BLOCK_LEN));
    const body = Array.from({ length: nBody }, (_, j) => readBlock(bodyOff + j * TASK_BLOCK_LEN));
    offsets.push(enterOff, bodyOff, nameOff);
    raw.push({ nameOff, enter, body });
  }

  /* 名字区在块区之后、且没有 NUL 结尾：一个名字的结束位置 = 下一个偏移 */
  const sorted = [...new Set(offsets.filter((o) => o > 0))].sort((a, b) => a - b);
  const nameEnd = (off: number): number => sorted.find((o) => o > off) ?? bytes.length;

  for (const r of raw) {
    let name = '';
    if (r.nameOff > 0) {
      name = new TextDecoder().decode(bytes.subarray(r.nameOff, nameEnd(r.nameOff)));
    }
    states.push({ name, enter: r.enter, body: r.body });
  }

  return { program: { states, main: bytes[4] }, error: '' };
}

/** 十六进制（日志/对拍用） */
export function hexOf(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}
