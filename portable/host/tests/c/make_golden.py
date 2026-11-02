"""重新生成「任务程序」金样本（C → fixtures/task.golden.json → web/src/lib/task.golden.ts）。

三步一体，缺一步就会漂：
  1. `tools/console/tests/c/taskgen.c` 链真 `firmware/App/Svc/task_prog.c`，把写死的那份
     三状态任务编成镜像（**真固件的编码结果**）；
  2. 这里把它写成 `tools/console/tests/fixtures/task.golden.json`（含场景 + 字节 + CRC）；
  3. 再生成 `tools/console/web/src/lib/task.golden.ts`，给 vitest 用。

用法（仓库根）：`pwsh -File tools/console/tests/c/make_golden.ps1`
改格式时：先改 task_prog.h/.c + taskgen.c 的场景，再跑上面这条，然后跑
`pytest tools/console/tests/test_task_bytes.py` 与 `cd tools/console/web && npm test`。
"""
from __future__ import annotations

import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent.parent
HEX_PER_LINE = 64


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def gcc() -> str:
    return os.environ.get("TASK_GCC") or os.environ.get("PIPE_GCC") or "C:\\mingw64\\bin\\gcc.exe"


def build_image() -> bytes:
    out = pathlib.Path(tempfile.mkdtemp(prefix="taskgen")) / "taskgen.exe"
    cmd = [gcc(), "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-I" + str(REPO / "firmware/App/Svc"),
           str(HERE / "taskgen.c"), str(REPO / "firmware/App/Svc/task_prog.c"),
           "-o", str(out)]
    cp = subprocess.run(cmd, capture_output=True, timeout=600)
    if cp.returncode != 0:
        sys.stderr.write(cp.stdout.decode("utf-8", "replace") + cp.stderr.decode("utf-8", "replace"))
        raise SystemExit("taskgen.c 编不过")
    blob = out.with_suffix(".bin")
    subprocess.run([str(out), str(blob)], check=True, timeout=120)
    return blob.read_bytes()


def decode(img: bytes) -> dict:
    """独立第三方解码（python 版）—— 用来确认 C 侧编的确实是 fixture 里那个场景。"""
    magic = struct.unpack_from("<H", img, 0)[0]
    ver = img[2]
    n_states = img[3]
    main = img[4]
    assert magic == 0x4B54 and ver == 1, (hex(magic), ver)
    states = []
    for i in range(n_states):
        so = 12 + i * 8
        name_off, enter_off, body_off = struct.unpack_from("<HHH", img, so)
        n_enter, n_body = img[so + 6], img[so + 7]

        def seq(off: int, cnt: int) -> list:
            out = []
            for k in range(cnt):
                at = off + k * 10
                # 块 = op(u8) flags(u8) p0(u16) p1(i16) p2(u16) p3(u16)
                op, flags, p0, p1, p2, p3 = struct.unpack_from("<BBHhHH", img, at)
                out.append({"op": op, "flags": flags, "p0": p0, "p1": p1, "p2": p2, "p3": p3})
            return out

        name = ""
        if name_off:
            ends = [struct.unpack_from("<H", img, 12 + j * 8)[0] for j in range(n_states)]
            end = min([e for e in ends if e > name_off] or [len(img)])
            name = img[name_off:end].decode("utf-8")
        states.append({"name": name, "enter": seq(enter_off, n_enter) if n_enter else [],
                       "body": seq(body_off, n_body) if n_body else []})
    return {"main": main, "states": states}


OP_CODE = {
    "SET_SPEED": 0x01, "SET_MODE": 0x02, "TURN_DEG": 0x03, "ACTION": 0x04, "WAIT_MS": 0x05,
    "WAIT_COND": 0x06, "GOTO": 0x07, "END": 0x08, "SET_SERVO": 0x09, "CROSS_CLEAR": 0x0A,
}


def scene_from_fixture(scen: dict) -> dict:
    """fixture 里的场景（op 写名字、省字段）→ 与 `decode()` 同形的字典，用于比对。"""
    def b(x: dict) -> dict:
        return {"op": OP_CODE[x["op"]], "flags": x.get("flags", 0), "p0": x.get("p0", 0),
                "p1": x.get("p1", 0), "p2": x.get("p2", 0), "p3": x.get("p3", 0)}
    return {"main": scen["main"],
            "states": [{"name": s["name"], "enter": [b(x) for x in s["enter"]],
                        "body": [b(x) for x in s["body"]]} for s in scen["states"]]}


def write_ts(fx: dict, path: pathlib.Path) -> None:
    scen = fx["scenario"]
    L = [
        "/**",
        " * @file  task.golden.ts",
        " * @brief 「任务程序」金样本 —— **由真固件源码生成**，不是手抄的",
        " *",
        " * 生成链：`tools/console/tests/c/taskgen.c`（链真 `firmware/App/Svc/task_prog.c`）",
        " *         → `tools/console/tests/fixtures/task.golden.json`（pytest 每次重编 C 对拍它）",
        " *         → 本文件（`task.golden.test.ts` 拿它钉住 `task.ts` 的编码器）",
        " *",
        " * 于是 TS 的 `encodeTask()` 与 C 的格式层被**同一个字节串**拴在一起：",
        " * 谁偷偷改了布局，`test_task_bytes.py` 或 `task.golden.test.ts` 至少红一个。",
        " * 重新生成：`pwsh -File tools/console/tests/c/make_golden.ps1`（**不要手改本文件**）",
        " */",
        "",
        "import { NO_STATE } from './task';",
        "",
        "export interface GoldenBlock {",
        "  op: string;",
        "  flags: number;",
        "  p0: number;",
        "  p1: number;",
        "  p2: number;",
        "  p3: number;",
        "}",
        "",
        "export interface GoldenState {",
        "  name: string;",
        "  enter: GoldenBlock[];",
        "  body: GoldenBlock[];",
        "}",
        "",
        f"export const GOLDEN_LEN = {fx['len']};",
        f"export const GOLDEN_CRC16 = 0x{fx['crc16']:04x};",
        f"export const GOLDEN_MAIN = {scen['main']};",
        "export const GOLDEN_HEX =",
    ]
    hexs = fx["hex"]
    for i in range(0, len(hexs), HEX_PER_LINE):
        line = f"  '{hexs[i:i + HEX_PER_LINE]}'"
        L.append(line + (" +" if i + HEX_PER_LINE < len(hexs) else ";"))
    L += ["", "export const GOLDEN_STATES: GoldenState[] = ["]
    for s in scen["states"]:
        L.append("  {")
        L.append(f"    name: {json.dumps(s['name'], ensure_ascii=False)},")
        for which in ("enter", "body"):
            L.append(f"    {which}: [")
            for x in s[which]:
                p3 = "NO_STATE" if x.get("p3", 0) == 0xFFFF else str(x.get("p3", 0))
                L.append("      { op: '%s', flags: %d, p0: %d, p1: %d, p2: %d, p3: %s },"
                         % (x["op"], x.get("flags", 0), x.get("p0", 0), x.get("p1", 0), x.get("p2", 0), p3))
            L.append("    ],")
        L.append("  },")
    L += ["];", "", "/** 金样本场景的状态名（与 C 侧 g_name[] 一字不差） */",
          "export const GOLDEN_NAMES: string[] = [%s];"
          % ", ".join(json.dumps(s["name"], ensure_ascii=False) for s in scen["states"]), ""]
    path.write_text("\n".join(L), encoding="utf-8", newline="\n")


def main() -> int:
    img = build_image()
    dec = decode(img)
    names = [s["name"] for s in dec["states"]]
    fx_path = HERE.parent / "fixtures" / "task.golden.json"
    old = json.loads(fx_path.read_text(encoding="utf-8")) if fx_path.exists() else None
    scen = (old or {}).get("scenario")
    if scen is not None and scene_from_fixture(scen) != dec:
        sys.stderr.write("!! C 侧(taskgen.c)编出来的场景与 fixture 里的 scenario 不一致：\n"
                         + json.dumps(dec, ensure_ascii=False, indent=1) + "\n")
        return 2
    if scen is None:
        sys.stderr.write("!! fixture 里没有 scenario，无法自动比对 C 侧场景：\n"
                         + json.dumps(dec, ensure_ascii=False) + "\n")
        return 3
    crc = crc16_ccitt(img[12:])
    assert struct.unpack_from("<H", img, 6)[0] == crc, "C 侧 CRC 与 python 算的不一样"
    fx = {"note": "C(taskgen.c) ↔ TS(task.ts) 共用金样本：scenario 编出来必须正好是 hex（本文件由 make_golden.py 生成，别手改）",
          "len": len(img), "crc16": crc, "hex": img.hex().upper(), "scenario": scen}
    fx_path.parent.mkdir(parents=True, exist_ok=True)
    fx_path.write_text(json.dumps(fx, ensure_ascii=False, indent=1) + "\n", encoding="utf-8", newline="\n")
    ts = REPO / "tools/console/web/src/lib/task.golden.ts"
    write_ts(fx, ts)
    print(f"len={len(img)} crc16=0x{crc:04X} 名字={names}")
    print(f"wrote {fx_path.relative_to(REPO)}")
    print(f"wrote {ts.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
