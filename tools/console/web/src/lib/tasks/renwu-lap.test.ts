/**
 * 旧内置比赛流程（已删的 `firmware/App/Svc/renwu.c`，赛道 11 个障碍）转成任务块的产物。
 * 这个 JSON 是**给用户导入的交付物**，所以在这里钉住：
 *  ① 走一遍「导入文件」的真实路径（`loadTaskFile`）能认；
 *  ② 过 `validateTask`（否则页面点"烧写"会被拦下）；
 *  ③ 编码后长度 ≤ `TASK_STORE_CAP`（4096；同时也断言它 **> 2048**，
 *     因为正是"忠实转换装不下"才把上限从 2048 提到 4096 的）；
 *  ④ 关键块还在（两轮直给 / 转角度 / 动作 2 后等 2050ms / 段尾合法），
 *     防止以后改瘦身规则时把语义悄悄删掉。
 *
 * **2026-10-07 第二版**：用户报「这份 JSON 与旧代码对不上」，逐行对拍后改了四类：
 *  · 换算基准 6480 → **7000**（`HP_TASK_SPEED` 的当前值，`host_link.c:108`）⇒ 全部速度刻度重算；
 *  · 补回第一版漏掉的 3 段循迹：栅栏 54×20ms=**1080ms**、方通 14×20ms=**280ms**、28×20ms=**560ms**；
 *  · 原地转前显式发「速度 0」（旧代码的 `Toggle_Gyro_Control(1,0,0)`），不再依赖残留状态；
 *  · 旧代码里 5 处「只看一半探头」的入口条件改用掩码精确表达。
 *
 * **2026-10-07 第三版（极性修正）**：用户当场指出「为什么会有全白？我记得我都是扫到黑线算的呀」
 * —— 第二版把入口条件**整体译反了**。旧板 `Haixin/Hardware/tracing.c:9-16` 是
 * `tracing_data[i] = !HAL_GPIO_ReadPin(...)`（取反），而 `firmware/App/Svc/host_link.c:496`
 * 写死 `if (bsp_trace_bit(i) == GPIO_PIN_SET) m |= 1<<i;` 并注上「1 = 压线」
 * ⇒ **`tracing_data[i] == 0` = 第 i 路压到黑线**（不是白！）。16 处「8 路全 0」→ 掩码 `0xFF`，
 * 5 处「只看一半探头」→ `0x1F / 0xF8 / 0x1F / 0x0F / 0xF8`。
 * 那一版是 15 状态 / 208 块 / 2434 B / CRC 0xE021（每个状态自带「等条件」块）。
 *
 * **★2026-10-09 第四版 —— 主页分派版（本文件现在钉的就是它）**
 * 用户第三次指出："我要的是**主页只放两样东西**：一直循环的（循迹）、和判断条件；
 * 判断中了进那个状态，状态里的代码跑完就出来，回来继续循迹、继续判断下一条。"
 * —— 这才是旧代码 `void A()` 的真身：
 *     `motor_tracing_pid(4.5);`（一直循迹）+ 一串 `if (8路全黑 && black_num==N) { 段N; black_num=M; }`
 * 第三版受"最多 16 个状态"限制，只能把 21 段压成 15 个状态、**每段自带入口条件**，
 * 于是主页不像主页、条件散在各处 —— 用户看的就是这个"没逻辑"。
 *
 * 现在固件补了两样、状态上限也放宽了：
 *   · `TASK_OP_IF_STAGE(0x0D)`「如果 段号==p0 且 p1 那几路压黑 ⇒ 跳到 p2」，**不满足就往下走**
 *     （与 0x06「等条件」的根本区别：0x06 停在那里等，写不出"判断①不行就看判断②"）
 *   · `TASK_OP_SET_STAGE(0x0C)`「段号 = p0」= 旧代码的 `black_num = M`
 *   · `TASK_MAX_STATES` 16 → 32
 * 所以本版把 21 段**一段一个状态**摊开：
 *   状态 0 = 主页：`[如果 段号=k 且 mask_k → 状态 k+1]` ×21 + `[跳到状态 0]`
 *   状态 k+1 = 段 k：原文的动作… + `[记段号 = k+1]` + `[跳到状态 0]`（最后一段收在「结束任务」）
 * 产物：**22 状态 / 235 块 / 2639 B / CRC 0x5E7B**（第三版是 15/208/2434/0xE021）。
 * 动作、速度、延时与第三版**逐块同源**，只有两处结构变化：入口条件搬到主页、段尾 goto 换成
 * SET_STAGE + GOTO 0。生成器 `out/_gen_lap5.py`，第三版原件留在 `out/renwu-lap.json.bak-v3-fenpai`。
 *
 * ⚠️ **倒车块依赖一处固件修复**：`task_block_t` 的 `p2` 是 u16（只有 `p1` 是有符号），
 *    所以「两轮直给 −3888/−3888」编码后 p2 变成 61936，固件的
 *    `SpeedControl_SetTarget((int)p1, (int)(int16_t)p2)` 已按有符号解释（`task_run.c` 的
 *    `TASK_OP_SET_MODE` 分支）⇒ 这 6 个负速度块是真倒车。本文件保留负值断言。
 */
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { COND, MODE, OP, encodeTask, validateTask, type TaskBlock } from '../task'
import { crossingMarks, loadTaskFile, maskLabel, serializeTaskFile } from '../taskedit'

const file = loadTaskFile(readFileSync(new URL('./renwu-lap.json', import.meta.url), 'utf8'))
const task = file.tasks[0]
const prog = task.prog
const states = prog.states
const allBlocks: TaskBlock[] = states.flatMap((s) => [...s.enter, ...s.body])
/** 主页（任务从这儿开始，一直循环） */
const home = states[prog.main]
/** 主页里那串分派判断 */
const ifs = home.body.filter((b) => b.op === OP.IF_STAGE)
/** 21 个段状态（状态 1..21；状态 0 是主页） */
const segments = states.slice(1)
/** 旧代码里 21 段的掩码顺序（第三版逐块同源，见文件头） */
const MASKS = [0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x1f, 0xff, 0x0f, 0xf8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]

describe('旧全程流程转出来的任务文件（renwu-lap.json · 主页分派版）', () => {
  it('能当普通任务文件导入，且 id/名字/主页都合理', () => {
    expect(file.ver).toBe(1)
    expect(file.tasks).toHaveLength(1)
    expect(task.id).toBe(0)
    expect(task.name).toContain('旧全程')
    expect(prog.main).toBe(0)
    expect(states).toHaveLength(22) // 1 主页 + 21 段
    expect(prog.main).toBeLessThan(states.length)
    // 任务名要进 Flash 页 58 的目录记录（上限 30 字节）
    expect(new TextEncoder().encode(task.name).length).toBeLessThanOrEqual(30)
  })

  it('过校验：没有 error（warnings 也空）', () => {
    expect(validateTask(prog).errors).toEqual([])
    expect(validateTask(prog).warnings).toEqual([])
  })

  it('编码后的字节数：能装进 4096 的单任务上限（这也正是把上限从 2048 提上来的原因）', () => {
    const enc = encodeTask(prog)
    expect(enc.errors).toEqual([])
    expect(enc.bytes.length).toBe(2679) // 尺寸护栏：改瘦身规则/增删块就会在这里报到
    expect(enc.bytes.length).toBeGreaterThan(2048)
    expect(enc.bytes.length).toBeLessThanOrEqual(4096)
    // 程序区 6144 B 里还能再放一个短任务
    expect(enc.bytes.length).toBeLessThan(6144)
    // CRC 护栏：加了「爬台阶」之后是 0xB95A（第四版初版 0x5E7B；第三版 0xE021；第二版全白 0x7319）
    expect(enc.bytes[6] | (enc.bytes[7] << 8)).toBe(0xb95a)
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

  it('★主页只放两样东西：21 条「如果…就跳」+ 一条"跳回自己"', () => {
    expect(home.name).toBe('主页')
    expect(home.body).toHaveLength(22)
    expect(ifs).toHaveLength(21)
    // 最后一块必须是"跳回主页" —— 一条判断都没中时，车继续循迹、下一拍接着判断
    const tail = home.body[home.body.length - 1]
    expect(tail.op).toBe(OP.GOTO)
    expect(tail.p0).toBe(prog.main)
    // 进门块 = 旧代码开头那句一直跑的 motor_tracing_pid(4.5)
    expect(home.enter.length).toBeGreaterThan(0)
    expect(home.enter.some((b) => b.op === OP.SET_MODE && b.p0 === MODE.TRACE_GYRO)).toBe(true)
  })

  it('★每条判断：段号 k、掩码按旧代码、目标就是第 k+1 个段状态', () => {
    ifs.forEach((b, k) => {
      expect(b.p0).toBe(k) // 段号：第 k 段
      expect(b.p1 & 0xff).toBe(MASKS[k]) // 那几路压黑
      expect(b.p1 & 0xff).not.toBe(0) // 掩码 0 = 恒不成立（固件 task_run.c 明确定死）
      expect(b.p2).toBe(k + 1) // 跳到段 k 的状态
    })
  })

  it('★段号闭环：段 k 末尾把段号记成 k+1，主页下一拍就轮到第 k+1 条判断', () => {
    segments.forEach((st, k) => {
      const body = st.body
      const last = body[body.length - 1]
      if (k === segments.length - 1) {
        // 最后一段：旧代码在这里把 renwu_flag 置 3 退出流程
        expect(last.op).toBe(OP.END)
        return
      }
      expect(last.op).toBe(OP.GOTO)
      expect(last.p0).toBe(prog.main) // 干完活回主页
      const set = body[body.length - 2]
      expect(set.op).toBe(OP.SET_STAGE)
      // 段 k 记的段号 == 主页里"目标指向本状态"的那条判断所要求的段号 + 1
      expect(set.p0).toBe(k + 1)
      expect(ifs[set.p0].p2).toBe(k + 2) // 下一条判断的目标就是下一个段状态
    })
  })

  it('掩码分布：16 处 8 路全压黑 + 5 处只看一半探头（0x1F×2 / 0xF8×2 / 0x0F×1）', () => {
    const hist = new Map<number, number>()
    for (const b of ifs) hist.set(b.p1 & 0xff, (hist.get(b.p1 & 0xff) ?? 0) + 1)
    expect([...hist.entries()].sort((a, b) => a[0] - b[0])).toEqual([
      [0x0f, 1], [0x1f, 2], [0xf8, 2], [0xff, 16],
    ])
    expect(maskLabel(0xff)).toBe('8 路全压黑')
    expect(maskLabel(0x1f)).toBe('第 1~5 路压黑')
    expect(maskLabel(0xf8)).toBe('第 4~8 路压黑')
    expect(maskLabel(0x0f)).toBe('第 1~4 路压黑')
  })

  it('★极性：判断的是「压到黑线」而不是「全白」—— 旧代码 tracing_data[i]==0 就是这个意思', () => {
    // 判据链（哪一个都够，这里是四取一的自证）：
    //   tracing.c:9-16 `!HAL_GPIO_ReadPin` 取反  ↔  PID_tracing.c:18-25 用原值当"看见线"
    //   host_link.c:496  `== GPIO_PIN_SET → 1 = 压线`
    //   docs/上位机通信协议.md:163 「bit i = 第 i+1 路灰度压线（1 = 压线）」
    //   docs/控制算法升级方案.md:42  「renwu.c 里『全 0』事件 = 8 路原始位全 SET（整排压线）」
    // 新结构里已经没有「等条件」块了（入口条件全搬到主页 ⇒ OP.IF_STAGE 的掩码直接是"压黑"），
    // 所以这里只钉"一条「全白」口径都不许出现"。
    expect(allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.CH_WHITE)).toHaveLength(0)
    expect(allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.LINE_LOST)).toHaveLength(0)
    expect(ifs.every((b) => (b.p1 & 0xff) !== 0)).toBe(true)
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

  it('倒车块故意保留负值（旧代码本意）；固件已把 p2 按有符号解释 ⇒ 真倒车', () => {
    const back = allBlocks.filter((b) => b.op === OP.SET_MODE && b.p0 === MODE.WHEELS && b.p1 < 0 && b.p2 < 0)
    expect(back).toHaveLength(6)
  })

  it('状态名 = 主页 + 段0..段20（段号与旧 black_num 链一一对应）', () => {
    expect(states.map((s) => s.name)).toEqual(['主页', ...Array.from({ length: 21 }, (_, k) => `段${k}`)])
  })

  it('存下来再导入还是同一份（页面"导出 → 导入"不会走样）', () => {
    expect(loadTaskFile(serializeTaskFile(file))).toEqual(file)
  })
})

/**
 * 「第几次压线」—— 用户 2026-10-09 要的视角：旧代码是「一个主循环一直循迹 + 按 `black_num`
 * 段号分派」，而 `black_num` 只在段尾被手动改。新结构里这条链就摊在**主页**上：
 * 第 k+1 条「如果…就跳」= 第 k+1 次压线 → 段 k。
 *
 * 序号由块在链上的位置算出来（`crossingMarks`），**不进镜像**（`OP.IF_STAGE` 的 p0 是段号，
 * 那才是运行时真正判的东西）—— 页面上的「第 N 次」徽章、「压线链」卡片都用它。
 */
describe('压线链：第几次压线 ↔ 哪一段', () => {
  const marks = crossingMarks(prog)

  it('全程 21 次压线，序号 1..21 连续', () => {
    expect(marks).toHaveLength(21)
    expect(marks.map((m) => m.n)).toEqual(Array.from({ length: 21 }, (_, i) => i + 1))
  })

  it('第 k 次压线都落在主页，第 k 次 → 段状态 k', () => {
    for (const m of marks) {
      expect(m.state).toBe(prog.main) // 全部在主页上（这就是"主页只放判断"）
      expect(m.seq).toBe('body')
      expect(m.index).toBe(m.n - 1) // 第 1 次在最前面
    }
  })

  it('掩码与主页判断逐条一致', () => {
    expect(marks.map((m) => m.mask)).toEqual(MASKS)
    expect(marks.every((m) => !m.white)).toBe(true)
  })

  it('★大坡和台阶3 各挂了一块「爬台阶」：没上去就跳回本状态、上去后立刻停车', () => {
    // 段16 = 大坡（状态 17）、段18 = 台阶3（状态 19）—— 用户 m 2026-10-09 指定的两处
    const expectClimb = (segIdx: number, n: number) => {
      const st = segments[segIdx]
      const climbIdx = st.body.findIndex((b) => b.op === OP.CLIMB)
      expect(climbIdx, `段${segIdx}（${st.name}）里没有「爬台阶」块`).toBeGreaterThan(0)
      const c = st.body[climbIdx]
      expect(c.p0, `段${segIdx} 要爬几节`).toBe(n)
      expect(c.p1).toBe(150) // 先抬过 15° 才算"抬起过"
      expect(c.p2).toBe(30) // 回落容差 3°
      expect(c.p3, `段${segIdx} 没上去要跳回自己`).toBe(segIdx + 1)
      // 紧接着必须是「停车」——就是用户要的"立马刹停"
      expect(st.body[climbIdx + 1].op).toBe(OP.SET_MODE)
      expect(st.body[climbIdx + 1].p0).toBe(MODE.IDLE)
    }
    expectClimb(16, 1) // 大坡：一整条坡，爬 1 节
    expectClimb(18, 2) // ★台阶3：两节台阶 —— "设定两节就必须跑完两段之后才认"
    // 全任务只有这两处
    expect(allBlocks.filter((b) => b.op === OP.CLIMB)).toHaveLength(2)
  })

  it('序号不占镜像体积：编出来还是 2679 B', () => {
    expect(encodeTask(prog).bytes.length).toBe(2679)
  })
})
