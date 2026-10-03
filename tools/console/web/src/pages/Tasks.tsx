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
import { store } from '../lib/store'
import { MODE, OP, decodeTask, encodeTask, type TaskBlock, type TaskProgram } from '../lib/task'
import {
  COND_SPECS,
  DRAG_MIME,
  EDIT_MAX_TASKS,
  MODE_OPTIONS,
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
  importTaskFile,
  insertBlock,
  loadTaskFile,
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
  duplicateBlock,
  updateBlock,
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
  onChange,
}: {
  b: TaskBlock
  at: Where
  stateCount: number
  onChange: (patch: Partial<TaskBlock>) => void
}) {
  const spec = OP_BY_CODE.get(b.op)
  if (!spec) return <div className="text-[12px] text-neon-rose">未知块 0x{b.op.toString(16)}（固件会当非法程序拒收）</div>

  const setField = (key: 'p0' | 'p1' | 'p2' | 'p3', v: number) => {
    // 两轮直给要同时点亮 flags.bit0（固件靠它区分"模式 4"与"模式 0"）
    if (b.op === OP.SET_MODE && key === 'p0') {
      onChange({ p0: v, flags: v === MODE.WHEELS ? b.flags | 1 : b.flags & ~1 })
      return
    }
    onChange({ [key]: v } as Partial<TaskBlock>)
  }

  return (
    <div className="mt-1.5 grid gap-1.5 rounded-lg bg-black/20 p-2">
      {spec.doc && <div className="text-[11px] text-slate-500">{spec.doc}</div>}
      {b.op === OP.TURN_DEG && (
        <Toggle on={(b.flags & 1) !== 0} onChange={(v) => onChange({ flags: v ? b.flags | 1 : b.flags & ~1 })} label={<span className="text-[12px]">相对当前朝向（关掉 = 绝对角度）</span>} />
      )}
      {spec.fields
        .filter((f) => !f.showIf || f.showIf(b))
        .map((f) => {
          const v = b[f.key]
          if (f.kind === 'mode') {
            return (
              <label key={f.key} className="flex items-center gap-2 text-[12px] text-slate-400">
                <span className="w-14 shrink-0">{f.label}</span>
                <select className="field flex-1" value={v} onChange={(e) => setField(f.key, Number(e.target.value))}>
                  {MODE_OPTIONS.map((m) => (
                    <option key={m.value} value={m.value}>
                      {m.label}
                    </option>
                  ))}
                </select>
              </label>
            )
          }
          if (f.kind === 'cond') {
            return (
              <label key={f.key} className="flex items-center gap-2 text-[12px] text-slate-400">
                <span className="w-14 shrink-0">{f.label}</span>
                <select className="field flex-1" value={v} onChange={(e) => setField(f.key, Number(e.target.value))}>
                  {COND_SPECS.map((c) => (
                    <option key={c.cond} value={c.cond}>
                      {c.label}
                    </option>
                  ))}
                </select>
              </label>
            )
          }
          if (f.kind === 'state') {
            return (
              <label key={f.key} className="flex items-center gap-2 text-[12px] text-slate-400">
                <span className="w-14 shrink-0">{f.label}</span>
                <select className="field flex-1" value={v} onChange={(e) => setField(f.key, Number(e.target.value))}>
                  <option value={0xffff}>继续往下跑</option>
                  {Array.from({ length: stateCount }, (_, i) => (
                    <option key={i} value={i}>
                      状态 {i}
                    </option>
                  ))}
                </select>
              </label>
            )
          }
          if (f.kind === 'servo') {
            return (
              <label key={f.key} className="flex items-center gap-2 text-[12px] text-slate-400">
                <span className="w-14 shrink-0">{f.label}</span>
                <select className="field flex-1" value={v} onChange={(e) => setField(f.key, Number(e.target.value))}>
                  <option value={1}>1 号</option>
                  <option value={2}>2 号</option>
                </select>
              </label>
            )
          }
          return (
            <label key={f.key} className="flex items-center gap-2 text-[12px] text-slate-400">
              <span className="w-14 shrink-0">{f.label}</span>
              <NumberField value={v} min={f.lo} max={f.hi} onChange={(nv) => setField(f.key, nv)} className="w-24" />
              {f.unit && <span className="text-[11px] text-slate-600">{f.unit}</span>}
              {f.unit === 'PWM 刻度' && v > 0 && v < 2600 && <Badge tone="amber">可能推不动（&lt;35% 死区）</Badge>}
            </label>
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
          <span className="text-neon-cyan/90">{spec?.label ?? `0x${b.op.toString(16)}`}</span>
          <span className="ml-1.5 text-slate-400">{describeBlock(b, stateCount)}</span>
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
      {open && <FieldEditor b={b} at={at} stateCount={stateCount} onChange={onPatch} />}
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
        <span className="num text-[11px] text-slate-500">状态 {i}</span>
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
  const [autoRun, setAutoRun] = React.useState(false)   // 烧写时下发 flags.bit0（开机自启）
  const [busy, setBusy] = React.useState('')
  const fileRef = React.useRef<HTMLInputElement | null>(null)

  React.useEffect(() => {
    writeStored(serializeTaskFile(file))
  }, [file])

  React.useEffect(() => {
    if (!monitor) return
    let alive = true
    const tick = async () => {
      const r = await taskApi.status()
      if (!alive) return
      if (r.ok) setStatus(r.data)
      else {
        setMonitor(false)
        store.toast('err', r.error)
      }
    }
    void tick()
    const id = window.setInterval(() => void tick(), 500)
    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [monitor])

  const task: TaskDef = file.tasks[Math.min(selTask, file.tasks.length - 1)] ?? file.tasks[0]
  const prog = task.prog
  const info = compileInfo(prog)
  const bytes = encodeTask(prog).bytes
  const activeState = Math.min(selState, prog.states.length - 1)

  const updTask = (t: TaskDef) => setFile((f) => ({ ...f, tasks: f.tasks.map((x) => (x === task ? t : x)) }))
  const upd = (fn: (p: TaskProgram) => TaskProgram) => updTask({ ...task, prog: fn(task.prog) })

  const refreshRemote = async () => {
    setBusy('读目录')
    const r = await taskApi.list()
    setBusy('')
    if (!r.ok) {
      setRemote({ supported: r.supported, items: [] })
      if (r.supported) store.toast('err', r.error)
      else store.toast('info', '桥/固件还没支持任务功能 —— 编辑照常，烧写等固件')
      return
    }
    setRemote(r.data)
    store.toast('ok', `车上 ${r.data.items.length} 个任务`)
  }

  const doBurn = async () => {
    if (info.errors.length) {
      store.toast('err', '有校验错误，先修好再烧')
      return
    }
    setBusy('烧写')
    const r = await taskApi.burn(task.id, bytes, info.len, info.crc16, task.name, autoRun ? 1 : 0)
    if (!r.ok) {
      setBusy('')
      store.toast('err', r.error)
      return
    }
    const back = await taskApi.get(task.id)
    setBusy('')
    if (!back.ok) {
      store.toast('info', `写进去了但回读失败：${back.error}`)
      return
    }
    const same = back.data.length === bytes.length && back.data.every((v, i) => v === bytes[i])
    if (same) store.toast('ok', `任务 ${task.id}「${task.name}」烧写并回读校验一致（${r.data.chunks} 片${autoRun ? '，已设开机自启' : ''}）`)
    else store.toast('err', `回读不一致（车上 ${back.data.length} B / 本地 ${bytes.length} B）—— 别让车跑这个任务`)
    void refreshRemote()
  }

  const doRun = async () => {
    setBusy('启动')
    const r = await taskApi.run(task.id)
    setBusy('')
    if (r.ok) {
      store.toast('ok', `已启动任务 ${task.id}`)
      setMonitor(true)
    } else store.toast('err', r.error)
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

  const cur = monitor && status ? { state: status.state, block: status.block } : null
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
                  <span className="num text-[10px] text-slate-500">#{t.id}</span>
                  <span className="min-w-0 flex-1 truncate text-[12px] text-slate-200">{t.name}</span>
                  {status?.running && status.id === t.id && <Badge tone="mint">跑着</Badge>}
                </div>
                <div className="text-[10px] text-slate-600">
                  {t.prog.states.length} 状态 · {blockCount(t.prog)} 块 · {encodeTask(t.prog).bytes.length} B
                </div>
              </button>
            ))}
            <div className="mt-1 flex flex-wrap items-center gap-1">
              <input
                className="field h-7 flex-1 text-[12px]"
                value={task.name}
                onChange={(e) => updTask({ ...task, name: e.target.value.slice(0, 32) })}
                placeholder="任务名（只存在上位机）"
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
                  store.toast('ok', `导入 ${tasks.length} 个任务${skipped ? `，跳过 ${skipped} 个` : ''}`)
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
            <div className="rounded-lg bg-neon-amber/10 p-2 text-[11px] leading-relaxed text-neon-amber">
              桥或固件还没实现任务帧（0x60–0x69），这一栏暂时只有编辑与编译可用。
              <br />
              等固件开了 <span className="num">HOST_CAP_TASK</span> 就能烧写。
            </div>
          )}
          {remote && remote.supported && remote.items.length === 0 && <div className="text-[12px] text-slate-500">车上一个任务都没有。</div>}
          {remote?.items.map((it) => (
            <div key={it.id} className="mb-1 flex items-center gap-1.5 rounded-lg border border-white/10 px-2 py-1">
              <span className="num text-[10px] text-slate-500">#{it.id}</span>
              <span className="min-w-0 flex-1 truncate text-[12px] text-slate-300">{it.name || '（车上无名）'}</span>
              {it.flags & 1 ? <span className="rounded bg-neon-mint/15 px-1 text-[10px] text-neon-mint">自启</span> : null}
              <span className="num text-[10px] text-slate-600">{it.len}B</span>
              <button className="text-[11px] text-slate-500 hover:text-neon-cyan" onClick={() => void pullRemote(it)} title="读回车上的程序">
                <Download size={12} />
              </button>
              <button
                className="text-[11px] text-slate-500 hover:text-neon-rose"
                title="从车上删掉"
                onClick={async () => {
                  const r = await taskApi.del(it.id)
                  if (r.ok) {
                    store.toast('ok', `车上任务 ${it.id} 已删`)
                    void refreshRemote()
                  } else store.toast('err', r.error)
                }}
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
              <button className="btn-primary !px-2 !py-0.5 text-[11px]" onClick={() => void doBurn()} disabled={!!busy || info.errors.length > 0 || !supported} title={info.errors.length ? '先修校验错误' : '写进车的 Flash'}>
                <Save size={12} /> 烧写到车
              </button>
              <button className="btn !px-2 !py-0.5 text-[11px]" onClick={() => void doRun()} disabled={!!busy || !supported}>
                <Play size={12} /> 开始
              </button>
              <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => void doStop()} disabled={!!busy || !supported}>
                <Square size={12} /> 停车
              </button>
              <Toggle on={autoRun} onChange={setAutoRun} disabled={!supported} label={<span className="text-[11px]" title="烧写时下发 flags.bit0：车上开机自己跑这个任务，不用电脑点「开始」">开机自启</span>} />
              <Toggle on={monitor} onChange={setMonitor} label={<span className="text-[11px]">监视</span>} disabled={!supported} />
            </div>
          }
        >
          <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
            <Stat label="镜像长度" value={String(info.len)} unit="B" hint={`车上程序区 6 KB 里占 ${info.budgetPct}%`} />
            <Stat label="CRC16" value={`0x${info.crc16.toString(16).toUpperCase().padStart(4, '0')}`} hint="覆盖 [12, len)，与车上回读的比" />
            <Stat label="状态 / 块" value={`${prog.states.length} / ${blockCount(prog)}`} hint={`主页 = 状态 ${prog.main}`} />
            <Stat label="任务编号" value={`#${task.id}`} hint="车上的槽位号，0~7" />
          </div>

          {status && monitor && (
            <div className="mt-2 rounded-lg bg-neon-mint/10 p-2 text-[11px] text-neon-mint">
              正在跑：任务 #{status.id} · 状态 {status.state} · 第 {status.block} 块 · 已跑 {status.elapsedMs} ms
              {status.flags ? ` · flags=0x${status.flags.toString(16)}` : ''}
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
            车上只认二进制。任务名与「开机自启」标志随 COMMIT 帧一起下发、存在车上目录页里（见 docs/任务编程.md §5）；
            烧写后会立刻回读比对，不一致会红字报出来。
          </div>
        </Card>

        <Card
          title="状态机"
          right={
            <button className="btn-ghost !px-2 !py-0.5 text-[11px]" onClick={() => upd((p) => addState(p))} disabled={prog.states.length >= 16}>
              <Plus size={12} /> 加状态
            </button>
          }
        >
          <div className="mb-2 text-[11px] text-slate-500">
            从左到右、从上到下就是这个状态里的执行顺序；把右边的块拖到蓝线处插入，拖动已有的块改顺序。点块改参数。
          </div>
          <div className="grid gap-2">
            {prog.states.map((_, i) => (
              <div key={i} onDragOver={() => setSelState(i)}>
                <StateCard
                  prog={prog}
                  i={i}
                  sel={selState === i}
                  open={open}
                  dropAt={dropAt}
                  cur={cur}
                  onSelect={() => setSelState(i)}
                  onDrop={onDrop}
                  onOver={setDropAt}
                  onToggleBlock={(at) => setOpen((o) => (o && o.state === at.state && o.seq === at.seq && o.index === at.index ? null : at))}
                  upd={upd}
                />
              </div>
            ))}
          </div>
        </Card>
      </div>

      {/* 右：块库 */}
      <div className="grid content-start gap-3">
        <Card title="块库" right={<span className="text-[10px] text-slate-600">拖到左边，或点一下加到状态 {activeState}</span>}>
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
                    title={s.doc}
                  >
                    <div className="text-[12px] text-neon-cyan/90">{s.label}</div>
                    <div className="text-[10px] leading-snug text-slate-500">{s.doc}</div>
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
          <div className="mt-2 num text-[10px] leading-relaxed text-slate-600">
            magic 0x4B54 · ver 1 · 头 12 B · 状态表 8 B/个 · 块 10 B/个 · CRC16/CCITT · 5 ms 一拍 · 上限 16 状态 / 64 块
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
