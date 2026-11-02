#!/usr/bin/env node
/**
 * KK_OLED (u8g2 格式) 字模「量字宽」工具 —— 和固件同一个算法，用来排版。
 *
 *   node tools/font/measure_text.mjs <font.c> "字符串" ...
 *
 * 为什么需要它：`draw_motor_page()` 那类"一行里图标/文字/数字/滑条横排"的版式，
 * 数字宽度决定会不会压到左边的标签（例如 `-100` 比 `100` 宽一个负号）。
 * 本工具复刻 `firmware/Middlewares/kk_oled/graphics/kk_oled_font.c` 的
 * `oled_find_glyph()` + `oled_decode_glyph_info()` + `OLED_GetUTF8Width()`：
 * 宽度 = 逐字 advance 累加，但**末字形不算右侧 advance 空白**，改用
 * `width + x_offset`（和固件逐字节一致）。
 */
import { readFileSync } from "node:fs";

/* ---- 与 check_charset.mjs 同一套 C 字面量解析 ---- */
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

const HEADER = 23;
const find = (font, cp) => {
  const be16 = (o) => (font[o] << 8) | font[o + 1];
  let cursor = HEADER;
  if (cp <= 0xff) {
    if (cp >= 0x61) cursor += be16(19);
    else if (cp >= 0x41) cursor += be16(17);
    while (cursor + 1 < font.length && font[cursor + 1] !== 0) {
      if (font[cursor] === cp) return cursor + 2;
      cursor += font[cursor + 1];
    }
    return -1;
  }
  cursor += be16(21);
  let lookup = cursor, indexed;
  do {
    cursor += be16(lookup);
    indexed = be16(lookup + 2);
    lookup += 4;
  } while (indexed < cp);
  for (;;) {
    if (cursor + 1 >= font.length) return -1;
    const encoded = be16(cursor);
    if (encoded === 0) return -1;
    if (encoded === cp) return cursor + 3;
    cursor += font[cursor + 2];
  }
};

/* ---- 位流（LSB-first），与 oled_read_bits() 一致 ---- */
function readBits(font, st, count) {
  if (count === 0) return 0;
  let value = font[st.ptr] >> st.bit;
  const total = st.bit + count;
  if (total > 8) value |= font[st.ptr + 1] << (8 - st.bit);
  st.ptr += total >> 3;
  st.bit = total & 7;
  if (count === 8) return value & 0xff;
  return value & ((1 << count) - 1);
}
function readSigned(font, st, count) {
  if (count === 0) return 0;
  return readBits(font, st, count) - (1 << (count - 1));
}

function glyphInfo(font, cp) {
  const at = find(font, cp);
  const space = cp === 0x20 ? -1 : find(font, 0x20);   /* 找不到就回退空格 */
  const off = at >= 0 ? at : space;
  if (off < 0) return null;
  const st = { ptr: off, bit: 0 };
  const width = readBits(font, st, font[4]);
  const height = readBits(font, st, font[5]);
  const xOffset = readSigned(font, st, font[6]);
  readSigned(font, st, font[7]);                       /* y_offset，宽度用不到 */
  const advance = readSigned(font, st, font[8]);
  return { width, height, xOffset, advance, fallback: at < 0 };
}

function textWidth(font, text) {
  let width = 0, lastAdvance = 0, lastVisible = 0;
  for (const ch of text) {
    const g = glyphInfo(font, ch.codePointAt(0));
    if (g) {
      lastAdvance = g.advance;
      lastVisible = g.width + g.xOffset;
      width += g.advance;
    } else {
      lastAdvance = 0;
      lastVisible = 0;
    }
  }
  width -= lastAdvance;
  width += lastVisible;
  return width;
}

const [fontPath, ...texts] = process.argv.slice(2);
if (!fontPath || !texts.length) {
  console.error('用法: node tools/font/measure_text.mjs <font.c> "字符串" ...');
  process.exit(2);
}
const font = parseBytes(readFileSync(fontPath, "utf8"));
console.log(`${fontPath}  max_width=${font[9]} max_height=${font[10]}`);
for (const t of texts) {
  const parts = [...t].map((ch) => {
    const g = glyphInfo(font, ch.codePointAt(0));
    return `${ch}=${g ? `${g.width}+${g.xOffset}/${g.advance}${g.fallback ? "(空格回退)" : ""}` : "缺"}`;
  });
  console.log(`"${t}" -> 宽 ${textWidth(font, t)} px   [${parts.join("  ")}]`);
}
