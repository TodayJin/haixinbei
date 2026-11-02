/**
 * 真车自测任务 `servo-cal-beep-test.json`：只验两件当晚刚做完、还没在真车上跑过的事 ——
 *   ① 任务块 `CAL_SET`(0x0B) 真的会切灰度标定套；② 「过点蜂鸣」每切一次状态响一声。
 *
 * 这里钉死五件事，防止以后改块/改编码/改文件形状把这些性质悄悄改掉：
 *   ① 走**页面「导入」的真实入口**（`importTaskFile`）能认，且存回再导入不走样；
 *   ② ★**文件的形状与参照物 `renwu-lap.json` 完全同构**（顶层 `{ver,tasks}`、
 *      任务 `{id,name,prog}`、程序 `{states,main}`），而且**文件内容 === 编辑器「导出」出来的文本**
 *      —— 见 `it('★形状…')` 与 `it('★文件就是编辑器导出的规范形态…')`；
 *   ③ `validateTask()` **errors 与 warnings 都为空**（否则页面点「烧写」会被拦下）；
 *   ④ 编码后的**字节数 / CRC / 状态数 / 块数**（尺寸护栏：改了块或名字就会在这里报到）；
 *   ⑤ ★**不含任何会驱动轮子或舵机的块** —— 用的是白名单（只允许 CAL_SET/WAIT_MS/GOTO/END），
 *      而不是黑名单：以后新增了别的"会动的"块，这条会自然红。
 *
 * **2026-10-07 BUG-1（用户实测「导入后编辑器里看不到它的块」）**：这份文件原来带着三个
 * **非任务格式的注释键**（顶层 `_note` / `_op_audit` + 任务里的 `_note`），顶层键是
 * `['_note','_op_audit','ver','tasks']` —— 与能正常导入的 `renwu-lap.json`
 * （`['ver','tasks']`）形状不一致。现在这些注释键**全部删掉**，文件重写成参照物的形状
 * （`id` 也从 0 改到 **3**：车上现有 #0 常态循迹 / #1 旧全程11障碍 / #2 测试，别再撞号）。
 * 那三条注释里值得留的信息搬到这里：
 *   · 为什么这么设计：固件 `seq_begin()` 每进一个状态就 `AppUi_Beep(20)` 响一声 ⇒
 *     10 个状态 = 10 声，间隔 1000ms，第 N 声 ⇔ 遥测 `state_id` = N−1 ⇔ 标定被切到该状态名里写的套；
 *   · 标定 0 = 普通（宝丽布）、1 = 亚克力；顺序 0→1→…→0，**跑完停在「普通」**，不留副作用；
 *   · 已知无害现象：`trace_cal_select_set()` 里有 `if (v == s_set) return;` ⇒ 若车本来就在
 *     「普通」套，状态 0 的 `CAL_SET 0` 是空操作（蜂鸣照响），第一次看得见的切换在状态 1。
 *   · `_op_audit` 的结论就是下面那个白名单用例：只有 `0x0B/0x05/0x07/0x08` 四类，
 *     `0x01/02/03/04/06/09/0A` 一个都没有。
 *
 * ⚠ 这条任务的**真车结论**（听到几声、标定切没切）只能由人在车上给；本文件只保证"编出来的东西
 *   与设计一致、且不会让车动"。固件侧的同一份字节另有独立验证：`TaskProg_Validate` 见报告。
 */
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { OP, crc16Ccitt, decodeTask, encodeTask, validateTask, type TaskBlock } from '../task'
import { importTaskFile, loadTaskFile, serializeTaskFile } from '../taskedit'

const RAW = readFileSync(new URL('./servo-cal-beep-test.json', import.meta.url), 'utf8')
/** 参照物：能被正常导入、且形状就是"任务文件该有的样子"的那份（旧全程 11 障碍） */
const REF = readFileSync(new URL('./renwu-lap.json', import.meta.url), 'utf8')
const file = importTaskFile(RAW)
const task = file.tasks[0]
const prog = task.prog
const states = prog.states
const allBlocks: TaskBlock[] = states.flatMap((s) => [...s.enter, ...s.body])

/** 只允许这四类块（少一个都不行：本任务的卖点就是"什么都不动"） */
const ALLOWED_OPS = new Set<number>([OP.CAL_SET, OP.WAIT_MS, OP.GOTO, OP.END])
/** 明确不许出现的块（会驱动轮子/舵机、或依赖运动的判断） */
const FORBIDDEN = ['SET_SPEED', 'SET_MODE', 'TURN_DEG', 'ACTION', 'SET_SERVO', 'WAIT_COND', 'CROSS_CLEAR']
const OP_NAME = new Map<number, string>(Object.entries(OP).map(([k, v]) => [v as number, k]))

/** 编码后的期望值（改了任务或编码格式，这里第一条就会红） */
const EXPECT_LEN = 501
const EXPECT_CRC16 = 0xd897
const EXPECT_STATES = 10
const EXPECT_BLOCKS = 28
const EXPECT_WAIT_MS = 1000

/** 行尾与末尾换行不影响"是不是同一份 JSON 文本"，比对前统一掉 */
const norm = (s: string): string => s.replace(/\r\n/g, '\n').replace(/\n+$/, '')

describe('自测任务：切灰度标定套 + 过点蜂鸣（servo-cal-beep-test.json）', () => {
  it('能当普通任务文件导入（页面「导入」按钮走的就是这条路）', () => {
    expect(file.skipped).toBe(0)
    expect(file.tasks).toHaveLength(1)
    // id 3：车上现有 #0 常态循迹 / #1 旧全程11障碍 / #2 测试 ⇒ 不与它们撞号
    expect(task.id).toBe(3)
    expect(task.name).toContain('自测')
    expect(prog.main).toBe(0)
    // 任务名要进 Flash 页 58 的目录记录（上限 30 字节）
    expect(new TextEncoder().encode(task.name).length).toBeLessThanOrEqual(30)
  })

  it('★形状与参照物同构：顶层 {ver,tasks} / 任务 {id,name,prog} / 程序 {states,main}', () => {
    // ① 键名逐项钉死（BUG-1 就是顶层多了 _note/_op_audit 这类非格式键）
    expect(Object.keys(JSON.parse(RAW))).toEqual(['ver', 'tasks'])
    expect(Object.keys(JSON.parse(RAW).tasks[0])).toEqual(['id', 'name', 'prog'])
    expect(Object.keys(JSON.parse(RAW).tasks[0].prog)).toEqual(['states', 'main'])
    // ② 与参照物**逐键对齐**（两份文件的形状不许漂开）
    const ref = JSON.parse(REF) as { tasks: Array<Record<string, unknown>> }
    expect(Object.keys(JSON.parse(RAW))).toEqual(Object.keys(JSON.parse(REF) as object))
    expect(Object.keys(JSON.parse(RAW).tasks[0])).toEqual(Object.keys(ref.tasks[0]))
    expect(Object.keys(JSON.parse(RAW).tasks[0].prog)).toEqual(
      Object.keys(ref.tasks[0].prog as object),
    )
    // ③ 块字段也只有 6 个（参照物的块长这样）
    const refBlock = (ref.tasks[0].prog as { states: Array<{ body: Array<Record<string, unknown>> }> })
      .states.flatMap((s) => s.body)[0]
    expect(Object.keys(allBlocks[0])).toEqual(Object.keys(refBlock))
    // ④ 反面判据：这几个键在文件里一个都不许再有
    for (const k of ['_note', '_op_audit']) expect(RAW).not.toContain(k)
  })

  it('★文件就是编辑器导出的规范形态（内容 === serializeTaskFile 的输出）', () => {
    expect(norm(RAW)).toBe(norm(serializeTaskFile({ ver: 1, tasks: [task] })))
  })

  it('过校验：没有 error，也没有 warning', () => {
    const { errors, warnings } = validateTask(prog)
    expect(errors).toEqual([])
    expect(warnings).toEqual([])
  })

  it('★不含任何会驱动轮子或舵机的块（白名单口径）', () => {
    for (const b of allBlocks) {
      const name = OP_NAME.get(b.op) ?? `0x${b.op.toString(16)}`
      expect(ALLOWED_OPS.has(b.op), `出现了不该有的块：${name}`).toBe(true)
    }
    const used = new Set(allBlocks.map((b) => OP_NAME.get(b.op)))
    for (const f of FORBIDDEN) expect(used.has(f), `不该出现 ${f}`).toBe(false)
    // 轮子相关的三个 op 一个都不能有
    for (const op of [OP.SET_SPEED, OP.SET_MODE, OP.TURN_DEG]) {
      expect(allBlocks.some((b) => b.op === op)).toBe(false)
    }
    // 舵机相关的两个 op 一个都不能有
    for (const op of [OP.ACTION, OP.SET_SERVO]) {
      expect(allBlocks.some((b) => b.op === op)).toBe(false)
    }
    // ★0x0B(CAL_SET) 出现 9 次；0x01/02/03/04/06/09/0A 都是 0（= 不动轮子/舵机）
    const hist = new Map<number, number>()
    for (const b of allBlocks) hist.set(b.op, (hist.get(b.op) ?? 0) + 1)
    expect(hist.get(OP.CAL_SET)).toBe(9)
    for (const op of [OP.SET_SPEED, OP.SET_MODE, OP.TURN_DEG, OP.ACTION, OP.WAIT_COND, OP.SET_SERVO, OP.CROSS_CLEAR]) {
      expect(hist.get(op) ?? 0, `${OP_NAME.get(op)} 不该出现`).toBe(0)
    }
  })

  it('编码后：10 状态 / 28 块 / 501 B / CRC 0xD897（尺寸与格式护栏）', () => {
    const enc = encodeTask(prog)
    expect(enc.errors).toEqual([])
    expect(enc.bytes.length).toBe(EXPECT_LEN)
    expect(enc.bytes[3]).toBe(EXPECT_STATES)
    expect(states).toHaveLength(EXPECT_STATES)
    expect(allBlocks).toHaveLength(EXPECT_BLOCKS)
    expect(enc.bytes[4]).toBe(0) // main
    // 头部字段：magic / ver / total_len / CRC
    expect(enc.bytes[0] | (enc.bytes[1] << 8)).toBe(0x4b54)
    expect(enc.bytes[2]).toBe(1)
    expect(enc.bytes[8] | (enc.bytes[9] << 8)).toBe(EXPECT_LEN)
    expect(enc.bytes[6] | (enc.bytes[7] << 8)).toBe(EXPECT_CRC16)
    expect(crc16Ccitt(enc.bytes.subarray(12))).toBe(EXPECT_CRC16)
    // 单条上限 4096：这条只占 1/8，随时能再塞别的测试
    expect(enc.bytes.length).toBeLessThanOrEqual(4096)
  })

  it('往返：decodeTask(encodeTask(p)) 与原件一字不差', () => {
    const dec = decodeTask(encodeTask(prog).bytes)
    expect(dec.error).toBe('')
    expect(dec.program).toEqual(prog)
  })

  it('每切一次状态响一声：状态数 = 蜂鸣次数，且每声之间等够 800ms 以上', () => {
    // 固件 seq_begin() 在**每次进入状态**时蜂鸣 ⇒ 声数 = 1（启动进 main）+ GOTO 次数
    const gotos = allBlocks.filter((b) => b.op === OP.GOTO)
    expect(gotos).toHaveLength(EXPECT_STATES - 1)
    expect(states.filter((s) => s.name === '').length).toBe(0) // 每个状态都有名字，界面/监视里能对上
    for (let i = 0; i < states.length; i += 1) {
      const body = states[i].body
      expect(body.length).toBeGreaterThan(0)
      const tail = body[body.length - 1]
      // 固件 tail_ok()：body 最后一块必须是「跳到状态 / 结束 / 带超时跳转的等待」
      expect([OP.GOTO, OP.END]).toContain(tail.op)
      if (i < states.length - 1) expect(tail.p0).toBe(i + 1) // 一环扣一环，没有回头/空转
    }
    const waits = allBlocks.filter((b) => b.op === OP.WAIT_MS)
    expect(waits).toHaveLength(EXPECT_STATES - 1)
    for (const w of waits) expect(w.p0).toBeGreaterThanOrEqual(800)
    expect(waits.every((w) => w.p0 === EXPECT_WAIT_MS)).toBe(true)
    // 整条任务 ≈ 9s，不至于让人等太久
    expect(waits.reduce((a, b) => a + b.p0, 0)).toBe(9000)
  })

  it('标定套是 0/1 交替、最后回到普通（跑完不留副作用）', () => {
    const sets = allBlocks.filter((b) => b.op === OP.CAL_SET)
    expect(sets.map((b) => b.p0)).toEqual([0, 1, 0, 1, 0, 1, 0, 1, 0])
    for (const b of sets) expect([0, 1]).toContain(b.p0) // CAL_SET 的 p0 只认 0/1
    expect(sets[sets.length - 1].p0).toBe(0)
    // 每个状态自己带一次切换（进门就切），所以"第 N 声" ⇔ "切到第 N 个状态名里写的套"
    for (let i = 0; i < states.length - 1; i += 1) {
      expect(states[i].body[0].op).toBe(OP.CAL_SET)
      expect(states[i].body[0].p0).toBe(i % 2 === 0 ? 0 : 1)
    }
    // 最后一个状态只干一件事：结束
    expect(states[states.length - 1].body.map((b) => b.op)).toEqual([OP.END])
  })

  it('存下来再导入还是同一份（页面「导出 → 导入」不会走样）', () => {
    const back = loadTaskFile(serializeTaskFile({ ver: 1, tasks: [task] }))
    expect(back).toEqual({ ver: 1, tasks: [task] })
  })
})
