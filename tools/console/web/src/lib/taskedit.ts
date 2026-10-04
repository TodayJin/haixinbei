/**
 * 任务编辑器的**纯逻辑**（块库元数据、拖拽、增删、存盘）—— 不碰 DOM、不碰网络。
 *
 * 为什么不放页面里：拖拽的插入位置、跨状态移动时"先删后插的下标补偿"最容易错，
 * 放在 JSX 里只能靠手点着试。这里是纯函数，`taskedit.test.ts` 把每条语义钉死，
 * `pages/Tasks.tsx` 只负责把鼠标事件翻译成这里的调用。
 *
 * 字节格式本身不在这里 —— 那是 `lib/task.ts`（与固件 `task_prog.c` 对拍过的那套）。
 */
import {
  COND,
  MODE,
  NO_STATE,
  OP,
  TASK_HDR_LEN,
  TASK_MAX_BLOCKS,
  TASK_MAX_NAME,
  TASK_MAX_STATES,
  blk,
  crc16Ccitt,
  encodeTask,
  validateTask,
  type TaskBlock,
  type TaskProgram,
  type TaskState,
} from './task'

/** 车上能放几个任务：文档算过"8 状态 × 12 块 ≈ 1.1KB ⇒ 4~5 个"，界面按 8 个允许 */
export const EDIT_MAX_TASKS = 8

/** 一个任务 = 车上的编号 + 上位机的名字 + 程序本体（名字只在上位机侧，见 docs/任务编程.md §5 的空洞） */
export interface TaskDef {
  id: number
  name: string
  prog: TaskProgram
}

export interface TaskFileJSON {
  ver: 1
  tasks: TaskDef[]
}

/* ------------------------------------------------------- 块库元数据（界面用） */

export type FieldKey = 'p0' | 'p1' | 'p2' | 'p3'
export type FieldKind = 'int' | 'mode' | 'cond' | 'state' | 'servo'

export interface FieldSpec {
  key: FieldKey
  label: string
  /** **给用户看的一句人话**（鼠标停在标题上 / 显示在这一格下面）。
   *  规矩：只讲"这个数会改变车的什么行为"，不出现 p0/p1/p2/p3、flags.bit0、0xFFFF、
   *  tracing_cross() 这类固件里的名字 —— 用户 m25776 就是被这些看不懂才要求重写的。 */
  hint: string
  kind: FieldKind
  lo: number
  hi: number
  unit?: string
  /** 只在满足条件时显示（比如两轮直给才有左右轮） */
  showIf?: (b: TaskBlock) => boolean
  /** kind === 'state' 时：是否允许选「继续往下跑」(0xFFFF = 不跳转)。
   *  只有「等条件」的"超时后"该有它（= 超时也不改道）；「跳到状态」的目标是个真状态，
   *  那里给 0xFFFF 等于加了个什么都不做的块（固件 task_run.c:214 的 GOTO 分支会跳过它）。 */
  allowNoState?: boolean
}

/** 下拉选项：值 + 界面上的名字 + 一句人话（鼠标停在选项/选中项下面显示） */
export interface Choice {
  value: number
  label: string
  hint: string
}

export interface OpSpec {
  op: number
  name: string
  label: string
  group: '运动' | '等待' | '动作' | '流程'
  doc: string
  fields: FieldSpec[]
}

const speed = (key: FieldKey, label: string, hint: string): FieldSpec => ({ key, label, hint, kind: 'int', lo: 0, hi: 7200, unit: '油门刻度' })

/** 10 个基础块的字段语义 —— 与 docs/任务编程.md 的块表逐条对应。
 *  文案规矩（用户 m25776）：`doc` 只说"这块是干什么的"，字段 `hint` 只说"这个数改变车的什么行为"，
 *  一律写物理现象与人话；固件内部名字（p0~p3、flags.bit0、0xFFFF、tracing_cross()）**不许出现**。 */
export const OP_SPECS: OpSpec[] = [
  {
    op: OP.SET_SPEED,
    name: 'SET_SPEED',
    label: '设定速度',
    group: '运动',
    doc: '给两个轮子定一个基准速度（快慢）',
    fields: [
      speed(
        'p0',
        '速度',
        '0 = 停；0~7200 是油门刻度（不是 mm/s），满刻度 7200 ≈ 516 mm/s。低于 2600（35%）还在电机死区里、车不会动；倒车写负数。旧流程里的「900」就是这里的 6480。',
      ),
    ],
  },
  {
    op: OP.SET_MODE,
    name: 'SET_MODE',
    label: '切换模式',
    group: '运动',
    doc: '决定车接下来怎么控制自己',
    fields: [
      { key: 'p0', label: '模式', hint: '选一种控制方式；下面那行小字写的就是你现在选中的这个是什么意思。', kind: 'mode', lo: 0, hi: 4 },
      { key: 'p1', label: '左轮', hint: '左轮油门刻度，负数 = 反转。', kind: 'int', lo: -7200, hi: 7200, unit: '刻度', showIf: (b) => b.p0 === MODE.WHEELS },
      { key: 'p2', label: '右轮', hint: '右轮油门刻度，负数 = 反转。左右给相反的数就能原地转。', kind: 'int', lo: -7200, hi: 7200, unit: '刻度', showIf: (b) => b.p0 === MODE.WHEELS },
    ],
  },
  {
    op: OP.TURN_DEG,
    name: 'TURN_DEG',
    label: '转角度',
    group: '运动',
    doc: '把车头转到一个角度（原地转）',
    fields: [
      { key: 'p0', label: '超时', hint: '最多等这么久；到点还没转到位也往下走，不会卡死在这里。', kind: 'int', lo: 0, hi: 60000, unit: 'ms' },
      { key: 'p1', label: '角度', hint: '要转多少度：正数 = 左转，负数 = 右转。', kind: 'int', lo: -360, hi: 360, unit: '度' },
      { key: 'p2', label: '容差', hint: '误差比它小就算"转到位了"，单位 0.1°：填 10 = 允许差 1.0°。', kind: 'int', lo: 0, hi: 100, unit: '0.1°' },
    ],
  },
  { op: OP.ACTION, name: 'ACTION', label: '舵机动作', group: '动作', doc: '播车上存好的那套舵机动作', fields: [{ key: 'p0', label: '动作号', hint: '车上动作表里的第几套（1~14），就是你在车上用五向键录的那些。', kind: 'int', lo: 1, hi: 14 }, { key: 'p2', label: '之后等待', hint: '动作播完后先等这么久再往下走（舵机自己在动，程序不等它）。', kind: 'int', lo: 0, hi: 60000, unit: 'ms' }] },
  { op: OP.SET_SERVO, name: 'SET_SERVO', label: '直接摆舵机', group: '动作', doc: '不经过动作表，直接给舵机一个角度', fields: [{ key: 'p0', label: '通道', hint: '车上的第 1 号或第 2 号舵机通道。', kind: 'servo', lo: 1, hi: 2 }, { key: 'p1', label: '角度', hint: '舵机摆到多少度（0~180），调试用。', kind: 'int', lo: 0, hi: 180, unit: '度' }] },
  { op: OP.WAIT_MS, name: 'WAIT_MS', label: '等待', group: '等待', doc: '死等一段时间，什么都不改', fields: [{ key: 'p0', label: '时间', hint: '等多久。1000 = 1 秒；等的时候车保持当前速度和模式继续跑。', kind: 'int', lo: 0, hi: 60000, unit: 'ms' }] },
  {
    op: OP.WAIT_COND,
    name: 'WAIT_COND',
    label: '等条件',
    group: '等待',
    doc: '等到某个情况出现再往下走',
    fields: [
      { key: 'p0', label: '条件', hint: '要等的那个情况（车底下发生什么才算等到）。', kind: 'cond', lo: 1, hi: 9 },
      { key: 'p1', label: '阈值', hint: '这个条件的门槛，含义跟着上面的条件变（看选中那条的说明）；填 0 = 用固件默认（全白 60、回到线中间 5、转到指定角度 10）。压到横线 / 立即 / 车出故障 这三条用不到它。', kind: 'int', lo: -32768, hi: 32767 },
      { key: 'p2', label: '超时', hint: '最多等多久。填 0 = 一直等（这时「超时后」那一栏完全不起作用）。', kind: 'int', lo: 0, hi: 60000, unit: 'ms' },
      { key: 'p3', label: '超时后', hint: '等超时了改跳到哪个状态；选「继续往下跑」就等于没超时，接着执行这一块的下一行。', kind: 'state', lo: 0, hi: 0xffff, allowNoState: true },
    ],
  },
  { op: OP.CROSS_CLEAR, name: 'CROSS_CLEAR', label: '清横线', group: '等待', doc: '把"压过几条横线"的计数清零', fields: [] },
  { op: OP.GOTO, name: 'GOTO', label: '跳到状态', group: '流程', doc: '立刻跳到另一个状态，不等任何东西', fields: [{ key: 'p0', label: '目标', hint: '跳到哪个状态；新状态会从它的「进门」序列重新开始。这里只能选真实状态。', kind: 'state', lo: 0, hi: 0xfffe }] },
  { op: OP.END, name: 'END', label: '结束任务', group: '流程', doc: '停车，整个任务到此结束', fields: [] },
]

export const OP_BY_CODE = new Map<number, OpSpec>(OP_SPECS.map((s) => [s.op, s]))

export interface CondSpec {
  cond: number
  label: string
  hint: string
}

export const COND_SPECS: CondSpec[] = [
  { cond: COND.LINE_CENTER, label: '回到线中间', hint: '车又正对着线了（重心偏差回到 4.5 附近）。阈值 = 允许偏多少（单位 0.1）：填 50 = 允许偏 5.0，填 0 用默认 5 = 允许偏 0.5。' },
  { cond: COND.LINE_LOST, label: '探头全白', hint: '车底下 8 个探头一个都没看见黑线 —— 掉出赛道、车悬空、正在过一个障碍（坡 / 木刺 / 栅栏）时都是"全白"，也就是以前说的"丢线"。阈值 = 灰度门槛（0~255）：读数比它小就算白；填 0 用默认 60。' },
  { cond: COND.LINE_FOUND, label: '又看见黑线', hint: '8 个探头里任意一个又看见黑线了（过完障碍、重新压回线上）。阈值 = 灰度门槛：读数 ≥ 它就算看见；填 0 用默认 60。' },
  { cond: COND.CROSS, label: '压到横线', hint: '车底下压过一条横着的黑线（路口 / 停止线那种）。过一条就满足一次，而且满足后横线计数会自动清零。' },
  { cond: COND.CROSS_COUNT, label: '压过几条横线', hint: '累计压过多少条横线才算等到。阈值 = 要几条：填 2 = 压过两条。' },
  { cond: COND.ANGLE_DONE, label: '转到指定角度', hint: '车头已经转到了目标角度附近（前面「转角度」那个块设的目标）。阈值 = 允许差多少（单位 0.1°）：填 10 = 允许差 1.0°，填 0 用默认 10。' },
  { cond: COND.DIST_MM, label: '走够距离', hint: '两个轮子转过的距离加起来够数了。阈值 = 编码器计数（不是毫米）。' },
  { cond: COND.FAULT, label: '车出故障', hint: '控制环报错停机了（电机 / 编码器异常那种），一般放在收尾里保命。' },
  { cond: COND.ALWAYS, label: '马上通过', hint: '立刻就算等到，不等任何东西；给它配一个「超时」就成了一个纯等待。' },
]

export const COND_BY_CODE = new Map<number, CondSpec>(COND_SPECS.map((c) => [c.cond, c]))

/** 下拉里的模式：停车 / 角度环 / 循迹（三级串级）/ 两轮直给。
 *  用户 m12856 定案「正常循迹只有三级串级」，m19122 又嫌选项太多 ⇒ 砍掉了旧两级（值 2）。
 *  「两轮直给」（值 4）**必须留着**：它是唯一能左右分开给速（含负值倒车）的模式，
 *  旧比赛流程里"斜着蹭障碍 / 原地转"全靠它；没有它，旧流程既复现不了、也没法倒车。
 *  值 2 固件仍然认、老文件仍然解析执行，只是不再出现在下拉里。（老镜像里若真是这个值，
 *  下拉会临时补一条标着「（旧）」的项，不改动文件。） */
export const MODE_OPTIONS: Choice[] = [
  { value: MODE.IDLE, label: '停车', hint: '什么都不输出，车靠惯性滑停；再给速度才会重新动。' },
  { value: MODE.GYRO, label: '角度环', hint: '不看线，锁住车头方向直走：陀螺仪盯着航向，歪了就纠回来。转弯后直行、过障碍时用。' },
  { value: MODE.TRACE_GYRO, label: '循迹', hint: '正常跟线跑（位置 → 速度 → 电流 三层一起调）。跑赛道就是这个。' },
  { value: MODE.WHEELS, label: '两轮直给', hint: '绕过所有调节器，左右轮各拿一个油门值直接转 —— 唯一能左右分开给（还能倒车）的模式，"原地转"和"斜着蹭障碍"都靠它。' },
]

/** 不在下拉里的旧值 → 人话（解析老文件用；执行语义固件一行没改） */
export const MODE_LEGACY: Choice[] = [
  { value: MODE.TRACE, label: '（旧）循迹环+速度环', hint: '两级循迹（位置环 + 速度环，没有电流环）。老文件里还有这个值，新流程请改用「循迹」。' },
]

/** 模式值 → 人话：先查下拉，再查旧值，最后退回数字 */
export function modeLabel(v: number): string {
  return MODE_OPTIONS.find((m) => m.value === v)?.label ?? MODE_LEGACY.find((m) => m.value === v)?.label ?? `模式 ${v}`
}

/** 模式值 → 这个模式到底把车怎么控制（选中项下面那行小字、以及鼠标停在选框上的提示） */
export function modeHint(v: number): string {
  return MODE_OPTIONS.find((m) => m.value === v)?.hint ?? MODE_LEGACY.find((m) => m.value === v)?.hint ?? ''
}

/** 条件号 → 这个条件在等什么（同上） */
export function condHint(v: number): string {
  return COND_BY_CODE.get(v)?.hint ?? ''
}

/** 下拉要显示的项：三个标准模式；若当前值不在其中（老文件用 2/4）就临时补一条，别让选框空着 */
export function modeChoices(cur: number): Choice[] {
  if (MODE_OPTIONS.some((m) => m.value === cur)) return MODE_OPTIONS
  return [...MODE_OPTIONS, ...MODE_LEGACY.filter((m) => m.value === cur)]
}

/** 状态的显示名：`状态 3 台阶2`（任务里给状态起的名字）。没有名字就只写编号。 */
function stateLabel(i: number, names?: string[]): string {
  const n = names?.[i]
  return n ? `状态 ${i} ${n}` : `状态 ${i}`
}

/** 「目标状态」下拉要显示的项：真实状态 0..stateCount-1；allowContinue（只有「等条件」的"超时后"）再加一条
 *  「继续往下跑」= 0xFFFF = 超时也不改道。当前值不在列表里（老文件把跳转目标存成了 0xFFFF，或目标状态
 *  后来被删了）就临时补一条标出来的项，别让选框空着 —— 固件对越界/0xFFFF 的跳转是"跳过这一块"，不会写坏程序。
 *  names 传进来时，每一项会带上状态名（光看编号根本不知道要跳到哪儿去）。 */
export function stateChoices(cur: number, stateCount: number, allowContinue: boolean, names?: string[]): { value: number; label: string }[] {
  const out: { value: number; label: string }[] = []
  if (allowContinue) out.push({ value: NO_STATE, label: '继续往下跑' })
  for (let i = 0; i < stateCount; i++) out.push({ value: i, label: stateLabel(i, names) })
  if (!out.some((o) => o.value === cur)) {
    out.push({ value: cur, label: cur === NO_STATE ? '（旧）继续往下跑' : `${stateLabel(cur, names)}（不存在）` })
  }
  return out
}

/** 该块默认长什么样（从块库拖进序列时用这个） */
export function defaultsFor(op: number): TaskBlock {
  switch (op) {
    case OP.SET_SPEED:
      return blk(OP.SET_SPEED, 6480)
    case OP.SET_MODE:
      return blk(OP.SET_MODE, MODE.TRACE_GYRO)
    case OP.TURN_DEG:
      return blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1)
    case OP.ACTION:
      return blk(OP.ACTION, 1, 0, 0)
    case OP.SET_SERVO:
      return blk(OP.SET_SERVO, 1, 90)
    case OP.WAIT_MS:
      return blk(OP.WAIT_MS, 500)
    case OP.WAIT_COND:
      return blk(OP.WAIT_COND, COND.CROSS, 0, 3000, NO_STATE)
    case OP.CROSS_CLEAR:
      return blk(OP.CROSS_CLEAR)
    case OP.GOTO:
      return blk(OP.GOTO, 0)
    case OP.END:
      return blk(OP.END)
    default:
      return blk(op)
  }
}

/** 油门刻度 → "约百分之多少"，给用户一个能对上感觉的数字 */
export function speedPct(v: number): string {
  return `${v < 0 ? '倒车 ' : ''}约 ${Math.round((Math.abs(v) / 7200) * 100)}% 油门`
}

/** 一行摘要（列表、块卡片用）。同样只写人话：该带单位的带上，别只丢一个裸数字。
 *  names = 状态名（跳到哪个状态用名字说，别只给编号）。 */
export function describeBlock(b: TaskBlock, stateCount: number, names?: string[]): string {
  const st = (n: number) => (n === NO_STATE ? '继续往下跑' : n < stateCount ? stateLabel(n, names) : `状态 ${n}（已不存在）`)
  switch (b.op) {
    case OP.SET_SPEED:
      return b.p0 ? `速度 ${b.p0}（${speedPct(b.p0)}）` : '速度 0（停）'
    case OP.SET_MODE:
      return b.p0 === MODE.WHEELS ? `两轮直给 左 ${b.p1} 右 ${b.p2}` : `模式：${modeLabel(b.p0)}`
    case OP.TURN_DEG:
      return `${b.flags & 1 ? '从当前朝向转' : '转到固定角度'} ${b.p1}°（±${(b.p2 / 10).toFixed(1)}° 内算到位，最多 ${b.p0}ms）`
    case OP.ACTION:
      return `摆第 ${b.p0} 套舵机动作${b.p2 ? `，然后等 ${b.p2}ms` : ''}`
    case OP.SET_SERVO:
      return `${b.p0} 号舵机 → ${b.p1}°`
    case OP.WAIT_MS:
      return `等 ${b.p0}ms${b.p0 >= 1000 ? `（${(b.p0 / 1000).toFixed(2).replace(/\.?0+$/, '')} 秒）` : ''}`
    case OP.WAIT_COND:
      return `等「${COND_BY_CODE.get(b.p0)?.label ?? `条件 ${b.p0}`}」${b.p2 ? `最多 ${b.p2}ms` : '不限时'}${b.p3 !== NO_STATE ? `，超时后跳到 ${st(b.p3)}` : ''}`
    case OP.CROSS_CLEAR:
      return '把横线计数清零'
    case OP.GOTO:
      return `跳到 ${st(b.p0)}`
    case OP.END:
      return '结束任务（停车）'
    default:
      return '这块网页不认识（固件会当非法程序拒收）'
  }
}

/** 一个字段**当前值**的人话解释：下拉项跟着选中的那一项走（用户 m25776 要的就是这个） */
export function fieldHint(f: FieldSpec, b: TaskBlock): string {
  if (f.kind === 'cond') return condHint(b[f.key]) || f.hint
  if (f.kind === 'mode') return modeHint(b[f.key]) || f.hint
  return f.hint
}

/** 一个下拉的**完整选项说明**：鼠标停在选框上时，原生提示框里逐行列出每个选项是什么意思 */
export function fieldLegend(f: FieldSpec, b: TaskBlock, stateCount: number, names?: string[]): string {
  if (f.kind === 'cond') return COND_SPECS.map((c) => `${c.label}：${c.hint}`).join('\n')
  if (f.kind === 'mode') return modeChoices(b[f.key]).map((m) => `${m.label}：${m.hint}`).join('\n')
  if (f.kind === 'state') {
    const rows = stateChoices(b[f.key], stateCount, !!f.allowNoState, names).map((o) => o.label)
    rows.push(f.allowNoState ? '「继续往下跑」= 超时也不改道，接着执行下一行' : '只能选真实状态')
    return rows.join('\n')
  }
  if (f.kind === 'servo') return '1 号 / 2 号：车上那两路舵机通道'
  return f.hint
}

/* ------------------------------------------------------------------ 名字 */

/** 按**字节**截断（格式里名字是 UTF-8 字节串；中文一个字 3 字节，数错就超限） */
export function truncateNameUtf8(s: string, maxBytes = TASK_MAX_NAME): string {
  const enc = new TextEncoder()
  if (enc.encode(s).length <= maxBytes) return s
  let out = ''
  let n = 0
  for (const ch of s) {
    const len = enc.encode(ch).length
    if (n + len > maxBytes) break
    out += ch
    n += len
  }
  return out
}

export function nameBytes(s: string): number {
  return new TextEncoder().encode(s).length
}

/* ------------------------------------------------------------------ 拖拽 */

export type SeqName = 'enter' | 'body'

/** 序列里的一个**插入位置**：index 可以等于该序列长度（放到末尾） */
export interface Where {
  state: number
  seq: SeqName
  index: number
}

export type DragPayload = { kind: 'new'; op: number } | { kind: 'move'; from: Where }

/** 拖拽时挂在 dataTransfer 上的自定义 MIME（同页面内拖拽，不跨窗口） */
export const DRAG_MIME = 'application/x-hxb-task'

export function encodeDrag(p: DragPayload): string {
  return JSON.stringify(p)
}

export function decodeDrag(raw: string): DragPayload | null {
  try {
    const o = JSON.parse(raw) as DragPayload
    if (!o || typeof o !== 'object') return null
    if (o.kind === 'new' && typeof o.op === 'number' && OP_BY_CODE.has(o.op)) return { kind: 'new', op: o.op }
    if (
      o.kind === 'move' &&
      o.from &&
      typeof o.from.state === 'number' &&
      (o.from.seq === 'enter' || o.from.seq === 'body') &&
      typeof o.from.index === 'number'
    ) {
      return { kind: 'move', from: o.from }
    }
    return null
  } catch {
    return null
  }
}

function cloneProg(p: TaskProgram): TaskProgram {
  return JSON.parse(JSON.stringify(p)) as TaskProgram
}

function seqOf(p: TaskProgram, w: Where): TaskBlock[] | null {
  const st = p.states[w.state]
  if (!st) return null
  return w.seq === 'enter' ? st.enter : st.body
}

function clampIndex(i: number, len: number): number {
  if (!Number.isFinite(i)) return len
  return Math.max(0, Math.min(len, Math.round(i)))
}

/** 在 `at` 处插入一个块 */
export function insertBlock(p: TaskProgram, at: Where, b: TaskBlock): TaskProgram {
  const out = cloneProg(p)
  const seq = seqOf(out, at)
  if (!seq) return p
  seq.splice(clampIndex(at.index, seq.length), 0, { ...b })
  return out
}

/** 删除 `at.index` 处的块 */
export function removeBlock(p: TaskProgram, at: Where): TaskProgram {
  const out = cloneProg(p)
  const seq = seqOf(out, at)
  if (!seq || at.index < 0 || at.index >= seq.length) return p
  seq.splice(at.index, 1)
  return out
}

/** 就地改一个块（`patch` 里带 op 的话就是换块类型，字段保留） */
export function updateBlock(p: TaskProgram, at: Where, patch: Partial<TaskBlock>): TaskProgram {
  const out = cloneProg(p)
  const seq = seqOf(out, at)
  if (!seq || at.index < 0 || at.index >= seq.length) return p
  seq[at.index] = { ...seq[at.index], ...patch }
  return out
}

/** 复制一个块，插到它后面 */
export function duplicateBlock(p: TaskProgram, at: Where): TaskProgram {
  const seq = seqOf(p, at)
  if (!seq || at.index < 0 || at.index >= seq.length) return p
  return insertBlock(p, { ...at, index: at.index + 1 }, seq[at.index])
}

/**
 * 移动一个块。**先删后插要补偿下标**：同一序列里往右移时，删掉之后目标位置左移 1，
 * 不补偿就会差一格（界面上表现为"往右拖少走一位"）。
 */
export function moveBlock(p: TaskProgram, from: Where, to: Where): TaskProgram {
  const out = cloneProg(p)
  const src = seqOf(out, from)
  if (!src || from.index < 0 || from.index >= src.length) return p
  const [b] = src.splice(from.index, 1)
  const dst = seqOf(out, to)
  if (!dst) return p
  let j = to.index
  if (from.state === to.state && from.seq === to.seq && j > from.index) j -= 1
  dst.splice(clampIndex(j, dst.length), 0, b)
  return out
}

/** 拖放的统一入口：块库拖来的是"新块"，拖动已有块的是"移动" */
export function dropPayload(p: TaskProgram, at: Where, d: DragPayload): TaskProgram {
  return d.kind === 'new' ? insertBlock(p, at, defaultsFor(d.op)) : moveBlock(p, d.from, at)
}

/* ------------------------------------------------------------------ 状态 */

export function addState(p: TaskProgram, name = '新状态'): TaskProgram {
  if (p.states.length >= TASK_MAX_STATES) return p
  const out = cloneProg(p)
  out.states.push({ name: truncateNameUtf8(name), enter: [], body: [] })
  return out
}

export function cloneState(p: TaskProgram, i: number, name?: string): TaskProgram {
  if (p.states.length >= TASK_MAX_STATES || !p.states[i]) return p
  const out = cloneProg(p)
  const src = out.states[i]
  out.states.splice(i + 1, 0, {
    name: truncateNameUtf8(name ?? `${src.name}-副本`),
    enter: JSON.parse(JSON.stringify(src.enter)) as TaskBlock[],
    body: JSON.parse(JSON.stringify(src.body)) as TaskBlock[],
  })
  return out
}

/**
 * 删掉一个状态，并把指向它的跳转收拾干净：
 *  - 目标号 > i 的减 1（后面的状态整体前移）；
 *  - 目标号 == i 的落到主页（没地方可跳了，回主页最不容易出事）；
 *  - 主页被删则顺移，最后夹进范围。
 */
export function removeState(p: TaskProgram, i: number): TaskProgram {
  if (p.states.length <= 1 || !p.states[i]) return p
  const out = cloneProg(p)
  out.states.splice(i, 1)
  const fallback = Math.min(i, out.states.length - 1)
  const fix = (ref: number): number => (ref === i ? fallback : ref > i ? ref - 1 : ref)
  /* 只修真正指向某个状态的引用：0xFFFF（"继续往下跑"）与越界的占位值（老文件）原样留着，
     免得 0xFFFF 被减成 0xFFFE、越修越偏 */
  const fixRef = (ref: number): number => (ref === NO_STATE || ref >= out.states.length + 1 ? ref : fix(ref))
  for (const st of out.states) {
    for (const b of [...st.enter, ...st.body]) {
      if (b.op === OP.GOTO) b.p0 = fixRef(b.p0)
      if (b.op === OP.WAIT_COND) b.p3 = fixRef(b.p3)
    }
  }
  out.main = i < out.main ? out.main - 1 : i === out.main ? fallback : out.main
  out.main = Math.max(0, Math.min(out.main, out.states.length - 1))
  return out
}

export function renameState(p: TaskProgram, i: number, name: string): TaskProgram {
  if (!p.states[i]) return p
  const out = cloneProg(p)
  out.states[i].name = truncateNameUtf8(name)
  return out
}

export function setMain(p: TaskProgram, i: number): TaskProgram {
  if (!p.states[i]) return p
  const out = cloneProg(p)
  out.main = i
  return out
}

/** 序列摊平成"带编号的行"（界面与测试共用同一套编号） */
export function seqRows(p: TaskProgram, i: number, seq: SeqName): { index: number; block: TaskBlock; isLast: boolean }[] {
  const list = (seq === 'enter' ? p.states[i]?.enter : p.states[i]?.body) ?? []
  return list.map((block, index) => ({ index, block, isLast: index === list.length - 1 }))
}

export function blockCount(p: TaskProgram): number {
  return p.states.reduce((n, s) => n + s.enter.length + s.body.length, 0)
}

/* --------------------------------------------------- 编译信息（预览面板） */

export interface CompileInfo {
  len: number
  crc16: number
  errors: string[]
  warnings: string[]
  /** 该任务占车上 6KB 程序区的百分比（估算：块区+名字区，不含状态表） */
  budgetPct: number
}

/** 编译预览：长度、CRC、错误/警告。**有错也照编**（草稿要能存），烧写前看 errors 空不空 */
export function compileInfo(p: TaskProgram): CompileInfo {
  const { bytes, errors } = encodeTask(p)
  const { warnings } = validateTask(p)
  return {
    len: bytes.length,
    // 与车上一致：CRC 只覆盖 [12, len) —— 整块算出来的数不是车回读时报的那个
    crc16: crc16Ccitt(bytes.subarray(TASK_HDR_LEN)),
    errors,
    warnings,
    budgetPct: Math.min(100, Math.round((bytes.length / 6144) * 100)),
  }
}

/* ---------------------------------------------------------------- 存盘 */

export function newTaskDef(id: number, name: string): TaskDef {
  return {
    id,
    name,
    prog: {
      main: 0,
      states: [
        {
          name: '常态循迹',
          enter: [blk(OP.SET_MODE, MODE.TRACE_GYRO)],
          body: [blk(OP.SET_SPEED, 6480), blk(OP.WAIT_COND, COND.CROSS, 0, 3000, 1), blk(OP.GOTO, 0)],
        },
        {
          name: '路口转向',
          enter: [blk(OP.WAIT_MS, 200)],
          body: [blk(OP.TURN_DEG, 2000, -90, 10, NO_STATE, 1), blk(OP.GOTO, 1)],
        },
      ],
    },
  }
}

export function defaultTaskFile(): TaskFileJSON {
  return { ver: 1, tasks: [newTaskDef(0, '常态循迹')] }
}

export function nextTaskId(tasks: TaskDef[]): number {
  for (let i = 0; i < EDIT_MAX_TASKS; i += 1) if (!tasks.some((t) => t.id === i)) return i
  return -1
}

/** 追加一个任务：id 取 0..EDIT_MAX_TASKS-1 里第一个空位，满了就原样返回（调用方自己提示）。 */
export function addTaskDef(tasks: TaskDef[], task: TaskDef, name = task.name): TaskDef[] {
  const id = nextTaskId(tasks)
  if (id < 0) return tasks
  return [...tasks, sanitizeTaskDef({ id, name, prog: task.prog }, id)!]
}

/* ------------------------------------------------- 读回来的东西一律当脏数据 */

function num(v: unknown, def: number, lo = -1e9, hi = 1e9): number {
  const n = typeof v === 'number' && Number.isFinite(v) ? Math.round(v) : def
  return Math.max(lo, Math.min(hi, n))
}

export function sanitizeBlock(raw: unknown): TaskBlock {
  const o = (raw ?? {}) as Partial<TaskBlock>
  const op = num(o.op, 0, 0, 0xff)
  if (!OP_BY_CODE.has(op)) return defaultsFor(OP.WAIT_MS)
  return {
    op,
    flags: num(o.flags, 0, 0, 0xffff),
    p0: num(o.p0, 0, -0x8000, 0xffff),
    p1: num(o.p1, 0, -0x8000, 0x7fff),
    p2: num(o.p2, 0, -0x8000, 0xffff),
    p3: num(o.p3, NO_STATE, -0x8000, 0xffff),
  }
}

export function sanitizeProgram(raw: unknown): TaskProgram {
  const o = (raw ?? {}) as Partial<TaskProgram>
  const statesRaw = Array.isArray(o.states) ? o.states : []
  const states: TaskState[] = statesRaw.slice(0, TASK_MAX_STATES).map((s) => {
    const st = (s ?? {}) as Partial<TaskState>
    return {
      name: truncateNameUtf8(typeof st.name === 'string' ? st.name : ''),
      enter: (Array.isArray(st.enter) ? st.enter : []).slice(0, TASK_MAX_BLOCKS).map(sanitizeBlock),
      body: (Array.isArray(st.body) ? st.body : []).slice(0, TASK_MAX_BLOCKS).map(sanitizeBlock),
    }
  })
  if (!states.length) states.push({ name: '常态循迹', enter: [], body: [] })
  return { states, main: num(o.main, 0, 0, states.length - 1) }
}

export function sanitizeTaskDef(raw: unknown, fallbackId: number): TaskDef | null {
  if (!raw || typeof raw !== 'object') return null
  const o = raw as Partial<TaskDef>
  if (!o.prog) return null
  return {
    id: num(o.id, fallbackId, 0, EDIT_MAX_TASKS - 1),
    name: truncateNameUtf8(typeof o.name === 'string' && o.name ? o.name : `任务${fallbackId}`),
    prog: sanitizeProgram(o.prog),
  }
}

/** 编号去重（同一个 id 出现两次就往后挪） */
export function dedupeIds(tasks: TaskDef[]): TaskDef[] {
  const used = new Set<number>()
  return tasks.map((t) => {
    let id = t.id
    while (used.has(id)) id = nextTaskId([...used].map((u) => ({ id: u } as TaskDef)))
    used.add(id)
    return { ...t, id }
  })
}

/** 从（可能是坏的/旧版的）localStorage 内容读回任务表；读不出来就给一份能用的默认值 */
export function loadTaskFile(raw: string | null): TaskFileJSON {
  if (!raw) return defaultTaskFile()
  try {
    const o = JSON.parse(raw) as { tasks?: unknown }
    const list = Array.isArray(o?.tasks) ? o.tasks : []
    const tasks = dedupeIds(
      list
        .slice(0, EDIT_MAX_TASKS)
        .map((t, i) => sanitizeTaskDef(t, i))
        .filter((t): t is TaskDef => !!t),
    )
    return tasks.length ? { ver: 1, tasks } : defaultTaskFile()
  } catch {
    return defaultTaskFile()
  }
}

export function serializeTaskFile(f: TaskFileJSON): string {
  return JSON.stringify({ ver: 1, tasks: f.tasks }, null, 1)
}

/** 从别人给的 JSON 文件导入（坏的丢掉而不是炸掉，返还丢了几条） */
export function importTaskFile(text: string): { tasks: TaskDef[]; skipped: number } {
  try {
    const o = JSON.parse(text) as { tasks?: unknown }
    const list = Array.isArray(o?.tasks) ? o.tasks : Array.isArray(o) ? (o as unknown[]) : []
    const tasks = list
      .map((t, i) => sanitizeTaskDef(t, i))
      .filter((t): t is TaskDef => !!t)
      .slice(0, EDIT_MAX_TASKS)
    return { tasks: dedupeIds(tasks), skipped: Math.max(0, list.length - tasks.length) }
  } catch {
    return { tasks: [], skipped: 0 }
  }
}
