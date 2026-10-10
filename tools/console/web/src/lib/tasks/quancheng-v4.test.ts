/**
 * 全程 v4 = `quancheng-v3.json` 经**逐句对照**（`docs/全程v3逐句对照解读.md`，754 行）查出的
 * 10 条问题**全部修掉**之后的副本。这个文件把每一条修复都钉成断言，防止以后又被改回去。
 *
 * ★ 这份测试**只看 v4**，一个字都不碰 v3（v3 是"未修的对照版"）。
 *   v4 的产物与佐证：**2767 B / 250 块 / 15 状态 / CRC `0xE251`**
 *   （2026-10-10 与 `Haixin/Hardware/renwu.c`（旧板 `A()`）**逐段等价核对**后更新：
 *     补回 5 道老门 + 补回段28 的「停车 / 速度7000 / 角度环」；当天又给主页 13 条
 *     「如果…就跳」加上了**满足时间 100ms 的滤波**（p0；固件 `TASK_OP_IF_STAGE`）。
 *     改前是 2687 B / 242 块 / `0x651C`；只补段1 = 2697 B / 243 块 / `0x9ECE`；
 *     补完 5 道门 = 2737 B / 247 块 / `0xAD57`；补完段28 = 2767 B / 250 块 / `0x1D0F`）。
 *
 * ★★ **2026-10-10：v4 又按用户要求改成了「主页分派 + 爬台阶」写法**，所以下面每条的
 *    *定位方式*变了（断言本身要验的事没变）：
 *      · 原来每个状态 **enter 里的入口条件**，现在搬到**主页的一条「如果…就跳」**上
 *        （用户："主页只放两种内容——一直循环的循迹、和判断条件"）。状态号整体 +1（主页占 0）。
 *      · 段尾原来的"跳到下一个"改成**跳回主页**；**不再有「记段号」**——"第几次"由固件自己数
 *        （主页第 k 条判断 = 第 k+1 次压线，见 `task_prog.h` 的 `TASK_OP_IF_STAGE` 注释）。
 *      · **不加任何超时兜底**（用户："8 秒兜底啥的就不能有这些东西……必须得完成"）。
 *      · 大坡（新状态 12）与台阶3收尾（新状态 14）各挂了「爬台阶」+「停车」。
 *      · 例外：**过减速带（新状态 5）** 的入口是条件 3「又看见黑线」，「如果…就跳」表达不了
 *        ⇒ 它不进主页判断，由「栅栏右转」（新状态 4）末尾**直接跳进去**，条件原样保留。
 *
 * 逐条（编号沿用对照文档 §4；行号都是 `git show 53b5fc0:firmware/App/Svc/renwu.c`）：
 *  · **A1** 40 块「等 N 毫秒」原写成 `WAIT_COND(条件 9 ALWAYS) + 超时 N` —— 解释器
 *    （`task_run.c:290-320`）在「条件成立 且 flags 低 7 位=0」时**当拍 return 1**，
 *    超时那条出路永远走不到 ⇒ 车上 40 块全是 0 ms、被吞掉的设计时长合计 **40170 ms**。
 *    改成 `WAIT_MS(op=0x05, p0=ms)` ⇒ 本文件里 `WAIT_MS` **47 块**、且**没有任何** ALWAYS 条件的等待块。
 *  · **A2** 8 处转向符号反：判据 `TURN_DEG(+p) ≡ target += p`（`task_run.c:253-255`），
 *    而旧板 `TurnLeft(p)=target-=p` / `TurnRight(p)=target+=p`（`Haixin/Hardware/gyro_control.c:218-234`，
 *    **与函数名相反**）⇒ 旧 `TurnRight(70/60/88)` 译 `+`、旧 `TurnLeft(80/88)` 译 `−`。
 *    6 处 `*Fixed()` 派生的是对的（双反），一个都没动。
 *  · **B1** S1/S2/S5 的入口原来是条件 3 `LINE_FOUND`（循迹时**恒真**）⇒ 换成条件 10 `CH_BLACK`
 *    + 掩码 `0xFF`；S3 用旧段 6 的 `0x1F`；S4 按它自己的重设计**保留不动**。
 *  · **B2** S6「钻方通」入口掩码 `0x1F` → **`0xF8`**（旧 `L516` 真正看的是 `tracing_data[3..7]`）。
 *  · **B3** 删掉从**死段 16**（`L682-696`）建出来的 S7「出洞口」，跳转直接接到「十字A」
 *    （旧 `L646 black_num=17`）；删状态后所有 GOTO/超时改道目标**跟着重编号**。
 *  · **C1** 补回段 2（`L150-156`）与段 11（`L500-507`）两个「上一个障碍做完 → 等下一个触发带」的锚点。
 *  · **C2** `ACTION 5/9` 在解释器里只走 `Servo_ApplyAction()`（只下发终点）⇒ 用
 *    `SET_SERVO` + `WAIT_MS` 把旧 `Servo_Move()` 的中间姿态**展开**（`servo.c:309-334`）。
 *  · **C3** 「定时循迹」段旧是 TIM2 10 Hz 的 `speed=200`（20% 占空比 ≈ `RW(200)`=**1556**），
 *    v3 用了 7000 ⇒ 改成 `SET_SPEED 1556` + `SET_MODE 2`（`MOTION_TRACE`，仍走循迹环）。
 *  · **C4** 补回 `L855-856` 的 500 ms 停顿（转完 88° 先停住再起步）。
 *  · **C5** 旧循迹基准 `speed=1000`=旧板满量程 ⇒ v4 仍是 7000（97.2%）**有意不动**
 *    （7000 是用户设定的 `HP_TASK_SPEED`）；全流程比旧板慢约 **2.8%**，只记在文档里。
 *
 * 另一份逐处修改日志：`out/_v4_fixlog.txt`（每处的旧代码行 + 新旧块对照）。
 */
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { COND, MODE, OP, crc16Ccitt, encodeTask, validateTask, type TaskBlock } from '../task'
import { loadTaskFile } from '../taskedit'

const file = loadTaskFile(readFileSync(new URL('./quancheng-v4.json', import.meta.url), 'utf8'))
const task = file.tasks[0]
const prog = task.prog
const states = prog.states
/** 主页（任务从这里开始，一直循环）—— 所有入口条件都在它身上 */
const home = states[prog.main]
/** 主页里那串「如果…就跳」（13 条 = 14 段减去不进主页判断的「过减速带」） */
const ifs = home.body.filter((b) => b.op === OP.IF_STAGE)
/** 按状态名找（B3 删过状态、主页占了下标 0 ⇒ 名字才是稳的） */
const st = (name: string) => {
  const s = states.find((x) => x.name === name)
  if (!s) throw new Error(`没有状态「${name}」`)
  return s
}
const allBlocks: TaskBlock[] = states.flatMap((s) => [...s.enter, ...s.body])
const isWaitMs = (b: TaskBlock, ms: number) => b.op === OP.WAIT_MS && b.p0 === ms

describe('全程 v4（quancheng-v4.json）：逐句对照查出的 10 条问题都已修', () => {
  it('文件形态：15 个状态（1 主页 + 14 段）/ main=0 / 名字能进目录记录', () => {
    expect(file.ver).toBe(1)
    expect(file.tasks).toHaveLength(1)
    expect(prog.main).toBe(0)
    expect(states).toHaveLength(15)
    expect(home.name).toBe('主页')
    expect(new TextEncoder().encode(task.name).length).toBeLessThanOrEqual(30)
    expect(task.name).toContain('v4')
  })

  it('过校验：errors=[] 且 warnings=[]', () => {
    const v = validateTask(prog)
    expect(v.errors).toEqual([])
    expect(v.warnings).toEqual([])
  })

  it('编码：2767 B / CRC 0xE251 / ≤4096（尺寸与 CRC 护栏）', () => {
    const enc = encodeTask(prog)
    expect(enc.errors).toEqual([])
    expect(enc.bytes.length).toBe(2767)
    expect(enc.bytes.length).toBeLessThanOrEqual(4096)
    const crc = enc.bytes[6] | (enc.bytes[7] << 8)
    expect(crc).toBe(0xe251)
    // CRC 自洽（独立复算一遍，与真固件 task_prog.c 同一算法）
    expect(crc16Ccitt(enc.bytes.subarray(12))).toBe(crc)
  })

  // ------------------------------------- ★补回 5 道老门（2026-10-10）
  it('★补回 5 道老门（段1 / 段7 / 段18 / 段27 / 段28）：与旧 renwu.c「一段一道门」对齐', () => {
    // 旧板 `A()`（`Haixin/Hardware/renwu.c`，955 行）是**一段一道门**，活链 21 道「8 路全黑」。
    // v4 把连续几段合并进一个状态时，把中间那次「8 路全黑」丢了 ⇒ 只吃 17 次黑带事件；
    // 补回这 5 道后（减速带多吃 1 次是**有意重设计**，两片减速带各一次）账目对上 21 次。
    const doorsOf = (name: string) =>
      st(name)
        .body.map((x, i) => (x.op === OP.WAIT_COND && x.p0 === COND.CH_BLACK && x.p1 === 0xff ? i : -1))
        .filter((i) => i >= 0)

    // ★段 1 的门（第一优先）：旧段0（L70）只做 Buzzer + Servo_Move(2) + HAL_Delay(300)，
    //   这 300ms 里循迹没停、角度环还没开 ⇒ 车按原速冲过【第一个路口】；
    //   旧段1（L77）要**再等一次** 8 路全黑才开角度环、做爬台阶的一串舵机动作。
    //   v4 原来两段连在一起 ⇒ 车一到路口就把整套爬台阶动作做完了。
    const b = st('上台阶').body
    expect(b[0]).toMatchObject({ op: OP.ACTION, p0: 2, p2: 300 })   // 段 0：过路口
    expect(b[1]).toMatchObject({ op: OP.WAIT_COND, p0: COND.CH_BLACK, p1: 0xff, p2: 8000, p3: 0xffff })
    expect(doorsOf('上台阶')).toEqual([1, 16])                      // 段1 的门 + 段2 的门（原锚点）
    const gyroIdx = b.findIndex((x) => x.op === OP.SET_MODE && x.p0 === MODE.GYRO)
    expect(gyroIdx).toBeGreaterThan(1)                              // 段1 的动作夹在两门之间
    expect(gyroIdx).toBeLessThan(16)

    // 段 7 的门：旧段6（L271，栅栏里 前进450ms + 转70°）之后、旧段7（L283，Load+500ms+Servo1）之前
    expect(doorsOf('栅栏右转')).toEqual([6])
    // 段 18 的门：旧段17（L642，前进400 + 转60 + 倒车600）之后、旧段18（L659，前进400 + 转-90）之前
    expect(doorsOf('十字A')).toEqual([8])
    // 段 27 / 段 28 的门：台阶3收尾里，旧段26 末尾（舵机#11，后等3000ms）之后
    expect(doorsOf('台阶3收尾')).toEqual([23, 27])
    const s14 = st('台阶3收尾').body
    expect(s14[22]).toMatchObject({ op: OP.ACTION, p0: 11, p2: 3000 })  // 段26 末块：舵机#11
    expect(s14[24]).toMatchObject({ op: OP.ACTION, p0: 1 })             // 段27：舵机#1（在两门之间）
    expect(s14[31]).toMatchObject({ op: OP.WAIT_MS, p0: 10000 })        // 段28：保持 10 秒
  })

  // ------------------------- ★「如果…就跳」的满足时间（滤波，2026-10-10 用户要求）
  it('★13 条「如果…就跳」都带「满足时间 100ms」滤波，用来滤掉十字路口那一下', () => {
    // 为什么要它：探头排**横着**装 ⇒ 十字路口的横向黑线与探头排平行、**8 路也会同时压黑**，
    // 与"骑上障碍的黑砂面"是同一个信号。路口那一下很短（3.8cm 线 @0.5m/s ≈ 76ms），
    // 障碍前的黑带面积大得多 ⇒ 要求"**连续**成立 ≥ 100ms"就把路口滤掉。
    // 固件侧：`TASK_OP_IF_STAGE` 的 p0 = 满足时间 ms，中途断一次重新计时；
    //         **p0 = 0 = 不滤波**，与加这个功能之前逐字节等价（老任务 p0 全是 0）。
    expect(ifs).toHaveLength(13)
    expect(ifs.every((b) => b.p0 === 100)).toBe(true)
    // 主页之外不许有「如果…就跳」：固件按"块下标 == 已分派次数"配对，
    // 状态内部的下标会撞号（task_prog.h 的 TASK_OP_IF_STAGE 注释里写了）
    const elsewhere = states
      .filter((s) => s !== home)
      .flatMap((s) => [...s.enter, ...s.body].filter((b) => b.op === OP.IF_STAGE))
    expect(elsewhere).toHaveLength(0)
  })

  // ------------------------------- ★段28 的「停车 / 速度 / 角度环」（2026-10-10 补回）
  it('★「台阶3收尾」在「保持 10 秒」之前必须先停车再前进 7000，不能带着倒车状态空转', () => {
    // 旧 renwu.c L935-938（段28）：`Load(0,0); Buzzer_blink(); Toggle_Gyro_Control(1, 900, 900);
    //   HAL_Delay(10000);` ⇒ **停车 → 开角度环以 900/900（=前进 7000）轰 10 秒**。
    // v4 原来那一块前面是 `两轮直给 -7000/-7000`（段27 的倒车）+ `等1000ms`，
    // 门通过之后**直接等 10000ms** ⇒ 那 10 秒车一直在【倒车】。
    const s14 = st('台阶3收尾').body
    const wIdx = s14.findIndex((b) => b.op === OP.WAIT_MS && b.p0 === 10000)
    expect(wIdx).toBeGreaterThan(0)
    // 紧挨着它的前三块必须是 停车 → 速度7000 → 角度环
    expect(s14[wIdx - 1]).toMatchObject({ op: OP.SET_MODE, p0: MODE.GYRO })
    expect(s14[wIdx - 2]).toMatchObject({ op: OP.SET_SPEED, p0: 7000 })
    expect(s14[wIdx - 3]).toMatchObject({ op: OP.SET_MODE, p0: MODE.IDLE })
    // 而它前面第四块是段28 的门
    expect(s14[wIdx - 4]).toMatchObject({ op: OP.WAIT_COND, p0: COND.CH_BLACK, p1: 0xff })
    // 再往前才是段27 的倒车
    expect(s14[wIdx - 5]).toMatchObject({ op: OP.WAIT_MS, p0: 1000 })
    expect(s14[wIdx - 6]).toMatchObject({ op: OP.SET_MODE, p0: MODE.WHEELS })
  })

  // ---------------------------------------------------------------- 主页分派
  it('★主页就是「一直循迹 + 13 条判断 + 跳回自己」；序号不用填、也没有任何超时兜底', () => {
    // 进门块 = 旧代码开头那句一直跑的 motor_tracing_pid(4.5) 的准备（设定速度 + 切循迹）
    expect(home.enter.some((b) => b.op === OP.SET_MODE && b.p0 === MODE.TRACE_GYRO)).toBe(true)
    expect(home.body).toHaveLength(14) // 13 条判断 + 1 条跳回
    expect(ifs).toHaveLength(13)
    expect(home.body[home.body.length - 1]).toMatchObject({ op: OP.GOTO, p0: prog.main })
    // ★序号不用填：靠"块下标 == 已分派次数"配对，p0 **不装序号**。
    //   （p0 现在是「满足时间（ms）」= 100ms 滤波，2026-10-10 起；见下面那条用例。
    //     它以前确实是废弃的段号字段，所以老断言是 `p0 === 0`。）
    expect(ifs.every((b) => b.p0 === 100)).toBe(true)
    // ★没有兜底：判断块没有"超时"这一出（p3 恒 0xFFFF = 不用）
    expect(ifs.every((b) => b.p3 === 0xffff)).toBe(true)
    // 每段末尾都回主页（「过减速带」由「栅栏右转」直接跳进去，不进主页判断）
    for (const s of states) {
      if (s === home) continue
      const body = s.body
      const last = body[body.length - 1]
      if (last.op === OP.END) continue // 最后一段（台阶3收尾）
      expect([OP.GOTO], `${s.name} 末尾`).toContain(last.op)
      expect([0, 5], `${s.name} 末尾目标`).toContain(last.p0) // 5 = 「栅栏右转」直接进「过减速带」
    }
  })

  it('★爬台阶挂在大坡（N=1）与台阶3收尾（N=2），后面紧跟「停车」', () => {
    const climbAt = (name: string, n: number) => {
      const body = st(name).body
      const i = body.findIndex((b) => b.op === OP.CLIMB)
      expect(i, `${name} 里没有「爬台阶」`).toBeGreaterThan(0)
      expect(body[i]).toMatchObject({ p0: n, p1: 150, p2: 30 })
      // p3 = 没上去就跳回**本状态**
      expect(body[i].p3).toBe(states.indexOf(st(name)))
      expect(body[i + 1]).toMatchObject({ op: OP.SET_MODE, p0: MODE.IDLE }) // 立马刹停
    }
    climbAt('大坡', 1)
    climbAt('台阶3收尾', 2)
    expect(allBlocks.filter((b) => b.op === OP.CLIMB)).toHaveLength(2)
  })

  // ---------------------------------------------------------------- A1
  it('A1：40 块假延时全部换成 WAIT_MS（总数 47），且没有任何 ALWAYS 条件的等待块', () => {
    const nWaitMs = allBlocks.filter((b) => b.op === OP.WAIT_MS).length
    expect(nWaitMs).toBe(47) // 38（A1 剩下的）+ 2（C1 锚点）+ 6（C2 展开）+ 1（C4 停顿）
    // 条件 9 ALWAYS 一个都不许再有（它就等于 0 ms）
    const always = allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.ALWAYS)
    expect(always).toEqual([])
    // 抽三处最有代表性的（草地整段 / 最长直行 / 终点保持 10 秒）
    expect(isWaitMs(st('过草地').body.find((b) => b.p0 === 2800)!, 2800)).toBe(true)
    expect(isWaitMs(st('栅栏右转').body.find((b) => b.p0 === 3800)!, 3800)).toBe(true)
    expect(isWaitMs(st('台阶3收尾').body.find((b) => b.p0 === 10000)!, 10000)).toBe(true)
    // 被吞掉的设计时长：47 块 WAIT_MS 合计 43770 ms
    //   = 39470（A1 从 40 块里剩下 38 块；S7「出洞口」那 700 ms 随 B3 删状态一起去掉）
    //   + 600（C1 两个锚点的 300 ms）
    //   + 3300（C2：5 号 400+100+1500、9 号 200+100+900 —— 后两笔是原 ACTION 的 p2 等待搬过来的）
    //   + 500（C4 的停顿）
    const sum = allBlocks.filter((b) => b.op === OP.WAIT_MS).reduce((a, b) => a + b.p0, 0)
    expect(sum).toBe(43770)
  })

  // ---------------------------------------------------------------- A2
  it('A2：8 处普通 Turn 按「目标角增量」取反；6 处 *Fixed() 的 ±90 一个没动', () => {
    const turn = (name: string, i: number) => st(name).body[i].p1
    expect(turn('栅栏右转', 4)).toBe(+70) // L334 TurnRight(70) = target += 70
    expect(turn('钻方通', 4)).toBe(-80) //  L527 TurnLeft(80)  = target -= 80 ★待实测（注释与函数名一致，见文档 §6）
    expect(turn('十字A', 4)).toBe(+60) //   L705 TurnRight(60)
    expect(turn('过U形隧道', 4)).toBe(+60) // L743 TurnRight(60)
    expect(turn('路口B', 4)).toBe(-80) //   L782 TurnLeft(80)
    expect(turn('十字C', 5)).toBe(+88) //   L828 TurnRight(88)
    expect(turn('大坡', 10)).toBe(-88) //   L853 TurnLeft(88)
    expect(turn('大坡', 24)).toBe(+88) //   L876 TurnRight(88)（C4 在它前面插了 2 块 ⇒ 下标 22→24）
    // *Fixed() 派生的 6 处（旧 TurnLeftFixed = TurnRight(90) ⇒ +90；旧 TurnRightFixed ⇒ −90）
    const fixed = states.flatMap((s) => s.body).filter((b) => b.op === OP.TURN_DEG && Math.abs(b.p1) === 90)
    expect(fixed.map((b) => b.p1).sort((a, b) => a - b)).toEqual([-90, -90, 90, 90, 90, 90])
  })

  // ---------------------------------------------------------------- B1 / B2
  it('B1：入口条件搬到主页判断——整排压黑 0xFF ×8、0x1F ×2、0xF8 ×2、0x0F ×1', () => {
    // 主页判断顺序 = 原 S0,S1,S2,S3,(S4 跳过),S5…S13
    expect(ifs.map((b) => b.p1 & 0xff)).toEqual([
      0xff, 0xff, 0xff, 0x1f, 0xff, 0xf8, 0x1f, 0x0f, 0xf8, 0xff, 0xff, 0xff, 0xff,
    ])
    // 之前"等不到就重试自己"的语义，主页分派天然就是这个（不满足就一直循环、继续循迹），
    // 而且**一个超时都没有**（用户 2026-10-10：不要兜底，必须完成）—— 上面那条已钉。
    // S4 是"减速带"的有意重设计：按用户要求保留条件 3，不进主页判断
    expect(st('过减速带').enter[0]).toMatchObject({ p0: COND.LINE_FOUND })
    expect(allBlocks.filter((b) => b.op === OP.WAIT_COND && b.p0 === COND.LINE_FOUND)).toHaveLength(1)
    // 所以「栅栏右转」末尾必须**直接跳进**它（新状态 5）
    const s3 = st('栅栏右转').body
    expect(s3[s3.length - 1]).toMatchObject({ op: OP.GOTO, p0: 5 })
  })

  it('B2：钻方通入口掩码 0xF8（旧 L516 看的是第 4..8 路）—— 现在在主页判断上', () => {
    const m = ifs.map((b) => b.p1 & 0xff)
    expect(m.filter((x) => x === 0xf8)).toHaveLength(2) // 钻方通 + 路口B
    expect(m.filter((x) => x === 0x0f)).toHaveLength(1) // 过U形隧道
    expect(m.filter((x) => x === 0x1f)).toHaveLength(2) // 栅栏右转 + 十字A
    expect(m.filter((x) => x === 0xff)).toHaveLength(8)
  })

  // ---------------------------------------------------------------- B3
  it('B3：从死段 16 建出来的「出洞口」已删，跳转重编号后没有悬空引用', () => {
    expect(states.map((s) => s.name)).not.toContain('出洞口')
    expect(states[8].name).toBe('十字A') // 旧 S8 上移一位；主页占了 0 ⇒ 再 +1
    for (const b of allBlocks) {
      if (b.op === OP.GOTO) expect(b.p0).toBeLessThan(states.length)
      if (b.op === OP.WAIT_COND && b.p3 !== 0xffff) expect(b.p3).toBeLessThan(states.length)
    }
    // 删掉的那个状态原来在 body 里做的两件事（前进 300 / 差速右摆 400）也不该再出现在任何地方
    expect(allBlocks.filter((b) => b.op === OP.SET_MODE && (b.flags & 1) && b.p1 === 3889 && b.p2 === -7000))
      .toEqual([])
  })

  // ---------------------------------------------------------------- C1
  it('C1：补回段 2 / 段 11 两个「等下一个触发带」的锚点（全黑 + 300 ms 停顿）', () => {
    const anchor = (name: string) => {
      const body = st(name).body
      expect(body[body.length - 1]).toMatchObject({ op: OP.GOTO, p0: prog.main }) // 干完回主页
      expect(body[body.length - 3]).toMatchObject({ op: OP.WAIT_COND, p0: COND.CH_BLACK, p1: 0xff, p2: 8000 })
      expect(body[body.length - 2]).toMatchObject({ op: OP.WAIT_MS, p0: 300 })
    }
    anchor('上台阶') // 旧 L150-156 段2
    anchor('过草地') // 旧 L500-507 段11
  })

  // ---------------------------------------------------------------- C2
  it('C2：ACTION 5 / 9 已展开成 SET_SERVO + WAIT_MS，中间姿态与内部延时都在', () => {
    // 旧 Servo_Move(5) = 135/170 → 400ms → 90 → 100ms → 80（servo.c:309-319）
    const a5 = st('上台阶')
    expect(a5.body.some((b) => b.op === OP.ACTION && b.p0 === 5)).toBe(false)
    const i5 = a5.body.findIndex((b) => b.op === OP.SET_SERVO)
    // 下标：主页分派把 S0 的入口条件搬走后 8→7；★2026-10-10 又在 body[1] 插了段1 的
    // 「8 路全黑」门（与 renwu.c 逐段核对后补回），后面的全体 +1 ⇒ 7→8。展开块仍占 7 块。
    expect(i5).toBe(8)
    const seq5 = a5.body.slice(i5, i5 + 7).map((b) => [b.op, b.p0, b.p1])
    expect(seq5).toEqual([
      [OP.SET_SERVO, 1, 135], [OP.SET_SERVO, 2, 170],
      [OP.WAIT_MS, 400, 0],
      [OP.SET_SERVO, 1, 90],
      [OP.WAIT_MS, 100, 0],
      [OP.SET_SERVO, 1, 80],
      [OP.WAIT_MS, 1500, 0], // 旧 HAL_Delay(1500)（原 ACTION #5 的 p2）
    ])
    // 旧 Servo_Move(9) = 70/10 → 200ms → 100/50 → 100ms → 100/80（servo.c:323-334）
    const a9 = st('台阶3收尾')
    expect(a9.body.some((b) => b.op === OP.ACTION && b.p0 === 9)).toBe(false)
    const i9 = a9.body.findIndex((b) => b.op === OP.SET_SERVO)
    expect(i9).toBe(8) // 台阶3收尾的入口条件在 enter 里（不是 body）⇒ 下标不受主页分派影响
    const seq9 = a9.body.slice(i9, i9 + 9).map((b) => [b.op, b.p0, b.p1])
    expect(seq9).toEqual([
      [OP.SET_SERVO, 1, 70], [OP.SET_SERVO, 2, 10],
      [OP.WAIT_MS, 200, 0],
      [OP.SET_SERVO, 1, 100], [OP.SET_SERVO, 2, 50],
      [OP.WAIT_MS, 100, 0],
      [OP.SET_SERVO, 1, 100], [OP.SET_SERVO, 2, 80],
      [OP.WAIT_MS, 900, 0], // 旧 HAL_Delay(900)
    ])
    // 其余 ACTION 块保持原样（1..14 与旧板逐项一致，只有 5/9 号带中间姿态）
    expect(allBlocks.filter((b) => b.op === OP.ACTION).length).toBe(29)
    expect(allBlocks.filter((b) => b.op === OP.SET_SERVO).length).toBe(10)
  })

  // ---------------------------------------------------------------- C3
  it('C3：「定时循迹」三处改成 SET_SPEED 1556 + SET_MODE 2（旧 TIM2 speed=200 = 20% 占空比）', () => {
    const timed = (name: string, i: number) => {
      expect(st(name).body[i]).toMatchObject({ op: OP.SET_SPEED, p0: 1556 })
      expect(st(name).body[i + 1]).toMatchObject({ op: OP.SET_MODE, p0: MODE.TRACE })
    }
    timed('钻方通', 5) //   L531-533 段13（1000 ms）
    timed('过U形隧道', 5) // L748-750 段19（500 ms）
    timed('十字D', 9) //     L906-908 段25（1000 ms）
    // ★ 不能误伤"主循环循迹"那几段（旧 motor_tracing_pid 的 speed=1000 = 满量程，C5 有意不动）
    expect(st('钻方通').body[18]).toMatchObject({ op: OP.WAIT_MS, p0: 280 })
    expect(st('钻方通').body[4]).toMatchObject({ op: OP.TURN_DEG })
    // 1556 的来源：RW(200) = 200 × 7000 ÷ 900 = 1555.6 → 1556
    expect(Math.round((200 * 7000) / 900)).toBe(1556)
  })

  // ---------------------------------------------------------------- C4
  it('C4：大坡段补回转完 88° 之后的 500 ms 停顿（两轮直给 0/0 + 等 500ms）', () => {
    const b = st('大坡').body
    expect(b[10]).toMatchObject({ op: OP.TURN_DEG, p1: -88 })
    expect(b[11]).toMatchObject({ op: OP.SET_MODE, p0: MODE.WHEELS, flags: 1, p1: 0, p2: 0 })
    expect(b[12]).toMatchObject({ op: OP.WAIT_MS, p0: 500 })
    expect(b[13]).toMatchObject({ op: OP.SET_SPEED, p0: 7000 }) // L859 重新起步
    expect(b[14]).toMatchObject({ op: OP.SET_MODE, p0: MODE.GYRO })
  })

  // ---------------------------------------------------------------- C5
  it('C5：循迹/直行基准仍是 7000（有意不动，全流程比旧板慢约 2.8%）', () => {
    // 旧 motor_tracing_pid() 的 int speed = 1000 是旧板满量程（100% 占空比）；
    // v4 用 7000（97.2%）。7000 是用户设定的 HP_TASK_SPEED，本次**不动数值**，
    // 只在 docs/全程v4修订说明.md 里记一句"整体慢约 2.8%"。
    const speeds = allBlocks.filter((b) => b.op === OP.SET_SPEED).map((b) => b.p0)
    // 13 = 原来 11 处 + 主页进门块里的"设定速度 7000"（主页分派把它提到最前面了）
    //      + ★2026-10-10 补回的段28「停车 → 速度7000 → 角度环」里的那一块
    //        （老 renwu.c L935-938：Load(0,0) 之后是 Toggle_Gyro_Control(1,900,900) =
    //        前进 7000 轰 10 秒；v4 原来缺这一段，那 10 秒车一直在倒车）
    expect(speeds.filter((x) => x === 7000).length).toBe(13)
    expect(Math.max(...speeds)).toBeLessThanOrEqual(7200)
    expect(7200 - 7000).toBe(200)
    expect((200 / 7200) * 100).toBeCloseTo(2.78, 1) // ≈2.8%
  })
})
