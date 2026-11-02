#!/usr/bin/env node
/**
 * 循迹位置算法离线验证台（不碰板子）：
 *   node tools/pos_sim.mjs [黑带宽度=3.8] [噪声计数=2]
 *
 * 用"黑带盖住每一路的面积"当传感器模型（8 路，路心 1..8，每路灵敏区 1 格宽），
 * 复现 firmware/App/Svc/PID_tracing.c 里 read_tracing_data_pid() 的两件事：
 *   ① 加权重心（现状）
 *   ② 边缘外推：cover = Σ 归一化黑度；最外一路 ≥0.9×最黑 时
 *        左切：pos = 0.5 + cover - 黑带宽度/2   （且必须 < 重心）
 *        右切：pos = 8.5 - cover + 黑带宽度/2   （且必须 > 重心）
 * 输出：不同线心真值下的"重心误差 / 外推后误差"，以及外推在哪些区间生效。
 * 再加一遍噪声，看 3 帧中值能把"帧间跳变"压掉多少。
 */
const W = Number(process.argv[2] ?? 3.8);   // 黑带宽度（格）
const NOISE = Number(process.argv[3] ?? 2); // 每路 ±噪声计数（0..255 刻度）

const CH = 8;
function coverage(c, w) {                    // 每路覆盖率 0..1
  const out = [];
  for (let i = 0; i < CH; i++) {
    const lo = i + 0.5, hi = i + 1.5;
    const bl = c - w / 2, br = c + w / 2;
    out.push(Math.max(0, Math.min(hi, br) - Math.max(lo, bl)));
  }
  return out;
}
function normalize(cov, noise = 0) {
  const lvl = cov.map((v) => Math.max(0, Math.min(255, v * 255 + noise * (Math.random() * 2 - 1))));
  const lmax = Math.max(...lvl);
  return lvl.map((v) => (lmax > 0 ? v / lmax : 0));
}
function centroid(w) {
  let n = 0, s = 0;
  for (let i = 0; i < CH; i++) { n += w[i]; s += w[i] * (i + 1); }
  return n > 0 ? s / n : NaN;
}
function pos2(w, barW) {                     // 重心 + 边缘外推（与固件同一套判据）
  const p0 = centroid(w);
  const wmax = Math.max(...w);
  let cover = 0;
  for (let i = 0; i < CH; i++) cover += w[i];
  let p = p0, fired = 0;
  if (wmax > 0) {
    if (w[0] >= 0.9 * wmax) { const q = 0.5 + cover - barW / 2; if (q < p) { p = q; fired = 1; } }
    else if (w[7] >= 0.9 * wmax) { const q = 8.5 - cover + barW / 2; if (q > p) { p = q; fired = 1; } }
  }
  return { p, p0, fired };
}

console.log(`黑带宽度 = ${W} 格，噪声 = ±${NOISE} 计数`);
console.log("线心真值 | 重心   误差  | 外推后 误差  | 外推");
console.log("---------+--------------+--------------+-----");
let e0 = 0, e1 = 0, n = 0, fireLo = null, fireHi = null, worst = 0, worstAt = 0;
for (let c = -1.5; c <= 10.5 + 1e-9; c += 0.05) {
  const w = normalize(coverage(c, W));
  const { p, p0, fired } = pos2(w, W);
  const d0 = p0 - c, d1 = p - c;
  e0 += Math.abs(d0); e1 += Math.abs(d1); n++;
  if (Math.abs(d1) > worst) { worst = Math.abs(d1); worstAt = c; }
  if (fired && fireLo === null) fireLo = c;
  if (fired) fireHi = c;
  if (Math.abs(c - Math.round(c * 2) / 2) < 1e-9 && c >= 0 && c <= 9.5)
    console.log(`${c.toFixed(2).padStart(8)} | ${p0.toFixed(3).padStart(6)} ${d0 >= 0 ? "+" : ""}${d0.toFixed(3)} | ${p.toFixed(3).padStart(6)} ${d1 >= 0 ? "+" : ""}${d1.toFixed(3)} | ${fired ? "是" : " "}`);
}
console.log("---------+--------------+--------------+-----");
console.log(`平均误差：重心 ${(e0 / n).toFixed(4)} 格 -> 外推后 ${(e1 / n).toFixed(4)} 格`);
console.log(`最大误差：外推后 ${worst.toFixed(4)} 格 (线心 ${worstAt.toFixed(2)})；外推生效区间 ${fireLo === null ? "无" : fireLo.toFixed(2) + " .. " + fireHi.toFixed(2)}`);

/* 噪声下的帧间跳变：raw 重心 vs 归一化 + 3 帧中值（只看线在阵内 1.0..8.0） */
function run(filt) {
  const hist = [];
  let prev = null, sum = 0, k = 0;
  for (let t = 0; t < 20000; t++) {
    const c = 1.0 + 7.0 * (0.5 + 0.5 * Math.sin(t / 600));   // 慢慢扫过整条阵
    const { p } = pos2(normalize(coverage(c, W), NOISE), W);
    let out = p;
    if (filt) {
      hist.push(p);
      if (hist.length > 3) hist.shift();
      if (hist.length === 3) out = [...hist].sort((a, b) => a - b)[1];
    }
    if (prev !== null) { sum += Math.abs(out - prev); k++; }
    prev = out;
  }
  return sum / k;
}
const jr = run(false), jf = run(true);
console.log(`\n噪声下帧间平均跳变（1.0..8.0 扫线 2 万帧）：raw ${jr.toFixed(4)} 格 -> 3 帧中值 ${jf.toFixed(4)} 格（${(100 - (jf / jr) * 100).toFixed(1)}% 压掉）`);
console.log(`折算成 aim：raw ${(jr * 10).toFixed(2)}°/帧 -> 中值 ${(jf * 10).toFixed(2)}°/帧`);
