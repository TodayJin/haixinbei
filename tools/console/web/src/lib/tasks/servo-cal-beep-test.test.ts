/**
 * 真车自测任务 `servo-cal-beep-test.json`：只验两件当晚刚做完、还没在真车上跑过的事 ——
 *   ① 任务块 `CAL_SET`(0x0B) 真的会切灰度标定套；② 「过点蜂鸣」每切一次状态响一声。
 *
 * 这里钉死四件事，防止以后改块/改编码把这些性质悄悄改掉：
 *   ① 走**页面「导入」的真实入口**（`importTaskFile`）能认，且存回再导入不走样；
 *   ② `validateTask()` **errors 与 warnings 都为空**（否则页面点「烧写」会被拦下）；
 *   ③ 编码后的**字节数 / CRC / 状态数 / 块数**（尺寸护栏：改了块或名字就会在这里报到）；
 *   ④ ★**不含任何会驱动轮子或舵机的块** —— 用的是白名单（只允许 CAL_SET/WAIT_MS/GOTO/END），
 *      而不是黑名单：以后新增了别的"会动的"块，这条会自然红。
 *
 * ⚠ 这条任务的**真车结论**（听到几声、标定切没切）只能由人在车上给；本文件只保证"编出来的东西
 *   与设计一致、且不会让车动"。固件侧的同一份字节另有独立验证：`TaskProg_Validate` 见报告。
 */
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { OP, crc16Ccitt, decodeTask, encodeTask, validateTask, type TaskBlock } from '../task'
import { importTaskFile, loadTaskFile, serializeTaskFile } from '../taskedit'

const RAW = readFileSync(new URL('./servo-cal-beep-test.json', import.meta.url), 'utf8')
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

describe('自测任务：切灰度标定套 + 过点蜂鸣（servo-cal-beep-test.json）', () => {
  it('能当普通任务文件导入（页面「导入」按钮走的就是这条路）', () => {
    expect(file.skipped).toBe(0)
    expect(file.tasks).toHaveLength(1)
    expect(task.id).toBe(0)
    expect(task.name).toContain('自测')
    expect(prog.main).toBe(0)
    // 任务名要进 Flash 页 58 的目录记录（上限 30 字节）
    expect(new TextEncoder().encode(task.name).length).toBeLessThanOrEqual(30)
    // 注释性键（_note/_op_audit）是给人看的，导入时会被 sanitize 丢掉 —— 但文件里必须在
    expect(RAW).toContain('_note')
    expect(RAW).toContain('_op_audit')
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
    // 轮子相关的四个 op 一个都不能有
    for (const op of [OP.SET_SPEED, OP.SET_MODE, OP.TURN_DEG]) {
      expect(allBlocks.some((b) => b.op === op)).toBe(false)
    }
    // 舵机相关的两个 op 一个都不能有
    for (const op of [OP.ACTION, OP.SET_SERVO]) {
      expect(allBlocks.some((b) => b.op === op)).toBe(false)
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
