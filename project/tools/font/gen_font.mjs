#!/usr/bin/env node
/**
 * KK_OLED (u8g2 格式) 静态字模「重生成」工具。
 *
 *   node tools/font/gen_font.mjs <font.c> <字体名> <字号> <数组名> [必须含的字符串 ...]
 *
 * 做法：把 firmware/**\/*.{c,h} 里出现的所有字符（严格 UTF-8 解码，失败
 * 才按 GBK）当候选；只保留**旧字模里已经有**的那些（避免每扫一次字模就
 * 漂移），再并上命令行里显式给的"必须含"字符，POST 给 ledfont 服务重生成，
 * 写回 <font.c>（旧文件备份到 <font.c>.bak）。
 *
 * 背景：KK_OLED 遇到子集外的字**静默不画**，所以改 UI 文案（尤其汉字）
 * 之后必须重生成字模，再用 tools/font/check_charset.mjs 复核缺字=0。
 * API 文档：https://ledfont.botelvdong.com/agent-api.md
 */
import { readFileSync, writeFileSync, readdirSync, statSync } from "node:fs";
import { join, extname } from "node:path";

const API = "https://ledfont.botelvdong.com/api/v1/compressed-fonts/generate";
const ROOT = "firmware";

function parseBytes(src) {
  const m = src.match(/const\s+uint8_t\s+\w+\s*\[[^\]]*\]\s*=/);
  if (!m) throw new Error("找不到 const uint8_t <name>[...] = 的数组定义");
  let i = m.index + m[0].length;
  const out = [];
  const simple = { n: 10, r: 13, t: 9, v: 11, b: 8, f: 12, a: 7, "\\": 92, '"': 34, "'": 39, "?": 63 };
  while (i < src.length) {
    const ch = src[i];
    if (ch === ";") break;
    if (ch !== '"') { i++; continue; }
    i++;
    while (i < src.length && src[i] !== '"') {
      if (src[i] === "\\") {
        i++;
        if (src[i] === "x") {
          i++;
          let hex = "";
          while (i < src.length && /[0-9a-fA-F]/.test(src[i])) hex += src[i++];
          out.push(parseInt(hex, 16) & 0xff);
        } else if (/[0-7]/.test(src[i])) {
          let oct = "";
          while (i < src.length && /[0-7]/.test(src[i]) && oct.length < 3) oct += src[i++];
          out.push(parseInt(oct, 8) & 0xff);
        } else { out.push(simple[src[i]] ?? src.charCodeAt(i)); i++; }
      } else { out.push(src.charCodeAt(i++) & 0xff); }
    }
    i++;
  }
  return Uint8Array.from(out);
}

function makeFinder(buf) {
  const be16 = (o) => (buf[o] << 8) | buf[o + 1];
  const headerSize = 23;
  const startUpper = be16(17), startLower = be16(19), startUnicode = be16(21);
  return {
    find(codepoint) {
      let cursor = headerSize;
      if (codepoint <= 0xff) {
        if (codepoint >= 0x61) cursor += startLower;
        else if (codepoint >= 0x41) cursor += startUpper;
        while (cursor + 1 < buf.length && buf[cursor + 1] !== 0) {
          if (buf[cursor] === codepoint) return true;
          cursor += buf[cursor + 1];
        }
        return false;
      }
      cursor += startUnicode;
      let lookup = cursor, indexed;
      do {
        const before = cursor;
        cursor += be16(lookup);
        indexed = be16(lookup + 2);
        lookup += 4;
        if (cursor <= before) throw new Error("Unicode 跳转表损坏");
      } while (indexed < codepoint);
      for (;;) {
        if (cursor + 1 >= buf.length) return false;
        const encoded = be16(cursor);
        if (encoded === 0) return false;
        if (encoded === codepoint) return true;
        cursor += buf[cursor + 2];
      }
    },
  };
}

function walk(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if ([".c", ".h"].includes(extname(p))) out.push(p);
  }
  return out;
}

const [fontPath, fontName, sizeArg, arrayName, ...mustHave] = process.argv.slice(2);
if (!fontPath || !fontName || !sizeArg || !arrayName) {
  console.error("用法: node tools/font/gen_font.mjs <font.c> <字体名> <字号> <数组名> [必须含的字符串 ...]");
  process.exit(2);
}

const oldSrc = readFileSync(fontPath, "utf8");
const finder = makeFinder(parseBytes(oldSrc));

/* 候选字符：源码里出现的所有非空白字符 + 可打印 ASCII */
const chars = new Set();
const utf8 = new TextDecoder("utf-8", { fatal: true });
const gbk = new TextDecoder("gbk");
for (const file of walk(ROOT, [])) {
  const buf = readFileSync(file);
  let text;
  try { text = utf8.decode(buf); } catch { text = gbk.decode(buf); }
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    if (cp > 0x20 && cp !== 0x7f) chars.add(ch);
  }
}
for (let c = 0x20; c <= 0x7e; c++) chars.add(String.fromCharCode(c));

const kept = [];
for (const ch of chars) {
  if (finder.find(ch.codePointAt(0))) kept.push(ch);
}
for (const s of mustHave) for (const ch of s) if (!kept.includes(ch)) kept.push(ch);
kept.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));

console.log("源码候选=" + chars.size + "  旧字模已有=" + (kept.length - mustHave.join("").length) +
            "  实际提交=" + kept.length);

const res = await fetch(API, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ font: fontName, size: Number(sizeArg), text: kept.join(""), array_name: arrayName }),
});
const json = await res.json();
if (json.code !== 200 || !json.data || !json.data.c_source) {
  console.error("生成失败: " + JSON.stringify(json).slice(0, 400));
  process.exit(1);
}
writeFileSync(fontPath + ".bak", oldSrc);
/* API 返回的 c_source 只有数组本体，不含项目自己的 #include —— 直接覆盖会把
 * 旧文件头部的 `#include "kk_font.h"` 弄丢（uint8_t 就编不过），所以先捡回来。 */
const preamble = oldSrc.split("\n").filter((l) => /^\s*#include/.test(l)).join("\n");
let newSrc = json.data.c_source;
if (preamble && !/^\s*#include/m.test(newSrc)) newSrc = preamble + "\n\n" + newSrc;
writeFileSync(fontPath, newSrc);
console.log("OK -> " + fontPath + "  byte_count=" + json.data.byte_count +
            "  glyph_count=" + (json.data.glyph_count ?? "?") + "  (旧文件备份 " + fontPath + ".bak)");
