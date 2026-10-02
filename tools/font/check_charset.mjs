#!/usr/bin/env node
/**
 * KK_OLED (u8g2 格式) 静态字模「查字」工具。
 *
 *   node tools/font/check_charset.mjs <font.c> [字符串或文本文件 ...]
 *
 * 把 C 字面量的八进制/十六进制转义解成字节，解析 u8g2 字体头，然后按
 * firmware/Middlewares/kk_oled/graphics/kk_oled_font.c 的 oled_find_glyph()
 * 逐字查表。KK_OLED 遇到子集外的字**静默不画**（不报错、不画方框），所以
 * 改 UI 文案之前必须用这个脚本对一遍。
 */
import { readFileSync, existsSync } from "node:fs";

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
        } else {
          out.push(simple[src[i]] ?? src.charCodeAt(i));
          i++;
        }
      } else {
        out.push(src.charCodeAt(i++) & 0xff);
      }
    }
    i++; // 跳过收尾引号
  }
  return Uint8Array.from(out);
}

function makeFinder(buf) {
  const be16 = (o) => (buf[o] << 8) | buf[o + 1];
  const headerSize = 23;
  const startUpper = be16(17), startLower = be16(19), startUnicode = be16(21);
  const glyphCount = be16(0);
  return {
    glyphCount,
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

const [fontPath, ...texts] = process.argv.slice(2);
if (!fontPath) { console.error("用法: node tools/font/check_charset.mjs <font.c> [字符串|文本文件 ...]"); process.exit(2); }
const bytes = parseBytes(readFileSync(fontPath, "utf8"));
const finder = makeFinder(bytes);
/* u8g2 头的前两字节不是字形数：真正可查的字形数在字节 [0]（本字体 187 = 0xBB）。 */
console.log(fontPath + "  字节数=" + bytes.length + "  字形数=" + bytes[0]);

const inputs = texts.length ? texts : [];
let text = "";
for (const t of inputs) text += existsSync(t) ? readFileSync(t, "utf8") : t;
const missing = [];
for (const ch of text) {
  const cp = ch.codePointAt(0);
  if (cp === 10 || cp === 13 || cp === 32) continue;
  if (!finder.find(cp)) missing.push(ch);
}
const uniqMissing = [...new Set(missing)];
console.log("检查字符数=" + [...text].filter(c => c !== "\n" && c !== "\r").length + "  缺字=" + uniqMissing.length +
  (uniqMissing.length ? "  -> " + uniqMissing.map(c => c + " U+" + c.codePointAt(0).toString(16).toUpperCase()).join(" ") : "  (全部命中)"));
