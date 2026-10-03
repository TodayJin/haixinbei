/**
 * 旧内置比赛流程（已删的 `firmware/App/Svc/renwu.c`，12 个障碍）转成任务块的产物。
 * 这个 JSON 是**给用户导入的交付物**，所以在这里钉住四件事：
 *  ① 走一遍「导入文件」的真实路径（`loadTaskFile`）能认；
 *  ② 过 `validateTask`（否则页面点"烧写"会被拦下）；
 *  ③ 编码后长度 ≤ `TASK_STORE_CAP`（4096；同时也断言它 **> 2048**，
 *     因为正是"忠实转换装不下"才把上限从 2048 提到 4096 的）；
 *  ④ 几个关键块还在（两轮直给 / 右转 90° / 动作 2 后等 2050ms / 状态尾都是 GOTO 或 END），
 *     防止以后改瘦身规则时把语义悄悄删掉。
 */
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { COND, MODE, OP, encodeTask, validateTask, type TaskBlock } from '../task'
import { loadTaskFile, serializeTaskFile } from '../taskedit'

const file = loadTaskFile(readFileSync(new URL('./renwu-lap.json', import.meta.url), 'utf8'))
const task = file.tasks[0]
const prog = task.prog
const states = prog.states
const allBlocks: TaskBlock[] = states.flatMap((s) => [...s.enter, ...s.body])

describe('旧全程流程转出来的任务文件（renwu-lap.json）', () => {
  it('能当普通任务文件导入，且 id/名字/主页都合理', () => {
    expect(file.ver).toBe(1)
    expect(file.tasks).toHaveLength(1)
    expect(task.id).toBe(0)
    expect(task.name).toContain('旧全程')
    expect(prog.main).toBe(0)
    expect(states).toHaveLength(15)
    expect(prog.main).toBeLessThan(states.length)
  })

  it('过校验：没有 error（warnings 也空）', () => {
    const { errors } = validateTask(prog)
    expect(errors).toEqual([])
    expect(validateTask(prog).warnings).toEqual([])
  })

  it('编码后的字节数：能装进 4096 的单任务上限（这也正是把上限从 2048 提上来的原因）', () => {
    const enc = encodeTask(prog)
    expect(enc.errors).toEqual([])
    expect(enc.bytes.length).toBe(2127) // 尺寸护栏：改了瘦身规则/增删块就会在这里报到
    expect(enc.bytes.length).toBeGreaterThan(2048)
    expect(enc.bytes.length).toBeLessThanOrEqual(4096)
    // 程序区 6144 B 里还能再放一个短任务
    expect(enc.bytes.length).toBeLessThan(6144)
  })

  it('名字都不超 32 字节，单序列都不超 64 块', () => {
    const enc = new TextEncoder()
    for (const s of states) {
      expect(s.name.length).toBeGreaterThan(0)
      expect(enc.encode(s.name).length).toBeLessThanOrEqual(32)
      expect(s.enter.length).toBeLessThanOrEqual(64)
      expect(s.body.length).toBeLessThanOrEqual(64)
      expect(s.body.length).toBeGreaterThan(0)
    }
  })

  it('每个状态都是「先等丢线，再往下走或跳下一个状态」', () => {
    for (let i = 0; i < states.length; i++) {
      const body = states[i].body
      expect(body[0].op).toBe(OP.WAIT_COND)
      expect(body[0].p0).toBe(COND.LINE_LOST)
      const tail = body[body.length - 1]
      if (i === states.length - 1) {
        expect(tail.op).toBe(OP.END)
      } else {
        expect(tail.op).toBe(OP.GOTO)
        expect(tail.p0).toBe(i + 1)
      }
    }
  })

  it('关键动作没在转换里丢：两轮直给、右转 90°、动作 2 后等 2050ms', () => {
    expect(allBlocks.some((b) => b.op === OP.SET_MODE && b.p0 === MODE.WHEELS && (b.flags & 1) !== 0)).toBe(true)
    expect(allBlocks.some((b) => b.op === OP.TURN_DEG && b.p1 === -90)).toBe(true)
    expect(allBlocks.some((b) => b.op === OP.ACTION && b.p0 === 2 && b.p2 === 2050)).toBe(true)
    // 旧代码里"原地转前先停车"：速度 0 必须出现在 TURN_DEG 之前
    for (const s of states) {
      const turnIdx = s.body.findIndex((b) => b.op === OP.TURN_DEG)
      if (turnIdx > 0) {
        expect(s.body.slice(0, turnIdx).some((b) => b.op === OP.SET_SPEED && b.p0 === 0)).toBe(true)
      }
    }
  })

  it('存下来再导入还是同一份（页面"导出 → 导入"不会走样）', () => {
    expect(loadTaskFile(serializeTaskFile(file))).toEqual(file)
  })
})
