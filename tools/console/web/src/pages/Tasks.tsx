import React from 'react'
import {
  AlertTriangle,
  ArrowLeft,
  ArrowRight,
  Copy,
  Download,
  GripVertical,
  Play,
  Plus,
  RefreshCw,
  Save,
  Square,
  Trash2,
  Upload,
  X,
} from 'lucide-react'
import { Badge, Card, NumberField, Stat, Toggle } from '../components/ui'
import {
  CAR_NAME_MAX,
  carNameKey,
  describeSlotPlan,
  fitCarName,
  humanizeTaskError,
  planTaskSlot,
  store,
  type SlotPlan,
} from '../lib/store'
import { COND, MODE, OP, decodeTask, encodeTask, waitCondJumpTarget, withWaitCondJumpTarget, type TaskBlock, type TaskProgram } from '../lib/task'
import {
  COND_SPECS,
  DRAG_MIME,
  EDIT_MAX_TASKS,
  OP_BY_CODE,
  OP_SPECS,
  addState,
  addTaskDef,
  blockCount,
  cloneState,
  compileInfo,
  decodeDrag,
  defaultsFor,
  describeBlock,
  dropPayload,
  encodeDrag,
  fieldHint,
  fieldLabel,
  fieldLegend,
  importTaskFile,
  insertBlock,
  loadTaskFile,
  modeChoices,
  moveBlock,
  nameBytes,
  newTaskDef,
  nextTaskId,
  removeBlock,
  removeState,
  renameState,
  seqRows,
  serializeTaskFile,
  setMain,
  stateChoices,
  stateLabel,
  duplicateBlock,
  updateBlock,
  type FieldKey,
  type FieldSpec,
  type SeqName,
  type TaskDef,
  type TaskFileJSON,
  type Where,
} from '../lib/taskedit'
import { taskApi, type RemoteTaskInfo, type TaskStatus } from '../lib/taskapi'

const STORE_KEY = 'hxb.tasks.v1'
const PALETTE_GROUPS: Array<'运动' | '等待' | '动作' | '流程'> = ['运动', '等待', '动作', '流程']

/** SSR / 隐私模式里没有 localStorage，统一从这里走，读写都不许抛 */
function readStored(): string | null {
  try {
    return typeof localStorage === 'undefined' ? null : localStorage.getItem(STORE_KEY)
  } catch {
    return null
  }
}

function writeStored(text: string): void {
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(STORE_KEY, text)
  } catch {
    /* 隐私模式：改完只在内存里 */
  }
}

function download(name: string, text: string): void {
  try {
    const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }))
    const a = document.createElement('a')
    a.href = url
    a.download = name
    a.click()
    URL.revokeObjectURL(url)
  } catch {
    store.toast('err', '浏览器不让下载，用「复制 JSON」代替')
  }
}

/* ------------------------------------------------------------------ 单块 */

function FieldEditor({
  b,
  at,
  stateCount,
  stateNames,
  onChange,
}: {
  b: TaskBlock
  at: Where
  stateCount: number
  stateNames: string[]
  onChange: (patch: Partial<TaskBlock>) => void
}) {
  const spec = OP_BY_CODE.get(b.op)
  if (!spec) return <div className="text-[12px] text-neon-rose">这个块网页不认识（编号 {b.op}），固件会当非法程序拒收</div>

  const setField = (key: FieldKey, v: number) => {
    // 两轮直给要同时点亮 flags.bit0（固件靠它区分"模式 4"与"模式 0"）
    if (b.op === OP.SET_MODE && key === 'p0') {
      onChange({ p0: v, flags: v === MODE.WHEELS ? b.flags | 1 : b.flags & ~1 })
      return
    }
    /* 「等条件」的「满足后」：下拉给的是**状态号**（或 0xFFFF = 继续往下），
       要翻译成 flags 的 bit7 + 低 7 位再存 —— 见 lib/task.ts 的 withWaitCondJumpTarget。 */
    if (b.op === OP.WAIT_COND && key === 'flags') {
      onChange({ flags: withWaitCondJumpTarget(b.flags, v) })
      return
    }
    onChange({ [key]: v } as Partial<TaskBlock>)
  }

  /* 一行的外壳：标题（鼠标停上去弹说明）+ 控件 + 下面一行"这个值是什么意思"（常显，不用悬停也能看到）。
     注意是**普通函数**不是组件：写成内联组件会让 React 每次渲染换一个类型、输入框一打字就丢焦点。 */
  const fieldRow = (f: FieldSpec, control: React.ReactNode) => (
    <div key={f.key} className="grid gap-0.5">
      <label className="flex items-center gap-2 text-[12px] text-slate-400" title={f.hint}>
        <span className="w-14 shrink-0 cursor-help border-b border-dotted border-slate-500/50">{fieldLabel(f, b)}</span>
        {control}
      </label>
      <div className="pl-16 text-[11px] leading-snug text-slate-400">{fieldHint(f, b)}</div>
    </div>
  )

  return (
    <div className="mt-1.5 grid gap-1.5 rounded-lg bg-black/20 p-2">
      {spec.doc && (
        <div className="text-[11px] text-slate-400" title="这块是干什么用的">
          <span className="text-neon-cyan/80">{spec.label}</span> —— {spec.doc}
        </div>
      )}
      {b.op === OP.TURN_DEG && (
        <Toggle
          on={(b.flags & 1) !== 0}
          onChange={(v) => onChange({ flags: v ? b.flags | 1 : b.flags & ~1 })}
          label={
            <span className="text-[12px]" title="开 = 从车现在朝着的方向再转这么多（转完就保持新朝向）；关 = 不管现在朝哪，转到罗盘上的那个绝对角度">
              相对当前朝向（关掉 = 转到绝对角度）
            </span>
          }
        />
      )}
      {spec.fields
        .filter((f) => {
          if (f.showIf && !f.showIf(b)) return false
          /* 「等条件」选中「指定哪几路是黑的 / 白的」时，**阈值那一格换成下面那组多选框**
             （两个字段填的是同一个 p1：一个是门槛、一个是 8 位掩码，不能同时出现）。 */
          if (b.op === OP.WAIT_COND && (b.p0 === COND.CH_BLACK || b.p0 === COND.CH_WHITE) && f.key === 'p1' && f.kind === 'int') return false
          return true
        })
        .map((f) => {
          const v = b[f.key]
          if (f.kind === 'condjump') {
            const cur = waitCondJumpTarget(b[f.key])
            return fieldRow(
              f,
              <select
                className="field flex-1"
                value={cur}
                title={fieldLegend(f, b, stateCount, stateNames)}
                onChange={(e) => setField(f.key, Number(e.target.value))}
              >
                {stateChoices(cur, stateCount, !!f.allowNoState, stateNames).map((o) => (
                  <option key={o.value} value={o.value}>
                    {o.label}
                  </option>
                ))}
              </select>,
            )
          }
          if (f.kind === 'chmask') {
            /* 8 个多选框 = 8 路探头（bit0 = 第 1 路）。**不套 fieldRow**：fieldRow 的外壳是个
               <label>，里面再放 <label> 是嵌套 label（点标题会误勾第一个框）。
               同一个控件伺候两条镜像条件（黑 / 白）：标题与方块提示都跟着 b.p0 改口。 */
            const mask = b[f.key] & 0xff
            const white = b.p0 === COND.CH_WHITE
            return (
              <div key={f.key} className="grid gap-0.5">
                <div className="flex items-center gap-2 text-[12px] text-slate-400">
                  <span className="w-14 shrink-0 cursor-help border-b border-dotted border-slate-500/50" title={f.hint}>
                    {fieldLabel(f, b)}
                  </span>
                  <div className="flex flex-1 flex-wrap items-center gap-1" title={fieldLegend(f, b, stateCount, stateNames)}>
                    {Array.from({ length: 8 }, (_, i) => {
                      const on = (mask & (1 << i)) !== 0
                      return (
                        <label
                          key={i}
                          className={`flex cursor-pointer select-none items-center gap-0.5 rounded border px-1 py-0.5 text-[11px] ${
                            on ? 'border-neon-cyan/60 bg-neon-cyan/10 text-neon-cyan' : 'border-white/10 text-slate-500 hover:border-white/25'
                          }`}
                          title={`第 ${i + 1} 路：${
                            on
                              ? white
                                ? '勾着 —— 它现在必须不压黑线（全白）'
                                : '勾着 —— 它现在必须压着黑线'
                              : '没勾 —— 它是黑是白都不管'
                          }`}
                        >
                          <input
                            type="checkbox"
                            className="h-3 w-3 accent-neon-cyan"
                            checked={on}
                            onChange={(e) => setField(f.key, e.target.checked ? mask | (1 << i) : mask & ~(1 << i))}
                          />
                          {i + 1}
                        </label>
                      )
                    })}
                  </div>
                </div>
                <div className="pl-16 text-[11px] leading-snug text-slate-400">{fieldHint(f, b)}</div>
              </div>
            )
          }
          if (f.kind === 'mode') {
            return fieldRow(
              f,
              <select
                className="field flex-1"
                value={v}
                title={fieldLegend(f, b, stateCount, stateNames)}
                onChange={(e) => setField(f.key, Number(e.target.value))}
              >
                {modeChoices(v).map((m) => (
                  <option key={m.value} value={m.value} title={m.hint}>
                    {m.label}
                  </option>
                ))}
              </select>,
            )
          }
          if (f.kind === 'cond') {
            return fieldRow(
              f,
              <select
                className="field flex-1"
                value={v}
                title={fieldLegend(f, b, stateCount, stateNames)}
                onChange={(e) => setField(f.key, Number(e.target.value))}
              >
                {COND_SPECS.map((c) => (
                  <option key={c.cond} value={c.cond} title={c.hint}>
                    {c.label}
                  </option>
                ))}
              </select>,
            )
          }
          if (f.kind === 'state') {
            return fieldRow(
              f,
              <select
                className="field flex-1"
                value={v}
                title={fieldLegend(f, b, stateCount, stateNames)}
                onChange={(e) => setField(f.key, Number(e.target.value))}
              >
                {stateChoices(v, stateCount, !!f.allowNoState, stateNames).map((o) => (
                  <option key={o.value} value={o.value}>
                    {o.label}
                  </option>
                ))}
              </select>,
            )
          }
          if (f.kind === 'servo') {
            return fieldRow(
              f,
              <select className="field flex-1" value={v} title={fieldLegend(f, b, stateCount, stateNames)} onChange={(e) => setField(f.key, Number(e.target.value))}>
                <option value={1}>1 号</option>
                <option value={2}>2 号</option>
              </select>,
            )
          }
          return fieldRow(
            f,
            <>
              <NumberField value={v} min={f.lo} max={f.hi} onChange={(nv) => setField(f.key, nv)} className="w-24" />
              {f.unit && <span className="text-[11px] text-slate-600">{f.unit}</span>}
              {f.unit === '油门刻度' && v > 0 && v < 2600 && <Badge tone="amber">可能推不动（&lt;35% 死区）</Badge>}
            </>,
          )
        })}
      <div className="flex items-center gap-2 text-[10px] text-slate-600">
        <span>
          {at.seq === 'enter' ? '进门' : '循环体'} 第 {at.index} 块
        </span>
        <button
          className="btn-ghost !px-1.5 !py-0 text-[10px]"
          title="换块类型（字段会回到该类型的默认值）"
          onClick={() => {
            const next = OP_SPECS[(OP_SPECS.findIndex((s) => s.op === b.op) + 1) % OP_SPECS.length]
            onChange({ ...defaultsFor(next.op) })
          }}
        >
          换成「{OP_SPECS[(OP_SPECS.findIndex((s) => s.op === b.op) + 1) % OP_SPECS.length].label}」
        </button>
      </div>
    </div>
  )
}

function BlockChip({
  b,
  at,
  stateCount,
  stateNames,
  open,
  current,
  onToggle,
  onPatch,
  onDelete,
  onDuplicate,
  onMove,
}: {
  b: TaskBlock
  at: Where
  stateCount: number
  stateNames: string[]
  open: boolean
  current: boolean
  onToggle: () => void
  onPatch: (patch: Partial<TaskBlock>) => void
  onDelete: () => void
  onDuplicate: () => void
  onMove: (dir: -1 | 1) => void
}) {
  const spec = OP_BY_CODE.get(b.op)
  return (
    <div
      draggable
      onDragStart={(e) => {
        e.dataTransfer.setData(DRAG_MIME, encodeDrag({ kind: 'move', from: at }))
        e.dataTransfer.effectAllowed = 'move'
      }}
      className={`rounded-lg border bg-white/[0.03] px-2 py-1 ${current ? 'border-neon-mint/60 ring-1 ring-neon-mint/30' : 'border-white/10'}`}
    >
      <div className="flex items-center gap-1.5">
        <GripVertical size={13} className="shrink-0 cursor-grab text-slate-600" />
        <span className="num text-[10px] text-slate-600">{at.index}</span>
        <button className="min-w-0 flex-1 truncate text-left text-[12px] text-slate-200" onClick={onToggle} title="点一下改参数">
          <span className="text-neon-cyan/90">{spec?.label ?? `不认识的块（编号 ${b.op}）`}</span>
          <span className="ml-1.5 text-slate-400">{describeBlock(b, stateCount, stateNames)}</span>
        </button>
        <button className="text-slate-600 hover:text-slate-300" title="左移" onClick={() => onMove(-1)}>
          <ArrowLeft size={12} />
        </button>
        <button className="text-slate-600 hover:text-slate-300" title="右移" onClick={() => onMove(1)}>
          <ArrowRight size={12} />
        </button>
        <button className="text-slate-600 hover:text-slate-300" title="复制一块" onClick={onDuplicate}>
          <Copy size={12} />
        </button>
        <button className="text-slate-600 hover:text-neon-rose" title="删掉这块" onClick={onDelete}>
          <X size={12} />
        </button>
      </div>
      {open && <FieldEditor b={b} at={at} stateCount={stateCount} stateNames={stateNames} onChange={onPatch} />}
    </div>
  )
}

/* ------------------------------------------------------------------ 状态 */

function DropLine({ at, active, onOver, onDrop }: { at: Where; active: boolean; onOver: (w: Where) => void; onDrop: (w: Where, d: ReturnType<typeof decodeDrag>) => void }) {
  return (
    <div
      data-dropline={`${at.state}.${at.seq}.${at.index}`}
      className={`-my-0.5 h-2 rounded ${active ? 'bg-neon-cyan/40' : 'bg-transparent'}`}
      onDragOver={(e) => {
        e.preventDefault()
        onOver(at)
      }}
      onDrop={(e) => {
        e.preventDefault()
        const raw = e.dataTransfer.getData(DRAG_MIME) || e.dataTransfer.getData('text/plain')
        onDrop(at, decodeDrag(raw))
      }}
    />
  )
}

function StateCard({
  prog,
  i,
  sel,
  open,
  dropAt,
  cur,
  onSelect,
  onDrop,
  onOver,
  onToggleBlock,
  upd,
}: {
  prog: TaskProgram
  i: number
  sel: boolean
  open: Where | null
  dropAt: Where | null
  cur: { state: number; block: number } | null
  onSelect: () => void
  onDrop: (at: Where, d: ReturnType<typeof decodeDrag>) => void
  onOver: (w: Where) => void
  onToggleBlock: (at: Where) => void
  upd: (fn: (p: TaskProgram) => TaskProgram) => void
}) {
  const st = prog.states[i]
  const isMain = prog.main === i
  const live = cur?.state === i
  const seqs: Array<[SeqName, string]> = [
    ['enter', '进门（可空）'],
    ['body', '循环体（末尾要能跳走）'],
  ]
  return (
    <div className={`rounded-xl border p-2.5 ${live ? 'border-neon-mint/50 bg-neon-mint/5' : sel ? 'border-neon-cyan/40 bg-white/[0.02]' : 'border-white/10'}`} onMouseDown={onSelect}>
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <span className="num text-[11px] text-slate-500" data-own-state="1" title={st.name ? `这个状态叫「${st.name}」，右边可以改` : '这个状态还没起名字，右边填一个'}>状态 {i}</span>
        <input
          className="field h-7 min-w-[140px] flex-1 text-[12px]"
          value={st.name}
          placeholder="状态名（可空）"
          onChange={(e) => upd((p) => renameState(p, i, e.target.value))}
        />
        {isMain && <Badge tone="mint">主页</Badge>}
        <span className="text-[11px] text-slate-600">
          {st.enter.length}+{st.body.length} 块 · 名字 {nameBytes(st.name)}/32 B
        </span>
        {!isMain && (
          <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => upd((p) => setMain(p, i))} title="任务启动时从这个状态开始">
            设为主页
          </button>
        )}
        <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => upd((p) => cloneState(p, i))} disabled={prog.states.length >= 16}>
          复制
        </button>
        <button
          className="btn-ghost !px-2 !py-0.5 text-[11px] text-neon-rose"
          onClick={() => upd((p) => removeState(p, i))}
          disabled={prog.states.length <= 1}
          title={prog.states.length <= 1 ? '至少要留一个状态' : '删掉这个状态，指向它的跳转会改道'}
        >
          删除
        </button>
      </div>

      <div className="grid gap-2 lg:grid-cols-2">
        {seqs.map(([seq, title]) => (
          <div key={seq} className="rounded-lg border border-white/5 bg-black/10 p-1.5">
            <div className="mb-1 text-[10px] uppercase tracking-wider text-slate-500">{title}</div>
            <DropLine at={{ state: i, seq, index: 0 }} active={!!dropAt && dropAt.state === i && dropAt.seq === seq && dropAt.index === 0} onOver={onOver} onDrop={onDrop} />
            {seqRows(prog, i, seq).map((r) => (
              <React.Fragment key={`${seq}${r.index}`}>
                <BlockChip
                  b={r.block}
                  at={{ state: i, seq, index: r.index }}
                  stateCount={prog.states.length}
                  stateNames={prog.states.map((s) => s.name)}
                  open={!!open && open.state === i && open.seq === seq && open.index === r.index}
                  current={!!cur && seq === 'body' && cur.state === i && cur.block === r.index}
                  onToggle={() => onToggleBlock({ state: i, seq, index: r.index })}
                  onPatch={(patch) => upd((p) => updateBlock(p, { state: i, seq, index: r.index }, patch))}
                  onDelete={() => upd((p) => removeBlock(p, { state: i, seq, index: r.index }))}
                  onDuplicate={() => upd((p) => duplicateBlock(p, { state: i, seq, index: r.index }))}
                  onMove={(dir) => upd((p) => moveBlock(p, { state: i, seq, index: r.index }, { state: i, seq, index: r.index + dir }))}
                />
                <DropLine
                  at={{ state: i, seq, index: r.index + 1 }}
                  active={!!dropAt && dropAt.state === i && dropAt.seq === seq && dropAt.index === r.index + 1}
                  onOver={onOver}
                  onDrop={onDrop}
                />
              </React.Fragment>
            ))}
            {seqRows(prog, i, seq).length === 0 && <div className="px-1 py-1 text-[11px] text-slate-600">空 —— 从右边把块拖进来</div>}
          </div>
        ))}
      </div>
    </div>
  )
}

/* ------------------------------------------------------------------ 页面 */

export function TasksPage() {
  const [file, setFile] = React.useState<TaskFileJSON>(() => loadTaskFile(readStored()))
  const [selTask, setSelTask] = React.useState(0)
  const [selState, setSelState] = React.useState(0)
  const [open, setOpen] = React.useState<Where | null>(null)
  const [dropAt, setDropAt] = React.useState<Where | null>(null)
  const [remote, setRemote] = React.useState<{ supported: boolean; items: RemoteTaskInfo[] } | null>(null)
  const [status, setStatus] = React.useState<TaskStatus | null>(null)
  const [monitor, setMonitor] = React.useState(false)
  const [busy, setBusy] = React.useState('')
  /** 编辑器任务编号 → 刚烧进去的车上槽号（读完目录之前用它兜底，见 carSlotOf） */
  const [burned, setBurned] = React.useState<Record<number, number>>({})
  const fileRef = React.useRef<HTMLInputElement | null>(null)

  React.useEffect(() => {
    writeStored(serializeTaskFile(file))
  }, [file])

  /* 页面一开着就持续问车上在跑什么（不只在「监视」打开时）：任务也可能是**车上**按中键
     启动的（任务页那套离线操作，用户 m24999），而跑着的时候固件对 COMMIT / DELETE 一律
     回「忙」（firmware/App/Svc/host_link.c:967,999），所以页面上必须看得见「正在跑」，
     否则用户只拿到一句「被车拒绝：忙」，不知道怎么把车救回来。
     （2026-10 之前这里还写着"开机自启会在上电后自己跑"——那个功能用户已经要求删掉了。） */
  React.useEffect(() => {
    let alive = true
    const tick = async () => {
      const r = await taskApi.status()
      if (!alive) return
      if (r.ok) setStatus(r.data)
      else if (monitor) {
        setMonitor(false)
        store.toast('err', r.error)
      }
    }
    void tick()
    const id = window.setInterval(() => void tick(), 1000)
    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [monitor])

  /* 拖到一半放手（丢在空白处/参数区、按 Esc、拖出窗口）时蓝线必须自己消失：
     只在 DropLine 的 onDrop 里清是不够的 —— 没落在任何蓝线上时浏览器只发 dragend，
     于是 dropAt 一直留着，蓝条就挂在那儿不走了。 */
  React.useEffect(() => {
    const clear = () => setDropAt(null)
    window.addEventListener('dragend', clear)
    window.addEventListener('drop', clear)
    return () => {
      window.removeEventListener('dragend', clear)
      window.removeEventListener('drop', clear)
    }
  }, [])

  const task: TaskDef = file.tasks[Math.min(selTask, file.tasks.length - 1)] ?? file.tasks[0]
  const prog = task.prog
  const info = compileInfo(prog)
  const bytes = encodeTask(prog).bytes
  const activeState = Math.min(selState, prog.states.length - 1)
  /** 界面上凡是"提到某个状态"的地方都要带上名字（光编号没法看），统一从这里取 */
  const stateNames = prog.states.map((s) => s.name)

  const updTask = (t: TaskDef) => setFile((f) => ({ ...f, tasks: f.tasks.map((x) => (x === task ? t : x)) }))
  const upd = (fn: (p: TaskProgram) => TaskProgram) => updTask({ ...task, prog: fn(task.prog) })

  const refreshRemote = async (silent = false) => {
    setBusy('读目录')
    const r = await taskApi.list()
    setBusy('')
    if (!r.ok) {
      setRemote({ supported: r.supported, items: [] })
      if (!silent) {
        if (r.supported) store.toast('err', r.error)
        else store.toast('info', '桥/固件还没支持任务功能 —— 编辑照常，烧写等固件')
      }
      return
    }
    setRemote(r.data)
    if (!silent) store.toast('ok', `车上 ${r.data.items.length} 个任务`)
  }

  /* 一进「任务」页就把车上的目录读出来：车上有没有任务不该等用户想起来点一下「读目录」
     才知道。只读，失败也别弹错。 */
  React.useEffect(() => {
    void refreshRemote(true)
  }, [])

  /* ---- 烧到哪个槽：规则与文案都在 lib/store.ts 的 planTaskSlot / describeSlotPlan 里 ----
     编辑器里那条任务的编号（`task.id`，0..7）**不是**车上槽号 —— 老代码拿它当槽号烧，
     车上目录没人看，换一条任务就可能悄悄盖掉车上同槽位的**别人的**任务。
     现在：同名 ⇒ 覆盖它那个槽；没同名 ⇒ 第一个空槽；8 个满了 ⇒ 拦下来不烧。 */
  const carItems = remote?.items ?? []
  /** 这条任务现在对应车上哪个槽；null = 车上还没有它（刚烧过的用记住的槽兜底） */
  const carSlotOf = (t: TaskDef): number | null => {
    const p = planTaskSlot(carItems, t.name)
    if (p.kind === 'same-name') return p.slot
    return burned[t.id] ?? null
  }
  const plan: SlotPlan = planTaskSlot(carItems, task.name)
  const carSlot = carSlotOf(task)
  /** 名字超过车上存得下的字节数时提醒一句（超出部分车上存不下来，桥也会按字节砍） */
  const nameOver = nameBytes(task.name) > CAR_NAME_MAX
  /** 编辑器里有几条同名（>1 时它们按"同名"规则会烧到**同一个槽**，得说清楚是谁覆盖谁） */
  const sameNameInEditor = carNameKey(task.name)
    ? file.tasks.filter((t) => carNameKey(t.name) === carNameKey(task.name)).length
    : 0

  const doBurn = async () => {
    if (info.errors.length) {
      store.toast('err', '有校验错误，先修好再烧')
      return
    }
    setBusy('烧写')
    /* 先读一次车上目录再决定烧哪个槽 —— 这是"同名覆盖 / 空槽 / 满"三条规则的唯一依据。
       不读就烧 = 老行为（拿编辑器编号当槽号），会悄悄盖掉车上别人的任务。 */
    const dir = await taskApi.list()
    if (!dir.ok) {
      setBusy('')
      setRemote({ supported: dir.supported, items: [] })
      store.toast('err', `为了不覆盖错槽位，这次没烧（先读车上目录就失败了）：${humanizeTaskError(dir.error)}`)
      console.warn('[tasks] 读目录失败：', dir.error)
      return
    }
    setRemote(dir.data)
    const p = planTaskSlot(dir.data.items, task.name)
    if (p.kind === 'full') {
      setBusy('')
      store.toast('err', p.error)
      return
    }
    const slot = p.slot
    if (status?.running) {
      // 解释器跑着时固件对 COMMIT 回「忙」——先停车再烧，别让用户对着红字发呆
      const s = await taskApi.stop()
      if (!s.ok) {
        setBusy('')
        store.toast('err', `车上有任务在跑（#${status.id}），先停车又失败了：${humanizeTaskError(s.error)}`)
        console.warn('[tasks] 停车失败：', s.error)
        return
      }
      store.toast('info', `车上有任务在跑（#${status.id}），已先停车再烧写`)
    }
    store.toast('info', describeSlotPlan(p, task.name))
    // 名字按车上存得下的 30 字节、在字符边界上截好再发（桥与固件都按字节砍，砍半个汉字很难看）
    const r = await taskApi.burn(slot, bytes, info.len, info.crc16, fitCarName(task.name))
    if (!r.ok) {
      setBusy('')
      store.toast('err', `烧写到车上 #${slot} 失败：${humanizeTaskError(r.error)}`)
      console.warn('[tasks] 烧写失败：', r.error)
      return
    }
    const back = await taskApi.get(slot)
    setBusy('')
    if (!back.ok) {
      store.toast('info', `已经写进车上 #${slot}，但回读校验没读完 —— ${humanizeTaskError(back.error)}`)
      console.warn('[tasks] 回读失败：', back.error)
      void refreshRemote()
      return
    }
    const same = back.data.length === bytes.length && back.data.every((v, i) => v === bytes[i])
    if (same) {
      setBurned((m) => ({ ...m, [task.id]: slot }))
      store.toast('ok', p.kind === 'same-name'
        ? `已覆盖车上同名的 #${slot}「${p.carName || task.name}」，回读一致（${r.data.chunks} 片）`
        : `已烧到空槽 #${slot}（${r.data.chunks} 片），回读一致`)
    } else {
      store.toast('err', `回读不一致（车上 ${back.data.length} B / 本地 ${bytes.length} B）—— 车上 #${slot} 这条别让它跑，重烧一次。`)
    }
    void refreshRemote()
  }

  const doRun = async () => {
    /* 跑的是**车上那个槽**，不是编辑器里的编号：名字对不上就成了"按了开始却跑了别的任务"。
       车上没有这条就直说，别去跑一个碰巧同号的槽。 */
    const slot = carSlot
    if (slot === null) {
      store.toast('err', `车上没有叫「${carNameKey(task.name) || '（这条没名字）'}」的任务 —— 先按「烧写到车」把它写进车上。`)
      return
    }
    setBusy('启动')
    if (status?.running && status.id !== slot) {
      // 车上一次只许一个任务占着电机，换任务跑得先停车（host_link.c:1013）
      const s = await taskApi.stop()
      if (!s.ok) {
        setBusy('')
        store.toast('err', `车上 #${status.id} 在跑，先停车又失败了：${humanizeTaskError(s.error)}`)
        return
      }
      store.toast('info', `已先停掉正在跑的 #${status.id}`)
    }
    const r = await taskApi.run(slot)
    setBusy('')
    if (r.ok) {
      store.toast('ok', `已启动车上 #${slot}`)
      setMonitor(true)
    } else {
      store.toast('err', `没启动起来：${humanizeTaskError(r.error)}`)
      console.warn('[tasks] 启动失败：', r.error)
    }
  }

  const doStop = async () => {
    setBusy('停止')
    const r = await taskApi.stop()
    setBusy('')
    if (r.ok) {
      store.toast('ok', '已停车')
      setMonitor(false)
      setStatus(null)
    } else store.toast('err', r.error)
  }

  const pullRemote = async (rt: RemoteTaskInfo) => {
    setBusy('回读')
    const r = await taskApi.get(rt.id)
    setBusy('')
    if (!r.ok) {
      store.toast('err', r.error)
      return
    }
    const dec = decodeTask(r.data)
    if (!dec.program) {
      store.toast('err', `车上任务 ${rt.id} 解不开：${dec.error}`)
      return
    }
    const id = nextTaskId(file.tasks)
    if (id < 0) {
      store.toast('err', `编辑器最多 ${EDIT_MAX_TASKS} 个任务`)
      return
    }
    setFile((f) => ({ ...f, tasks: [...f.tasks, { id, name: rt.name || `车上任务${rt.id}`, prog: dec.program as TaskProgram }] }))
    store.toast('ok', `车上任务 ${rt.id} 已经读进编辑器`)
  }

  /** 删车上的任务。跑着的任务固件会回「忙」（firmware/App/Svc/host_link.c:999），
      taskApi.delWithStop() 会自己先停车再删 —— 用户不该被一个自己会跑的任务困住、删都删不掉。 */
  const delRemote = async (rt: RemoteTaskInfo) => {
    setBusy('删除')
    const { result, stopped } = await taskApi.delWithStop(rt.id)
    setBusy('')
    if (result.ok) {
      store.toast('ok', `车上任务 ${rt.id} 已删${stopped ? '（它正在跑，已先停车）' : ''}`)
      setMonitor(false)
      void refreshRemote()
    } else store.toast('err', result.error)
  }

  const onDrop = (at: Where, d: ReturnType<typeof decodeDrag>) => {
    setDropAt(null)
    if (!d) return
    upd((p) => dropPayload(p, at, d))
    if (d.kind === 'new') setOpen(at)
  }

  const paletteAdd = (op: number) => {
    const st = Math.min(selState, prog.states.length - 1)
    const target: Where = { state: st, seq: 'body', index: (prog.states[st]?.body ?? []).length }
    upd((p) => insertBlock(p, target, defaultsFor(op)))
    setOpen(target)
  }

  const cur = status?.running ? { state: status.state, block: status.block } : null
  const supported = remote?.supported ?? true

  return (
    <div className="grid gap-3 xl:grid-cols-[270px_minmax(0,1fr)_280px]">
      {/* 左：任务列表 + 车上现有 */}
      <div className="grid content-start gap-3">
        <Card
          title="任务列表"
          right={
            <div className="flex items-center gap-1">
              <button
                className="btn-ghost !px-2 !py-0.5 text-[11px]"
                disabled={file.tasks.length >= EDIT_MAX_TASKS}
                onClick={() => {
                  const id = nextTaskId(file.tasks)
                  if (id < 0) return
                  setFile((f) => ({ ...f, tasks: [...f.tasks, newTaskDef(id, `任务${id}`)] }))
                  setSelTask(file.tasks.length)
                  setSelState(0)
                  setOpen(null)
                }}
              >
                <Plus size={12} /> 新建
              </button>
              <button
                className="btn-ghost !px-2 !py-0.5 text-[11px]"
                onClick={() => {
                  const id = nextTaskId(file.tasks)
                  if (id < 0) {
                    store.toast('err', `最多 ${EDIT_MAX_TASKS} 个任务`)
                    return
                  }
                  setFile((f) => ({ ...f, tasks: addTaskDef(f.tasks, task, `${task.name}-副本`) }))
                  setSelTask(file.tasks.length)
                }}
                title="复制当前任务"
              >
                <Copy size={12} /> 复制
              </button>
            </div>
          }
        >
          <div className="grid gap-1.5">
            {file.tasks.map((t, i) => (
              <button
                key={t.id}
                className={`rounded-lg border px-2 py-1.5 text-left ${
                  i === selTask ? 'border-neon-cyan/40 bg-neon-cyan/5' : 'border-white/10 hover:border-white/20'
                }`}
                onClick={() => {
                  setSelTask(i)
                  setSelState(0)
                  setOpen(null)
                }}
              >
                <div className="flex items-center gap-1.5">
                  <span className="num text-[10px] text-slate-500" title="编辑器里的编号（不是车上的槽号；烧到车上哪个槽看下面那行）">#{t.id}</span>
                  <span className="min-w-0 flex-1 truncate text-[12px] text-slate-200">{t.name}</span>
                  {status?.running && status.id === carSlotOf(t) && <Badge tone="mint">跑着</Badge>}
                </div>
                <div className="text-[10px] text-slate-600">
                  {t.prog.states.length} 状态 · {blockCount(t.prog)} 块 · {encodeTask(t.prog).bytes.length} B
                  {' · '}
                  {carSlotOf(t) === null ? (
                    <span title="车上还没有这条任务：烧写会写进第一个空槽">车上没有</span>
                  ) : (
                    <span className="text-neon-cyan/70" title="这条任务在车上的槽号（同名再烧就是覆盖它）">车上 #{carSlotOf(t)}</span>
                  )}
                </div>
              </button>
            ))}
            <div className="mt-1 flex flex-wrap items-center gap-1">
              <input
                className="field h-7 flex-1 text-[12px]"
                value={task.name}
                onChange={(e) => updTask({ ...task, name: e.target.value.slice(0, 32) })}
                placeholder="任务名（随 COMMIT 下发到车上的目录页）"
              />
              <button
                className="btn-ghost !px-2 !py-0.5 text-[11px] text-neon-rose"
                disabled={file.tasks.length <= 1}
                onClick={() => {
                  setFile((f) => ({ ...f, tasks: f.tasks.filter((x) => x !== task) }))
                  setSelTask(0)
                }}
                title="从浏览器里删掉（不动车上的）"
              >
                <Trash2 size={12} />
              </button>
            </div>
            <div className="flex flex-wrap items-center gap-1">
              <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => download(`task-${task.id}.json`, serializeTaskFile({ ver: 1, tasks: [task] }))}>
                <Download size={12} /> 导出这个
              </button>
              <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => download('tasks.json', serializeTaskFile(file))}>
                <Download size={12} /> 导出全部
              </button>
              <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => fileRef.current?.click()}>
                <Upload size={12} /> 导入
              </button>
              <input
                ref={fileRef}
                type="file"
                accept=".json,application/json"
                className="hidden"
                onChange={async (e) => {
                  const f = e.target.files?.[0]
                  e.target.value = ''
                  if (!f) return
                  const { tasks, skipped } = importTaskFile(await f.text())
                  if (!tasks.length) {
                    store.toast('err', '这个文件里没有能读的任务')
                    return
                  }
                  /* ★2026-10-07 两处修复（用户 m 报「导入之后没有/看不到块」）：
                   *   ① 导入后要**切换选中项**到刚导入的那条 —— 新建/复制都 setSelTask(...)，
                   *      导入却没有 ⇒ 新任务被 push 到列表末尾，界面还停在原来选中的那条上
                   *      （他当时选的是 3 块的「测试」⇒ 以为「导入后什么都没有」）。
                   *   ② 编辑器满 EDIT_MAX_TASKS 时原来静默 break、toast 仍报「导入 N 个」✗
                   *      ⇒ 满员直接说人话；放不下的也如实报出来。 */
                  const base = file.tasks.length
                  const room = Math.max(0, EDIT_MAX_TASKS - base)
                  if (room === 0) {
                    store.toast('err', `编辑器已经有 ${EDIT_MAX_TASKS} 条任务了 —— 先在左边删掉一条，再导入`)
                    return
                  }
                  const added = Math.min(tasks.length, room)
                  setFile((prev) => {
                    const merged: TaskDef[] = [...prev.tasks]
                    for (const t of tasks) {
                      if (merged.length >= EDIT_MAX_TASKS) break
                      const id = nextTaskId(merged)
                      if (id < 0) break
                      merged.push({ ...t, id })
                    }
                    return { ...prev, tasks: merged }
                  })
                  setSelTask(base)          /* ★选中刚导入的第一条 */
                  store.toast('ok', `导入 ${added} 个任务${skipped ? `，跳过 ${skipped} 个` : ''}${tasks.length > added ? `（还有 ${tasks.length - added} 条放不下：编辑器上限 ${EDIT_MAX_TASKS} 条）` : ''}`)
                }}
              />
            </div>
          </div>
        </Card>

        <Card
          title="车上现有"
          right={
            <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void refreshRemote()} disabled={!!busy}>
              <RefreshCw size={12} /> 读目录
            </button>
          }
        >
          {!remote && <div className="text-[12px] text-slate-500">点「读目录」看车上存了哪些任务（需要固件/桥支持）。</div>}
          {remote && !remote.supported && (
            <div
              className="rounded-lg bg-neon-amber/10 p-2 text-[11px] leading-relaxed text-neon-amber"
              title="车上固件没有上报「任务」这项能力（内部编号 0x60–0x69 的任务帧、能力位 bit7）"
            >
              车上这套固件还不支持读写任务，这一栏暂时只有编辑与编译可用。
              <br />
              要烧写，得先给车上刷一份带任务功能的固件。
            </div>
          )}
          {remote && remote.supported && remote.items.length === 0 && <div className="text-[12px] text-slate-500">车上一个任务都没有。</div>}
          {remote?.items.map((it) => (
            <div key={it.id} className="mb-1 flex items-center gap-1.5 rounded-lg border border-white/10 px-2 py-1">
              <span className="num text-[10px] text-slate-500">#{it.id}</span>
              <span className="min-w-0 flex-1 truncate text-[12px] text-slate-300">{it.name || '（车上无名）'}</span>
              <span className="num text-[10px] text-slate-600">{it.len}B</span>
              <button className="text-[11px] text-slate-500 hover:text-neon-cyan" onClick={() => void pullRemote(it)} title="读回车上的程序">
                <Download size={12} />
              </button>
              <button
                className="text-[11px] text-slate-500 hover:text-neon-rose"
                title="从车上删掉（跑着的话会自动先停车）"
                disabled={!!busy}
                onClick={() => void delRemote(it)}
              >
                <Trash2 size={12} />
              </button>
            </div>
          ))}
        </Card>
      </div>

      {/* 中：编译/烧写 + 状态卡片 */}
      <div className="grid content-start gap-3">
        <Card
          title="编译与烧写"
          right={
            <div className="flex flex-wrap items-center gap-1">
              <button
                className="btn-primary !px-2 !py-0.5 text-[11px]"
                onClick={() => void doBurn()}
                disabled={!!busy || info.errors.length > 0 || !supported || plan.kind === 'full'}
                title={
                  info.errors.length
                    ? '先修校验错误'
                    : plan.kind === 'full'
                      ? plan.error
                      : `${describeSlotPlan(plan, task.name)}（点下去会先重读一次车上目录再定槽位）`
                }
              >
                <Save size={12} /> 烧写到车{plan.kind === 'full' ? '' : ` #${plan.slot}`}
              </button>
              <button
                className="btn !px-2 !py-0.5 text-[11px]"
                onClick={() => void doRun()}
                disabled={!!busy || !supported}
                title={carSlot === null ? '车上还没有这条任务 —— 先烧写' : `跑车上的 #${carSlot}`}
              >
                <Play size={12} /> 开始
              </button>
              <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void doStop()} disabled={!!busy || !supported}>
                <Square size={12} /> 停车
              </button>
              <Toggle on={monitor} onChange={setMonitor} label={<span className="text-[11px]">监视</span>} disabled={!supported} />
            </div>
          }
        >
          <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
            <Stat label="镜像长度" value={String(info.len)} unit="B" hint={`车上程序区 6 KB 里占 ${info.budgetPct}%`} />
            <Stat label="校验和" value={`0x${info.crc16.toString(16).toUpperCase().padStart(4, '0')}`} hint="从头 12 字节之后算起，烧完和车上回读的对比" />
            <Stat label="状态 / 块" value={`${prog.states.length} / ${blockCount(prog)}`} hint={`主页（任务从这里开始）= ${stateLabel(prog.main, stateNames)}`} />
            <Stat
              label="烧到哪个槽"
              value={plan.kind === 'full' ? '没空槽' : `#${plan.slot}${plan.kind === 'same-name' ? ' 覆盖' : ' 空槽'}`}
              tone={plan.kind === 'full' ? 'rose' : plan.kind === 'same-name' ? 'amber' : 'slate'}
              hint={
                plan.kind === 'full'
                  ? '车上 8 个槽都满了，先删一个'
                  : plan.kind === 'same-name'
                    ? '车上有同名任务：烧写=覆盖它'
                    : '车上没有同名任务：写进第一个空槽'
              }
              title={describeSlotPlan(plan, task.name)}
            />
          </div>

          {supported && (
            <div
              className={`mt-2 rounded-lg p-2 text-[11px] leading-relaxed ${
                plan.kind === 'full'
                  ? 'bg-neon-rose/10 text-neon-rose'
                  : plan.kind === 'same-name'
                    ? 'bg-neon-amber/10 text-neon-amber'
                    : 'bg-white/[0.03] text-slate-400'
              }`}
            >
              {describeSlotPlan(plan, task.name)}
              {sameNameInEditor > 1 && (
                <>
                  {' '}
                  <b>编辑器里有 {sameNameInEditor} 条同名任务</b> —— 它们都算"同一个任务"、都烧到这个槽，
                  后烧的覆盖先烧的。
                </>
              )}
              {nameOver && (
                <>
                  {' '}
                  名字有 {nameBytes(task.name)} 字节，车上目录页只存得下 {CAR_NAME_MAX} 字节 —— 会存成「
                  {fitCarName(task.name)}」。
                </>
              )}
              {plan.kind !== 'full' && <span className="text-slate-500">（点「烧写到车」会先重读一次车上目录再定槽位。）</span>}
            </div>
          )}

          {status?.running && (
            <div className="mt-2 rounded-lg bg-neon-mint/10 p-2 text-[11px] text-neon-mint">
              正在跑：任务 #{status.id} · {stateLabel(status.state, stateNames)} · 第 {status.block} 块 · 已跑 {status.elapsedMs} ms
              <span className="text-neon-amber">　跑着的时候车上拒收「烧写到车」和「删除」——先按「停车」。</span>
            </div>
          )}

          {info.errors.length > 0 && (
            <div className="mt-2 rounded-lg bg-neon-rose/10 p-2">
              <div className="mb-1 flex items-center gap-1.5 text-[12px] text-neon-rose">
                <AlertTriangle size={13} /> 固件会拒收这 {info.errors.length} 条（烧写按钮已禁用）
              </div>
              <ul className="ml-4 list-disc text-[11px] text-neon-rose/90">
                {info.errors.map((e) => (
                  <li key={e}>{e}</li>
                ))}
              </ul>
            </div>
          )}
          {info.warnings.length > 0 && (
            <div className="mt-2 rounded-lg bg-neon-amber/10 p-2">
              <div className="mb-1 text-[12px] text-neon-amber">能编过，但多半不是本意：</div>
              <ul className="ml-4 list-disc text-[11px] text-neon-amber/90">
                {info.warnings.map((w) => (
                  <li key={w}>{w}</li>
                ))}
              </ul>
            </div>
          )}
          {info.errors.length === 0 && info.warnings.length === 0 && (
            <div className="mt-2 text-[11px] text-slate-500">校验通过，可以烧。{busy ? `（${busy}中…）` : ''}</div>
          )}
          <div className="mt-2 text-[10px] leading-relaxed text-slate-600">
            车上只认二进制。任务名随 COMMIT 帧一起下发、存在车上目录页里（见 docs/任务编程.md §5）；
            烧写后会立刻回读比对，不一致会红字报出来。
            <br />
            烧到哪个槽的规则：车上有<b>同名</b>任务就覆盖它那个槽（不会多出一条副本）；没有同名就写进
            <b>第一个空槽</b>；8 个槽全满又没有同名会被拦下来，先删一个再烧。
            跑任务：车上按任务页中键（离线），或这里按「开始」跑车上那个槽。
          </div>
        </Card>

        <Card
          title="状态机"
          right={
            <div className="flex items-center gap-2">
              <span className="text-[10px] text-slate-600">{prog.states.length} 个状态</span>
              <button
                className="btn-ghost !px-2 !py-0.5 text-[11px]"
                onClick={() => {
                  const n = prog.states.length
                  upd((p) => addState(p))
                  setSelState(Math.min(n, 15))
                }}
                disabled={prog.states.length >= 16}
              >
                <Plus size={12} /> 加状态
              </button>
            </div>
          }
        >
          <div className="mb-2 text-[11px] text-slate-500">
            一屏只看一个状态：任务从「主页」开始跑，别的状态靠「跳到状态 / 超时改道」切进去。点上面的标签换状态，也可以把块拖到标签上再松手。
            进门序列在被切进来时跑一遍；循环体跑完停在最后一块等条件。把右边的块拖到蓝线处插入，点块改参数。
          </div>

          {/* 状态标签条：主页永远排第一，其余按编号排；一屏只渲染选中的那一个状态 */}
          <div className="mb-2 flex flex-wrap items-center gap-1">
            {[prog.main, ...prog.states.map((_, i) => i).filter((j) => j !== prog.main)].map((i) => (
              <button
                key={i}
                onClick={() => setSelState(i)}
                onDragOver={() => setSelState(i)}
                title={prog.main === i ? `任务启动时从这个状态开始（${stateLabel(i, stateNames)}）` : stateLabel(i, stateNames)}
                className={`flex items-center gap-1.5 rounded-lg border px-2 py-1 text-[11px] ${
                  activeState === i ? 'border-neon-cyan/50 bg-neon-cyan/10 text-neon-cyan' : 'border-white/10 text-slate-400 hover:border-neon-cyan/30'
                }`}
              >
                {prog.main === i && <Badge tone="mint">主页</Badge>}
                <span className="num text-slate-500">{i}</span>
                <span>{prog.states[i].name || `状态 ${i}`}</span>
                <span className="num text-slate-600">
                  {prog.states[i].enter.length}+{prog.states[i].body.length}
                </span>
                {!!cur && cur.state === i && <span className="h-1.5 w-1.5 rounded-full bg-neon-mint" title="正在跑这个状态" />}
              </button>
            ))}
          </div>

          <StateCard
            prog={prog}
            i={activeState}
            sel
            open={open}
            dropAt={dropAt}
            cur={cur}
            onSelect={() => setSelState(activeState)}
            onDrop={onDrop}
            onOver={setDropAt}
            onToggleBlock={(at) => setOpen((o) => (o && o.state === at.state && o.seq === at.seq && o.index === at.index ? null : at))}
            upd={upd}
          />
        </Card>
      </div>

      {/* 右：块库 */}
      <div className="grid content-start gap-3">
        <Card title="块库" right={<span className="text-[10px] text-slate-600">拖到左边，或点一下加到 {stateLabel(activeState, stateNames)}</span>}>
          {PALETTE_GROUPS.map((g) => (
            <div key={g} className="mb-2">
              <div className="mb-1 text-[10px] uppercase tracking-wider text-slate-500">{g}</div>
              <div className="grid gap-1">
                {OP_SPECS.filter((s) => s.group === g).map((s) => (
                  <div
                    key={s.op}
                    draggable
                    onDragStart={(e) => {
                      e.dataTransfer.setData(DRAG_MIME, encodeDrag({ kind: 'new', op: s.op }))
                      e.dataTransfer.effectAllowed = 'copy'
                    }}
                    onClick={() => paletteAdd(s.op)}
                    className="cursor-grab rounded-lg border border-white/10 bg-white/[0.03] px-2 py-1.5 hover:border-neon-cyan/40"
                    title={
                      s.fields.length
                        ? `${s.label}：${s.doc}\n\n可设置（鼠标停在左边那块的具体字段上也有说明）：\n` +
                          s.fields.map((f) => `· ${f.label}：${f.hint}`).join('\n')
                        : `${s.label}：${s.doc}`
                    }
                  >
                    <div className="text-[12px] text-neon-cyan/90">{s.label}</div>
                    <div className="text-[10px] leading-snug text-slate-400">{s.doc}</div>
                    {s.fields.some((f) => !f.showIf) && (
                      <div className="mt-0.5 text-[10px] text-slate-600">可设置：{s.fields.filter((f) => !f.showIf).map((f) => f.label).join(' / ')}</div>
                    )}
                  </div>
                ))}
              </div>
            </div>
          ))}
        </Card>

        <Card title="这份程序长什么样">
          <div className="text-[11px] leading-relaxed text-slate-500">
            每个状态有「进门」序列（进入这个状态时跑一遍）和「循环体」（跑完停在最后一块等条件）。「等条件」是唯一能卡住的块，
            其它块都占一拍就走。循环体末尾必须是「跳到状态 / 结束 / 带超时改道的等条件」，否则车会保持最后一个模式一直开 —— 这就是校验里那条错误。
          </div>
          <div className="mt-2 num text-[10px] leading-relaxed text-slate-600" title="这是车上真正认的二进制格式，写给想核对的人看">
            文件开头固定 4B54 · 版本 1 · 文件头 12 字节 · 每个状态 8 字节 · 每个块 10 字节 · 校验方式 CRC16/CCITT · 车上 5 毫秒跑一拍 · 最多 16 个状态、每段最多 64 块
          </div>
          <div className="mt-2 text-[10px] text-slate-600">
            快捷键：拖块改顺序；◀▶ 微调；点块展开改参数；同一块再点一下收起。
          </div>
        </Card>
      </div>
    </div>
  )
}

export type { TaskProgram, RemoteTaskInfo, TaskStatus }
