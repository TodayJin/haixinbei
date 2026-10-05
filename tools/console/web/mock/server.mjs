#!/usr/bin/env node
/**
 * 假后端（零依赖，只用 node:http）—— 按 tools/console/docs/API.md 的冻结契约实现全部端点 + SSE。
 *
 * 用途：没有小车、没有 Python 桥时开发和截图自检。
 *   node mock/server.mjs            # 默认 8770，并托管 ../dist（先 npm run build）
 *   PORT=9000 node mock/server.mjs
 *
 * 数据全是合成的：yaw 缓慢漂移、pitch/roll 抖动、8 路灰度扫过一条"线"、
 * 电压 12V 缓降、电流跟着 PWM 走、偶尔丢线。动作表用固件里 14 个动作的真实默认角度。
 */

import http from 'node:http'
import { readFile, stat } from 'node:fs/promises'
import { extname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = resolve(fileURLToPath(import.meta.url), '..')
const DIST = resolve(__dirname, '..', 'dist')
const PORT = Number(process.env.PORT || 8770)
const HOST = '127.0.0.1'

// ---------------------------------------------------------------------------
// 状态（模拟"车 + 桥"）
// ---------------------------------------------------------------------------
const t0 = Date.now()

const params = [
  p(0x0201, 'line_kp', '循迹 Kp', '循迹环', '', 2700, 0, 5000, 1, 1),
  p(0x0202, 'line_ki', '循迹 Ki', '循迹环', '', 10.8, 0, 200, 2, 0.1),
  p(0x0203, 'line_kd', '循迹 Kd', '循迹环', '', 360, 0, 1000, 1, 1),
  p(0x0204, 'line_ilimit', '循迹积分限幅', '循迹环', '', 50, 0, 10000, 0, 1),
  p(0x0301, 'base_speed', '循迹速度', '速度环', '', 6480, 0, 7200, 0, 10),
  p(0x0302, 'base_speed_slow', '循迹慢速', '速度环', '', 2500, 0, 7200, 0, 10),
  p(0x0304, 'task_speed', '任务流程速度', '任务', '', 6480, 0, 7200, 0, 10),
  p(0x0601, 'speed_kp', '速度环 Kp', '速度环', '', 20, 0, 2000, 2, 0.5),
  p(0x0602, 'speed_ki', '速度环 Ki', '速度环', '', 2, 0, 200, 2, 0.1),
  p(0x0603, 'speed_kd', '速度环 Kd', '速度环', '', 0, 0, 500, 2, 0.1),
  p(0x0604, 'speed_ilimit', '速度环积分限幅', '速度环', '', 3000, 0, 20000, 0, 100),
  p(0x0605, 'speed_ff_slope', '摩擦前馈斜率', '速度环', '', 0.662, 0, 2, 3, 0.001, '换电机要重标'),
  p(0x0606, 'speed_ff_fric', '摩擦前馈常数', '速度环', '', 2327, 0, 5000, 0, 10, '换电机要重标'),
  p(0x0101, 'gyro_kp', '角度环 Kp', '角度环', '', 150, 0, 2000, 1, 1),
  p(0x0102, 'gyro_ki', '角度环 Ki', '角度环', '', 0, 0, 200, 2, 0.1),
  p(0x0103, 'gyro_kd', '角度环 Kd', '角度环', '', 15, 0, 500, 1, 1),
  p(0x0104, 'gyro_ilimit', '角度环积分限幅', '角度环', '', 1000, 0, 20000, 0, 50),
  p(0x0105, 'turn_angle_offset', '转弯角度', '任务', '°', 90, 1, 180, 0, 1),
  p(0x0401, 'turn_tolerance', '转弯到位容差', '任务', '°', 2, 0.1, 20, 1, 0.5),
  p(0x0402, 'turn_timeout', '转弯超时', '任务', 'ms', 1500, 100, 10000, 0, 50),
  p(0x0403, 'yaw_rate_damp', '陀螺阻尼', '角度环', '', 1, 0, 20, 2, 0.1),
  p(0x0404, 'marker_hold', '黑标保持', '任务', '', 3, 1, 50, 0, 1),
  p(0x0405, 'marker_min_gap', '黑标最小间隔', '任务', 'ms', 50, 0, 5000, 0, 5),
  p(0x0305, 'counts_per_rev', '编码器每圈计数', '机械/车辆', '', 1980, 1, 100000, 0, 10),
  p(0x0306, 'wheel_dia', '轮径', '机械/车辆', 'mm', 65, 10, 300, 1, 0.5),
  p(0x0307, 'wheel_base', '轮距', '机械/车辆', 'mm', 150, 10, 500, 0, 1),
  p(0x0303, 'pwm_max', 'PWM 满量程', '机械/车辆', '', 7200, 100, 7200, 0, 100),
  p(0x0501, 'vbat_nominal', '额定电压', '电源', 'mV', 12000, 3000, 30000, 0, 100),
  p(0x0502, 'stall_current', '堵转电流', '电源', 'mA', 2500, 100, 20000, 0, 100),
  p(0x0503, 'vbat_low', '低压告警', '电源', 'mV', 10500, 3000, 30000, 0, 100),
  // 串级那两个标定量：桥本地维护（固件里是运行期变量，不上参数表）
  p(0x0701, 'angle_per_unit', '串级 每格多少度', '串级', '°/单位', 10, 0.5, 60, 2, 0.5, '重心偏 1 单位 -> 目标航向偏移'),
  p(0x0702, 'angle_max', '串级 航向偏移上限', '串级', '°', 35, 1, 90, 1, 1),
]

function p(id, key, label, group, unit, def, min, max, decimals, step, note) {
  return { id, key, label, group, unit, value: def, default: def, min, max, decimals, step, dirty: false, note }
}

const ACTION_DEFAULTS = [
  [1, '收回', 0, 0],
  [2, '打下准备', 60, 120],
  [3, '打下1', 105, 120],
  [4, '打下2', 135, 150],
  [5, '台阶2收回', 80, 170, '135/170 →400ms→ 90 →100ms→ 80'],
  [6, '大坡1', 100, 120],
  [7, '三级12', 95, 120],
  [8, '三级2收回', 40, 10],
  [9, '三级3打', 100, 80, '70/10 →200ms→ 100/50 →100ms→ 100/80'],
  [10, '高台', 115, 120],
  [11, '高台下', 85, 120],
  [12, '收回2', 40, 0],
  [13, '打下1(另一路)', 90, 60],
  [14, '打下准备(另一路)', 30, 60],
]

let actions = ACTION_DEFAULTS.map(([n, name, a1, a2, steps]) => ({
  n,
  name,
  a1,
  a2,
  preset_a1: a1,
  preset_a2: a2,
  dirty: false,
  compound: n === 5 || n === 9,
  steps,
}))

let presets = []

const link = {
  ok: true,
  mode: 'sim',
  port: null,
  baud: 921600,
  connected: true,
  stats: { rx: 0, tx: 0, crc: 0, resync: 0, drops: 0 },
  telem_hz: 50,
  last_rx_ms: 0,
  rtt_ms: 4,
  fw: {
    name: 'haixin-app',
    version: '1.11.0-mock',
    proto: 2,
    param_crc: crc16(JSON.stringify(params.map((x) => x.value))),
    telem_hz: 50,
    caps: 0b1111111,
  },
}

const cal = {
  ok: true,
  active_set: 0,
  quality: 0,
  ch: Array.from({ length: 8 }, (_, i) => ({
    i,
    blk: 820 + i * 6,
    wht: 3100 + i * 12,
    thr: (820 + i * 6 + 3100 + i * 12) >> 1,
    jitter: 3 + (i % 3),
    pol: 0,
    ok: i !== 6,
    reason: i === 6 ? 2 : 0,
    level: i >= 3 && i <= 4 ? 850 : 3050,
    thr_level: 1900,
  })),
}

let drive = { mode: 'off', vl: 0, vr: 0, base: 0, turn: 0 }
let servo = { ch1: 0, ch2: 0, dev1: 0, dev2: 0 }
let telemHz = 50
let sim = { yaw: -64.7, pitch: 0.4, roll: -0.8, vl: 0, vr: 0, vbat: 12040, linePos: 4.5, action: 1, nvram: 0 }
const logs = []
const clients = new Set()
let lastStats = Date.now()

function log(level, text) {
  const item = { ts_ms: Date.now(), level, text }
  logs.push(item)
  if (logs.length > 500) logs.shift()
  send('log', item)
}

function ack(req, ok = true, status = 0, text = 'OK') {
  const item = { seq: (ack.seq = (ack.seq || 0) + 1), req, status, ok, text }
  send('ack', item)
  return item
}

/** CRC16/CCITT-FALSE。字符串按 UTF-16 码元（老行为，ASCII 等价）、Uint8Array/Buffer 按原始字节。 */
function crc16(bytes) {
  let crc = 0xffff
  if (typeof bytes === 'string') {
    for (const ch of bytes) {
      crc ^= ch.charCodeAt(0) << 8
      for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff
    }
    return crc
  }
  for (const b of bytes) {
    crc ^= b << 8
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff
  }
  return crc
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------
function send(event, data) {
  const payload = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`
  for (const res of clients) res.write(payload)
}

function paramsResponse() {
  return {
    ok: true,
    param_crc: link.fw.param_crc,
    source: 'fw',
    items: params.map((x) => ({ ...x })),
  }
}

function stateEvent() {
  return {
    link,
    params: paramsResponse(),
    actions: { ok: true, items: actions },
    presets,
    cal,
  }
}

/** 合成一帧遥测 */
function makeTelem() {
  const now = Date.now()
  const s = (now - t0) / 1000
  const driving = drive.mode !== 'off'
  const targetL = driving ? (drive.mode === 'wheels' ? drive.vl : drive.base + drive.turn) : 0
  const targetR = driving ? (drive.mode === 'wheels' ? drive.vr : drive.base - drive.turn) : 0

  // 一阶逼近 + 一点噪声
  sim.vl += (targetL - sim.vl) * 0.12 + (Math.random() - 0.5) * 12
  sim.vr += (targetR - sim.vr) * 0.12 + (Math.random() - 0.5) * 12
  sim.pwm_l = Math.round(sim.vl * 0.662 + 2327 * Math.sign(sim.vl || 1) * (driving ? 1 : 0))
  sim.pwm_r = Math.round(sim.vr * 0.662 + 2327 * Math.sign(sim.vr || 1) * (driving ? 1 : 0))

  sim.yaw = -64.7 + Math.sin(s * 0.22) * 18 + Math.sin(s * 1.7) * 0.6
  sim.pitch = 0.4 + Math.sin(s * 0.9) * 1.6 + (Math.random() - 0.5) * 0.3
  sim.roll = -0.8 + Math.cos(s * 0.7) * 2.2 + (Math.random() - 0.5) * 0.3

  // 8 路灰度：一条宽 2 路的"线"在阵列上来回扫
  sim.linePos = 3.5 + Math.sin(s * 0.5) * 3.1
  const gray = []
  const line = []
  for (let i = 0; i < 8; i++) {
    const d = Math.abs(i - sim.linePos)
    const ink = Math.max(0, 1 - d / 1.6)
    // 固件口径：u8 0..255，越高越黑（255 = 压黑线）—— host_ext.h:80 trace_cal_level()
    gray.push(Math.round(30 + ink * 195 + (Math.random() - 0.5) * 14))
    line.push(ink > 0.5 ? 1 : 0)
  }
  const lost = Math.random() < 0.004
  if (lost) for (let i = 0; i < 8; i++) { gray[i] = 6 + Math.round(Math.random() * 10); line[i] = 0 }

  sim.vbat = Math.max(10200, sim.vbat - 0.35)
  const ibat = Math.round(Math.max(0, (Math.abs(sim.pwm_l) + Math.abs(sim.pwm_r)) * 0.09) + 120 + (Math.random() - 0.5) * 40)

  const err = Math.sin(s * 1.3) * 6
  return {
    t: now - t0,
    t_host: now,
    yaw: round(sim.yaw, 2),
    pitch: round(sim.pitch, 2),
    roll: round(sim.roll, 2),
    vl: round(sim.vl, 1),
    vr: round(sim.vr, 1),
    pwm_l: sim.pwm_l || 0,
    pwm_r: sim.pwm_r || 0,
    vbat_mv: Math.round(sim.vbat),
    ibat_ma: ibat,
    servo1_us: Math.round(500 + (servo.ch1 / 180) * 2000) + servo.dev1,
    servo2_us: Math.round(500 + (servo.ch2 / 180) * 2000) + servo.dev2,
    servo1_deg: round(servo.ch1, 1),
    servo2_deg: round(servo.ch2, 1),
    line,
    gray,
    state_id: taskRun.running ? taskRun.state : 0xff,   // 解释器当前状态号；0xFF = 没在跑
    loop_us: 900 + Math.round(Math.random() * 260),
    mode: driving ? 3 : 0,
    task_id: taskRun.running ? taskRun.id : 0xff,       // 当前任务号；0xFF = 没在跑
    action: sim.action,
    trace: { pos: round(sim.linePos, 2), pid: Math.round((4.5 - sim.linePos) * 260) },
    angle: { err: round(err, 2), rate_ref: round(err * 12, 1), corr: Math.round(err * 30) },
    flags: {
      lost,
      slope: false,
      gyro_ok: true,
      motor: driving,
      host_drive: driving,
      logging: false,
      flash: sim.nvram === 2,
      fault: false,
    },
    nvram: sim.nvram,
  }
}

function round(v, d) {
  const k = 10 ** d
  return Math.round(v * k) / k
}

let telemTimer = null
function startTimers() {
  if (telemTimer) clearInterval(telemTimer)
  telemTimer = setInterval(() => {
    if (link.connected) {
      const t = makeTelem()
      link.stats.rx++
      link.stats.tx += 3
      link.last_rx_ms = 0
      send('telem', t)
      if (Math.random() < 0.02) link.stats.drops += 1
    }
    if (Date.now() - lastStats > 1000) {
      lastStats = Date.now()
      link.telem_hz = telemHz
      send('stats', {
        stats: { ...link.stats },
        telem_hz: telemHz,
        last_rx_ms: link.connected ? 0 : null,
        busy_pct: round(3 + Math.random() * 6, 1),
      })
    }
  }, Math.max(10, Math.round(1000 / telemHz)))
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.map': 'application/json; charset=utf-8',
  '.woff2': 'font/woff2',
}

function json(res, code, body) {
  const text = JSON.stringify(body)
  res.writeHead(code, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' })
  res.end(text)
}

const fail = (res, code, error, why) => json(res, code, { ok: false, error, code: why })

async function readBody(req) {
  const chunks = []
  for await (const c of req) chunks.push(c)
  if (chunks.length === 0) return {}
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8'))
  } catch {
    return null
  }
}

// ---------------------------------------------------------------------------
// 任务页（契约 tools/console/docs/API.md §3.6）—— 假后端也存一份，好让 #/tasks 页能整条走通
// ---------------------------------------------------------------------------
const tasks = new Map()      // id -> { name, buf: Buffer }
const staging = new Map()    // id -> Buffer（put 上来还没 commit 的）
let taskRun = { id: 0, running: false, state: 0, block: 0, nStates: 1, nBody: 1, t0: 0, flags: 0 }

function taskStatus() {
  if (!taskRun.running) return { id: taskRun.id, state: 0, running: 0, block: 0, elapsed_ms: 0, elapsedMs: 0, flags: 0 }
  const elapsed = Date.now() - taskRun.t0
  // 让监视画面动起来：每 400ms 走一块，走到序列尾就换下一个状态
  const step = Math.floor(elapsed / 400)
  const state = (taskRun.state + Math.floor(step / Math.max(1, taskRun.nBody))) % Math.max(1, taskRun.nStates)
  return {
    id: taskRun.id, state, running: 1, block: step % Math.max(1, taskRun.nBody),
    elapsed_ms: elapsed, elapsedMs: elapsed, flags: 0,
  }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || HOST}`)
  const path = url.pathname

  try {
    if (path === '/api/health') {
      return json(res, 200, { ok: true, version: 'mock-1.0', uptime_ms: Date.now() - t0, sim: true })
    }

    if (path === '/api/link' && req.method === 'GET') return json(res, 200, link)

    if (path === '/api/link/connect' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      // 回放模式 2026-10-04 已按用户要求删除：这里也必须与桥一致地拒绝
      if (!['sim', 'serial'].includes(body.mode)) return fail(res, 400, 'mode 必须是 sim/serial', 'bad_param')
      link.mode = body.mode
      link.port = body.port ?? null
      link.baud = body.baud ?? 921600
      link.connected = true
      link.note = ''
      log('info', `已连接：${link.mode}${link.mode === 'serial' && link.port ? ' ' + link.port : ''}`)
      ack('link/connect')
      send('link', link)
      return json(res, 200, { ok: true, link })
    }

    if (path === '/api/link/disconnect' && req.method === 'POST') {
      link.connected = false
      log('warn', '链路已断开')
      ack('link/disconnect')
      send('link', link)
      return json(res, 200, { ok: true })
    }

    if (path === '/api/ports') {
      return json(res, 200, {
        ok: true,
        ports: [
          { port: 'COM26', desc: 'USB-SERIAL CH340（无线下载器）' },
          { port: 'COM12', desc: 'STMicroelectronics STLink Virtual COM Port' },
          { port: 'COM4', desc: '蓝牙链接上的标准串行' },
        ],
      })
    }

    if (path === '/api/params' && req.method === 'GET') return json(res, 200, paramsResponse())

    if (path === '/api/params/set' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const item = params.find((x) => (body.id !== undefined ? x.id === body.id : x.key === body.key))
      if (!item) return fail(res, 404, '参数不存在', 'not_found')
      if (typeof body.value !== 'number' || !Number.isFinite(body.value)) return fail(res, 400, 'value 必须是有限数', 'bad_param')
      if (body.value < item.min || body.value > item.max) {
        return fail(res, 400, `${item.label} 超出范围 ${item.min}..${item.max}`, 'out_of_range')
      }
      item.value = body.value
      item.dirty = item.value !== item.default
      ack('params/set')
      send('params', paramsResponse())
      return json(res, 200, { ok: true, param: { ...item } })
    }

    if (path === '/api/params/push' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const keys = body.keys || []
      const n = keys.length || params.filter((x) => x.dirty).length
      for (const x of params) if (!keys.length || keys.includes(x.key)) x.dirty = false
      log('info', `下发 ${n} 个参数到小车`)
      ack('params/push')
      send('params', paramsResponse())
      return json(res, 200, { ok: true, pushed: n })
    }

    if (path === '/api/params/save' && req.method === 'POST') {
      sim.nvram = 2
      setTimeout(() => (sim.nvram = 3), 600)
      log('info', '小车正在把参数写进 Flash…')
      ack('params/save')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/params/reload' && req.method === 'POST') {
      for (const x of params) x.dirty = false
      ack('params/reload')
      send('params', paramsResponse())
      return json(res, 200, paramsResponse())
    }

    if (path === '/api/params/defaults' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      for (const x of params) {
        if (!body.keys || body.keys.includes(x.key)) {
          x.value = x.default
          x.dirty = false
        }
      }
      ack('params/defaults')
      send('params', paramsResponse())
      return json(res, 200, paramsResponse())
    }

    if (path === '/api/actions' && req.method === 'GET') return json(res, 200, { ok: true, items: actions })

    if (path === '/api/actions/set' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const item = actions.find((a) => a.n === body.n)
      if (!item) return fail(res, 404, `动作 ${body.n} 不存在（1..14）`, 'not_found')
      for (const [k, v] of [['a1', body.a1], ['a2', body.a2]]) {
        if (v !== undefined && (!Number.isFinite(v) || v < 0 || v > 180)) {
          return fail(res, 400, `${item.name} 的 ${k} 必须在 0..180`, 'out_of_range')
        }
      }
      if (body.a1 !== undefined) item.a1 = body.a1
      if (body.a2 !== undefined) item.a2 = body.a2
      item.dirty = item.a1 !== item.preset_a1 || item.a2 !== item.preset_a2
      sim.action = item.n
      servo.ch1 = item.a1
      servo.ch2 = item.a2
      ack('actions/set')
      send('actions', { ok: true, items: actions })
      return json(res, 200, { ok: true, item })
    }

    if (path === '/api/actions/save' && req.method === 'POST') {
      for (const a of actions) {
        a.preset_a1 = a.a1
        a.preset_a2 = a.a2
        a.dirty = false
      }
      sim.nvram = 2
      setTimeout(() => (sim.nvram = 3), 700)
      log('info', '动作表已写进小车 Flash')
      ack('actions/save')
      send('actions', { ok: true, items: actions })
      return json(res, 200, { ok: true })
    }

    if (path === '/api/actions/play' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const item = actions.find((a) => a.n === body.n)
      if (!item) return fail(res, 404, `动作 ${body.n} 不存在`, 'not_found')
      sim.action = item.n
      servo.ch1 = item.a1
      servo.ch2 = item.a2
      log('info', `播放动作 ${item.n} ${item.name}（${item.a1}/${item.a2}）`)
      ack('actions/play')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/servo' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      if (body.ch !== 1 && body.ch !== 2) return fail(res, 400, 'ch 必须是 1 或 2', 'bad_param')
      if (!Number.isFinite(body.angle) || body.angle < 0 || body.angle > 180) {
        return fail(res, 400, '角度必须在 0..180', 'out_of_range')
      }
      if (body.ch === 1) servo.ch1 = body.angle
      else servo.ch2 = body.angle
      ack('servo')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/servo/center' && req.method === 'POST') {
      servo.ch1 = 90
      servo.ch2 = 90
      ack('servo/center')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/servo/deviation' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      if (body.ch !== 1 && body.ch !== 2) return fail(res, 400, 'ch 必须是 1 或 2', 'bad_param')
      if (!Number.isFinite(body.dev_us) || Math.abs(body.dev_us) > 100) {
        return fail(res, 400, '中位偏差必须在 ±100µs', 'out_of_range')
      }
      if (body.ch === 1) servo.dev1 = body.dev_us
      else servo.dev2 = body.dev_us
      ack('servo/deviation')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/drive' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      if (!['off', 'wheels', 'base_turn'].includes(body.mode)) return fail(res, 400, 'mode 必须是 off/wheels/base_turn', 'bad_param')
      // `base` / `vl` / `vr` 是固件速度环的速度刻度 0..7200（不是 mm/s，见 lib/heading.ts 的 PWM_MAX）；
      // `turn` 是目标航向偏移（度），单独按 ±540 夹（HOLD_LIMIT）。
      const lim = (v) => Math.max(-7200, Math.min(7200, Number(v) || 0))
      const limTurn = (v) => Math.max(-540, Math.min(540, Number(v) || 0))
      drive = {
        mode: body.mode,
        vl: lim(body.vl),
        vr: lim(body.vr),
        base: lim(body.base),
        turn: limTurn(body.turn),
      }
      if (body.mode === 'off') log('warn', '驾驶关闭：停车，交还控制权')
      else log('info', `驾驶：${body.mode} ${JSON.stringify({ vl: drive.vl, vr: drive.vr, base: drive.base, turn: drive.turn })}`)
      ack('drive')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/cmd' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      // 命令名与 tools/console/bridge/state.py 的 CMD_NAMES 逐字一致（2026-10-04 修：
      // 这里原来写的是 telem_hz/gyro_cal/gyro_turn/gyro_zero，跟真桥对不上，
      // 于是网页下发的命令在 mock 和真桥上一半能过一半报「没有这个命令」）。
      const allowed = ['stop', 'start_a', 'start_b', 'start_c', 'calib_gyro', 'save_params',
        'clear_fault', 'reset', 'set_telem_hz', 'set_logging', 'set_telem_mode']
      if (!allowed.includes(body.cmd)) return json(res, 200, { ok: false, error: `没有这个命令：${body.cmd}` })
      if (['start_a', 'start_b', 'start_c'].includes(body.cmd)) {
        // 固件删了内置任务流程 ⇒ 这三个命令回 ST_UNSUPPORTED（0x08）＝ 桥侧 ok:false
        log('warn', `命令 ${body.cmd} 已废弃（固件回"不支持"）`)
        return json(res, 200, { ok: false, code: 'unsupported', status: 0x08, error: `${body.cmd} 已废弃：任务改用 #/tasks 页的「开始」` })
      }
      if (body.cmd === 'calib_gyro' || body.cmd === 'set_logging') {
        // 固件 host_link.c:820-825 这两个还没实现，明说"未就绪"，不假装成功
        log('warn', `命令 ${body.cmd} 还没实现（固件回"未就绪"）`)
        return json(res, 200, { ok: false, code: 'not_ready', status: 0x05, error: `${body.cmd} 还没实现（固件回"未就绪"）` })
      }
      if (body.cmd === 'set_telem_hz') {
        // 固件 host_link.c:806-811 钳 1..200（不是 100）
        const hz = Number(body.arg)
        if (!Number.isFinite(hz)) return json(res, 200, { ok: false, error: 'set_telem_hz 要整数频率', code: 'out_of_range' })
        telemHz = Math.max(1, Math.min(200, Math.round(hz)))
        startTimers()
      }
      if (body.cmd === 'stop') {
        drive = { mode: 'off', vl: 0, vr: 0, base: 0, turn: 0 }
      }
      log('info', `命令 ${body.cmd}${body.arg !== undefined ? ' ' + body.arg : ''}`)
      ack('cmd')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/log' && req.method === 'GET') {
      const limit = Math.max(1, Math.min(1000, Number(url.searchParams.get('limit') || 200)))
      return json(res, 200, { ok: true, items: logs.slice(-limit) })
    }

    if (path === '/api/cal') return json(res, 200, cal)

    if (path === '/api/presets' && req.method === 'GET') return json(res, 200, { ok: true, items: presets })

    if (path === '/api/presets' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const name = String(body.name || '').trim()
      if (!name) return fail(res, 400, '预设名不能为空', 'bad_param')
      const keys = Array.isArray(body.targets) ? body.targets : []
      const kind = keys.every((k) => k.startsWith('action.')) ? 'actions' : keys.every((k) => k.startsWith('param.')) ? 'params' : 'mixed'
      const item = { name, kind, updated_ms: Date.now(), keys, note: body.note }
      presets = [...presets.filter((x) => x.name !== name), item]
      log('info', `预设「${name}」已保存（${keys.length} 项）`)
      ack('presets')
      send('state', stateEvent())
      return json(res, 200, { ok: true, item })
    }

    if (path === '/api/presets/apply' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const item = presets.find((x) => x.name === body.name)
      if (!item) return fail(res, 404, `没有预设「${body.name}」`, 'not_found')
      log('info', `套用预设「${item.name}」→ ${(body.targets || []).join(',')}`)
      ack('presets/apply')
      return json(res, 200, { ok: true })
    }

    if (path === '/api/presets' && req.method === 'DELETE') {
      const name = url.searchParams.get('name') || ''
      const before = presets.length
      presets = presets.filter((x) => x.name !== name)
      if (presets.length === before) return fail(res, 404, `没有预设「${name}」`, 'not_found')
      ack('presets/delete')
      send('state', stateEvent())
      return json(res, 200, { ok: true })
    }

    // ---- 任务页（与真桥的 /api/tasks* 同一份契约；真桥见 tools/console/bridge/server.py）----
    if (path === '/api/tasks' && req.method === 'GET') {
      const items = [...tasks.entries()]
        .sort((a, b) => a[0] - b[0])
        .map(([id, t]) => ({ id, name: t.name, len: t.buf.length, crc: crc16(t.buf.subarray(12)), flags: t.flags }))
      return json(res, 200, { ok: true, supported: true, items })
    }

    if (path === '/api/tasks/get' && req.method === 'GET') {
      const id = Number(url.searchParams.get('id') ?? 0)
      const t = tasks.get(id)
      if (!t) return fail(res, 404, `车上没有任务 ${id}`, 'not_found')
      return json(res, 200, { ok: true, hex: t.buf.toString('hex') })
    }

    if (path === '/api/tasks/put' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const id = Number(body.id)
      const off = Number(body.offset)
      const hex = String(body.hex ?? '')
      if (!Number.isInteger(id) || id < 0 || id > 7) return fail(res, 400, 'id 必须是 0..7', 'bad_param')
      if (!Number.isInteger(off) || off < 0) return fail(res, 400, 'offset 必须是非负整数', 'bad_param')
      if (hex.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(hex)) return fail(res, 400, 'hex 必须是偶数长度的十六进制', 'bad_param')
      if (hex.length > 240) return fail(res, 400, `一帧最多 120 字节（现在 ${hex.length / 2}）`, 'out_of_range')
      const data = Buffer.from(hex, 'hex')
      const cur = staging.get(id) ?? Buffer.alloc(0)
      const need = off + data.length
      if (need > 4096) return fail(res, 400, `单个任务最多 4096 字节（解释器 RAM 上限，写到 ${need}）`, 'out_of_range')
      const next = Buffer.alloc(Math.max(cur.length, need), 0xff)
      cur.copy(next)
      data.copy(next, off)
      staging.set(id, next)
      return json(res, 200, { ok: true, written: data.length, offset: off })
    }

    if (path === '/api/tasks/commit' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const id = Number(body.id)
      // 假后端也要装真固件的铁律：解释器在跑时擦一页会把 CPU 按住几十毫秒 ⇒ 回 busy，
      // 让操作员先按停止，而不是"悄悄帮你停掉任务再烧"（那比拒绝更坏）。
      if (taskRun.running) return fail(res, 400, '任务正在跑：先点「停止」再烧写', 'busy')
      const buf = staging.get(id)
      if (!buf) return fail(res, 400, `任务 ${id} 还没收到任何数据`, 'bad_param')
      const len = Number(body.len)
      if (!Number.isInteger(len) || len < 12 || len > buf.length) return fail(res, 400, `len 不合法：${body.len}`, 'bad_param')
      const mine = crc16(buf.subarray(12, len))
      if (mine !== Number(body.crc)) {
        return fail(res, 400, `CRC 不对（车上算 0x${mine.toString(16).toUpperCase()}，上位机给 ${body.crc}）`, 'crc')
      }
      if (buf[2] !== 1) return fail(res, 400, `版本不支持：${buf[2]}`, 'bad_param')
      if (buf[3] === 0 || buf[3] > 16) return fail(res, 400, `状态数越界：${buf[3]}`, 'bad_param')
      const used = [...tasks.entries()].filter(([k]) => k !== id).reduce((s, [, t]) => s + t.buf.length, 0)
      if (used + len > 6144) {
        return fail(res, 400, `程序区（最多 6144 字节）装不下：已用 ${used}，这次 ${len}`, 'out_of_range')
      }
      tasks.set(id, { name: String(body.name ?? ''), buf: Buffer.from(buf.subarray(0, len)),
                      flags: (Number(body.flags) || 0) & 0xff })   // 保留位：原样存（曾经只留 bit0 开机自启）
      staging.delete(id)
      log('info', `任务 ${id} 已写入假 Flash（${len} 字节）`)
      ack('tasks/commit')
      return json(res, 200, { ok: true, id, len })
    }

    if (path === '/api/tasks/delete' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const id = Number(body.id)
      if (taskRun.running) return fail(res, 400, '任务正在跑：先点「停止」再删除', 'busy')
      if (!tasks.has(id)) return fail(res, 404, `车上没有任务 ${id}`, 'not_found')
      tasks.delete(id)
      staging.delete(id)
      return json(res, 200, { ok: true })
    }

    if (path === '/api/tasks/run' && req.method === 'POST') {
      const body = (await readBody(req)) || {}
      const id = Number(body.id)
      const t = tasks.get(id)
      if (!t) return fail(res, 404, `车上没有任务 ${id}`, 'not_found')
      if (taskRun.running) return fail(res, 400, '已经有一个任务在跑：先停止', 'busy')
      if (t.buf.subarray(0, 2).toString('latin1') !== 'TK' && t.buf.readUInt16LE(0) !== 0x4b54) {
        return fail(res, 400, 'magic 不对', 'bad_param')
      }
      const main = t.buf[4]
      const nStates = t.buf[3]
      const stOff = 12 + main * 8
      const nBody = t.buf[stOff + 7] || 1
      taskRun = { id, running: true, state: main, block: 0, nStates, nBody, t0: Date.now(), flags: 0 }
      log('info', `任务 ${id} 开始跑（${nStates} 状态，主页 ${main}）`)
      return json(res, 200, { ok: true, status: taskStatus() })
    }

    if (path === '/api/tasks/stop' && req.method === 'POST') {
      taskRun = { ...taskRun, running: false }
      log('info', '任务已停')
      return json(res, 200, { ok: true, status: taskStatus() })
    }

    if (path === '/api/tasks/status' && req.method === 'GET') {
      return json(res, 200, { ok: true, status: taskStatus() })
    }

    if (path === '/api/stream') {
      res.writeHead(200, {
        'content-type': 'text/event-stream; charset=utf-8',
        'cache-control': 'no-cache, no-transform',
        connection: 'keep-alive',
        'x-accel-buffering': 'no',
      })
      res.write('retry: 1500\n\n')
      clients.add(res)
      res.write(`event: state\ndata: ${JSON.stringify(stateEvent())}\n\n`)
      for (const item of logs.slice(-50)) res.write(`event: log\ndata: ${JSON.stringify(item)}\n\n`)
      const ping = setInterval(() => res.write(': ping\n\n'), 15000)
      req.on('close', () => {
        clearInterval(ping)
        clients.delete(res)
      })
      return
    }

    if (path.startsWith('/api/')) return fail(res, 404, `没有这个端点：${path}`, 'not_found')

    // 静态文件（dist/）
    let file = path === '/' ? '/index.html' : path
    let full = join(DIST, file)
    try {
      const st = await stat(full)
      if (st.isDirectory()) full = join(full, 'index.html')
    } catch {
      full = join(DIST, 'index.html')
    }
    try {
      const data = await readFile(full)
      res.writeHead(200, { 'content-type': MIME[extname(full)] || 'application/octet-stream', 'cache-control': 'no-store' })
      res.end(data)
    } catch {
      res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' })
      res.end('dist/ 还没有构建：先 npm run build（或在 tools/console/web 里 npm run dev）')
    }
    return
  } catch (e) {
    return fail(res, 500, `假后端内部错误：${e?.message || e}`, 'internal')
  }
})

startTimers()
server.listen(PORT, HOST, () => {
  console.log(`[mock] 假后端已启动：http://${HOST}:${PORT}`)
  console.log(`[mock] 静态目录：${DIST}`)
  console.log('[mock] 端点：/api/health /link /ports /params* /actions* /servo* /drive /cmd /log /cal /presets* /tasks* /stream')
})
