/**
 * 旧内置比赛流程（已删的 `firmware/App/Svc/renwu.c`，赛道 11 个障碍）转成任务块的产物。
 * 这个 JSON 是**给用户导入的交付物**，所以在这里钉住：
 *  ① 走一遍「导入文件」的真实路径（`loadTaskFile`）能认；
 *  ② 过 `validateTask`（否则页面点"烧写"会被拦下）；
 *  ③ 编码后长度 ≤ `TASK_STORE_CAP`（4096；同时也断言它 **> 2048**，
 *     因为正是"忠实转换装不下"才把上限从 2048 提到 4096 的）；
 *  ④ 几个关键块还在（两轮直给 / 转角度 / 动作 2 后等 2050ms / 状态尾都是 GOTO 或 END），
 *     防止以后改瘦身规则时把语义悄悄删掉。
 *
 * **2026-10-07 第二版**：用户报「这份 JSON 与旧代码对不上」，逐行对拍后改了四类：
 *  · 换算基准 6480 → **7000**（`HP_TASK_SPEED` 的当前值，`host_link.c:108`）⇒ 全部速度刻度重算；
 *  · 补回第一版漏掉的 3 段循迹：栅栏 54×20ms=**1080ms**、方通 14×20ms=**280ms**、28×20ms=**560ms**
 *    （以及「关角度环后电机恢复到 enable 时保存的速度」被误写成「速度 0」的几处）；
 *  · 原地转前显式发「速度 0」（旧代码的 `Toggle_Gyro_Control(1,0,0)`），不再依赖残留状态；
 *  · 旧代码里 5 处「只看一半探头」的入口条件改用掩码精确表达。
 *
 * **2026-10-07 第三版（★极性修正，就是本文件最底下那两条用例钉的东西）**：用户当场指出
 * 「为什么会有全白？我记得我都是扫到黑线算的呀」—— 第二版把入口条件**整体译反了**。
 * 旧板 `Haixin/Hardware/tracing.c:9-16` 是 `tracing_data[i] = !HAL_GPIO_ReadPin(...)`（取反），
 * 而 `Haixin/Hardware/PID_tracing.c:18-25` 拿原值当「这一路看见线」；`firmware/App/Svc/host_link.c:496`
 * 写死 `if (bsp_trace_bit(i) == GPIO_PIN_SET) m |= 1<<i;` 并注上「1 = 压线」，
 * `docs/上位机通信协议.md:163` 同口径，`docs/控制算法升级方案.md:42` 更是直说
 * **「`renwu.c` 里『全 0』事件 = 8 路原始位全 SET（整排压线）」**。
 * ⇒ **`tracing_data[i] == 0` = 第 i 路压到黑线**（不是白！）。
 * 所以全部 21 个入口条件改成**条件 10 `CH_BLACK` + 掩码**：
 *   16 处「8 路全 0」→ 掩码 `0xFF`；5 处「只看一半探头」→ `0x1F / 0xF8 / 0x1F / 0x0F / 0xF8`（同第二版）。
 * 镜像**除这 21 块的条件号/掩码外逐字节相同**：仍然 **2434 B / 208 块 / 15 状态**，CRC `0x7319` → **`0xE021`**。
 * 逐段依据见 `out/_lap3_table.md` 与 `out/_gen_lap3.py`；第二版原件留在
 * `out/renwu-lap.json.bak-v2-fuhao-fan`（tasks 目录里的 `renwu-lap.json.bak` 是第一版的）。
 *
 * ⚠️ **倒车块依赖一处固件修复**：`task_block_t` 的 `p2` 是 u16（只有 `p1` 是有符号），
 *    所以「两轮直给 −3888/−3888」编码后 p2 变成 61936，固件的
 *    `SpeedControl_SetTarget((int)p1, (int)p2)` 会把它当成「右轮全速前进」⇒ 车原地打转。
 *    修法：`(int)(int16_t)b->p2`（`task_run.c` 的 `TASK_OP_SET_MODE` 分支，1 行）。
 *    本文件里那 6 个负速度块**故意保留负值**（那是旧代码的本意），等固件修好即生效。
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
/** 某块后面紧跟着的那一块（找不到就 undefined） */
const after = (pred: (b: TaskBlock) => boolean) =>
  allBlocks.find((b, i) => pred(b) && allBlocks[i + 1] !== undefined && allBlocks[i + 1].op === OP.WAIT_MS)

describe('旧全程流程转出来的任务文件（renwu-lap.json）', () => {
  it('能当普通任务文件导入，且 id/名字/主页都合理', () => {
    expect(file.ver).toBe(1)
    expect(file.tasks).toHaveLength(1)
    expect(task.id).toBe(0)
    expect(task.name).toContain('旧全程')
    expect(prog.main).toBe(0)
    expect(states).toHaveLength(15)
    expect(prog.main).toBeLessThan(states.length)
    // 任务名要进 Flash 页 58 的目录记录（上限 30 字节）
    expect(new TextEncoder().encode(task.name).length).toBeLessThanOrEqual(30)
  })

  it('过校验：没有 error（warnings 也空）', () => {
    const { errors } = validateTask(prog)
    expect(errors).toEqual([])
    expect(validateTask(prog).warnings).toEqual([])
  })

  it('编码后的字节数：能装进 4096 的单任务上限（这也正是把上限从 2048 提上来的原因）', () => {
    const enc = encodeTask(prog)
    expect(enc.errors).toEqual([])
    expect(enc.bytes.length).toBe(2434) // 尺寸护栏：改了瘦身规则/增删块就会在这里报到
    expect(enc.bytes.length).toBeGreaterThan(2048)
    expect(enc.bytes.length).toBeLessThanOrEqual(4096)
    // 程序区 6144 B 里还能再放一个短任务
    expect(enc.bytes.length).toBeLessThan(6144)
    // CRC 护栏：第三版把 21 个入口条件从「全白」改成「全压黑」后是 0xE021
    // （第二版/全白那版是 0x7319）—— 条件再被改反，这里立刻红
    expect(enc.bytes[6] | (enc.bytes[7] << 8)).toBe(0xe021)
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

  it('每个状态都是「先等条件满足，再往下走或跳下一个状态」', () => {
    for (let i = 0; i < states.length; i++) {
      const body = states[i].body
      expect(body[0].op).toBe(OP.WAIT_COND)
      // ★第三版：**每个**入口都是「哪几路压到黑线」（条件 10）。
      // 第二版这里写的是 [LINE_LOST, CH_WHITE] —— 那是把旧代码的极性译反了（见文件头）。
      expect(body[0].p0).toBe(COND.CH_BLACK)
      expect(body[0].p1 & 0xff).not.toBe(0) // 掩码 0 = 恒不满足（固件 task_run.c 明确定死）
      const tail = body[body.length - 1]
      if (i === states.length - 1) {
        expect(tail.op).toBe(OP.END)
      } else {
        expect(tail.op).toBe(OP.GOTO)
        expect(tail.p0).toBe(i + 1)
      }
    }
  })

  it('★极性：入口条件是「压到黑线」而不是「全白」——旧代码 tracing_data[i]==0 就是这个意思', () => {
    // 判据链（哪一个都够，这里是四取一的自证）：
    //   tracing.c:9-16 `!HAL_GPIO_ReadPin` 取反  ↔  PID_tracing.c:18-25 用原值当"看见线"
    //   host_link.c:496  `== GPIO_PIN_SET → 1 = 压线`
    //   docs/上位机通信协议.md:163 「bit i = 第 i+1 路灰度压线（1 = 压线）」
    //   docs/控制算法升级方案.md:42  「renwu.c 里『全 0』事件 = 8 路原始位全 SET（整排压线）」
    const waits = allBlocks.filter((b) => b.op === OP.WAIT_COND)
    expect(waits).toHaveLength(21)
    // 一条都不许再用「丢线 / 全白」那两条条件
    expect(allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.LINE_LOST)).toHaveLength(0)
    expect(allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.CH_WHITE)).toHaveLength(0)
    // 16 处「8 路全 0」= 8 路全压黑（掩码 0xFF）
    expect(waits.filter((b) => b.p1 === 0xff)).toHaveLength(16)
    // 5 处「只看一半探头」= 那几路全压黑（掩码与第二版逐个相同，只是判据反了过来）
    expect(waits.filter((b) => b.p1 !== 0xff).map((b) => b.p1)).toEqual([0x1f, 0xf8, 0x1f, 0x0f, 0xf8])
  })

  it('旧代码里 5 处「只看一半探头」的入口，用掩码精确表达（不是擅自扩成 8 路）', () => {
    // renwu.c：段6(H271)/段17(H642) 看 data[0..4]==0、段13(H461)/段21(H719) 看 data[3..7]==0、
    //          段19(H679) 看 data[0..3]==0（5..7 或 0..2 那几路被作者注释掉了 ⇒ 不管）
    const masked = allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.CH_BLACK && b.p1 !== 0xff)
    expect(masked.map((b) => b.p1)).toEqual([0x1f, 0xf8, 0x1f, 0x0f, 0xf8])
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

  it('速度刻度按 HP_TASK_SPEED = 7000 换算（不是过期默认值 6480）', () => {
    // RW(900) = 7000（旧板「90% 油门」在本板的等价刻度）
    expect(allBlocks.some((b) => b.op === OP.SET_SPEED && b.p0 === 7000)).toBe(true)
    // RW(920)/RW(880) = 7155/6844（栅栏段第一块，`renwu.c:179` 的 Load(RW(920), RW(880))）
    expect(allBlocks.some((b) => b.op === OP.SET_MODE && b.p1 === 7155 && b.p2 === 6844)).toBe(true)
    // RW(500) = 3888（台阶3 最后一段半速爬）
    expect(allBlocks.some((b) => b.op === OP.SET_SPEED && b.p0 === 3888)).toBe(true)
    // 旧基准 6480 一个都不该剩：那是 2026-10-06 之前的值
    expect(allBlocks.some((b) => b.op === OP.SET_SPEED && b.p0 === 6480)).toBe(false)
  })

  it('第一版漏掉的 3 段定时循迹都补回来了（栅栏 1080ms / 方通 280ms + 560ms）', () => {
    for (const ms of [1080, 280, 560]) {
      const i = allBlocks.findIndex((b, k) => b.op === OP.WAIT_MS && b.p0 === ms && allBlocks[k - 1]?.op === OP.SET_MODE && allBlocks[k - 1]?.p0 === MODE.TRACE_GYRO)
      expect(i, `缺少「切循迹 + 等 ${ms}ms」这一对块`).toBeGreaterThan(0)
    }
    // 栅栏段：倒车 800ms 之后必须还有这 1080ms，再切角度环直行（旧代码 renwu.c:189-295）
    const rev = allBlocks.findIndex((b) => b.op === OP.SET_MODE && b.p1 === -3888 && b.p2 === -3888)
    expect(rev).toBeGreaterThan(0)
    expect(allBlocks.slice(rev, rev + 6).some((b, k) => b.op === OP.WAIT_MS && b.p0 === 1080 && allBlocks[rev + k - 1].op === OP.SET_MODE)).toBe(true)
  })

  it('倒车块故意保留负值（旧代码本意）；等固件把 p2 按有符号解释后才真的倒车', () => {
    const back = allBlocks.filter((b) => b.op === OP.SET_MODE && b.p0 === MODE.WHEELS && b.p1 < 0 && b.p2 < 0)
    expect(back).toHaveLength(6)
    // 这些块编码后 p2 会写成 0xF1F0/0xE4A8（无符号 61936/59056）—— 那是固件侧的待修点，
    // 不是本文件的错；修法见文件头注释与 memory/DECISIONS.md 的 2026-10-07 条目。
  })

  it('状态名带旧 black_num 段号，跟 renwu.c 一段一段对得上', () => {
    expect(states.map((s) => s.name)).toEqual([
      '台阶2(段0/1/2)', '木刺(段3)', '栅栏(段4/6/7)', '减速带空段(段9)', '草(段10)',
      '方形通道(段11/13)', '洞口(段17)', '十字A(段18)', 'UUUUU(段19)', '路口B(段21)',
      '路口C(段23)', '大坡(段24)', '路口D(段25)', '台阶3(段26)', '收尾(段27/28)',
    ])
  })

  it('存下来再导入还是同一份（页面"导出 → 导入"不会走样）', () => {
    expect(loadTaskFile(serializeTaskFile(file))).toEqual(file)
  })
})
