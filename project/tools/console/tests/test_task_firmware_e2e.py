"""上位机拼出来的任务 → **真固件校验器**读不读？

`test_task_bytes.py` 证明的是"C 编的 = TS 编的 = 金样本"。这个文件补的是**另一半**：
上位机编辑器里那个"新建任务"的默认程序、以及各种边界程序（16 状态、64 块、负角度、
按 UTF-8 字节截断的中文名、故意缺收尾块），**能不能过车上那句 `TaskProg_Validate()`**。

链路（全部用真源码，不复制一份布局）：

  1. esbuild 把 `tools/console/tests/c/taskentry.ts` 打成 `.mjs`（它 re-export 的正是
     `web/src/lib/task.ts` + `taskedit.ts` 的编码器）；
  2. node 跑 `c/emit_tasks.mjs` 编出 7 个镜像 + `manifest.json`（上位机自己算的 len/crc16）；
  3. gcc 把 `c/taskcheck.c` 链上**真** `firmware/App/Svc/task_prog.c`；
  4. 逐个镜像比：车上校验码必须是 `0 ok`、车上读出来的 len/CRC/状态名/块字段必须与
     manifest 完全一致；"缺收尾块"那个必须**两边都报错**（上位机 `validateTask` 报
     "最后一块"，车上回 `TASK_E_TAIL(9)`）。

没有 gcc / node / esbuild 时整组 skip。
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
WEB = os.path.join(REPO, "tools", "console", "web")
GCC = (os.environ.get("TASK_GCC") or os.environ.get("PIPE_GCC") or shutil.which("gcc")
       or r"C:\mingw64\bin\gcc.exe")
NODE = shutil.which("node") or r"C:\Program Files\nodejs\node.exe"


def _run(cmd, **kw):
    """中文 Windows 上 gcc/node 按 GBK 吐诊断 ⇒ 一律收 bytes 再容错解码。"""
    cp = subprocess.run(cmd, capture_output=True, timeout=900, **kw)
    out = cp.stdout.decode("utf-8", "replace")
    err = cp.stderr.decode("utf-8", "replace")
    return cp.returncode, out, err


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    """返回 (taskcheck.exe, 输出目录, manifest, taskcheck 输出) —— 整套只跑一次。"""
    if not (os.path.isfile(GCC) or shutil.which(GCC)):
        pytest.skip(f"没有 gcc，跳过（TASK_GCC 可指定）：{GCC}")
    if not (os.path.isfile(NODE) or shutil.which(NODE)):
        pytest.skip(f"没有 node，跳过：{NODE}")

    tmp = tmp_path_factory.mktemp("task_e2e")
    work = str(tmp)

    # 1) 打包上位机编码器（esbuild 随 vite 一起来，走 npx 免得依赖 PATH）
    bundle = os.path.join(work, "taskentry.mjs")
    rc, out, err = _run(
        f'npx --no-install esbuild "{os.path.join(HERE, "c", "taskentry.ts")}" '
        f'--bundle --format=esm --platform=node --log-level=warning --outfile="{bundle}"',
        cwd=WEB, shell=True)
    assert rc == 0, f"esbuild 打包失败：\n{out}\n{err}"

    # 2) 编镜像
    rc, out, err = _run([NODE, os.path.join(HERE, "c", "emit_tasks.mjs"), bundle, work])
    assert rc == 0, f"emit_tasks 失败：\n{out}\n{err}"

    with open(os.path.join(work, "manifest.json"), "r", encoding="utf-8") as fh:
        manifest = json.load(fh)
    with open(os.path.join(work, "roundtrip.json"), "r", encoding="utf-8") as fh:
        roundtrip = json.load(fh)

    # 3) 车上那半：真 task_prog.c
    exe = os.path.join(work, "taskcheck.exe" if os.name == "nt" else "taskcheck")
    rc, out, err = _run(
        [GCC, "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-I" + os.path.join(REPO, "firmware", "App", "Svc"),
         os.path.join(HERE, "c", "taskcheck.c"),
         os.path.join(REPO, "firmware", "App", "Svc", "task_prog.c"),
         "-o", exe])
    assert rc == 0, f"taskcheck 编译失败：\n{out}\n{err}"

    # 4) 跑每个镜像
    readings = {}
    for item in manifest:
        rc, out, err = _run([exe, os.path.join(work, item["file"])])
        assert rc == 0, f"taskcheck 跑 {item['file']} 失败：\n{out}\n{err}"
        readings[item["name"]] = out

    return {"dir": work, "manifest": manifest, "roundtrip": roundtrip, "readings": readings, "exe": exe}


def _parse(text: str) -> dict:
    """把 taskcheck 的输出变成好断言的结构。

    **必须先归一化换行**：taskcheck 是 C 程序，Windows 上 printf("\n") 出来的其实是 CRLF，
    而下面每条正则都以 `$` 收尾 —— `$` 只吃 `\n`，留着 `\r` 会让**每一条都默默不匹配**
    （这种"全都解析不出来"的失败会伪装成"测试全绿"，比报错更难查）。
    """
    text = text.replace("\r\n", "\n")
    m = re.search(r"^VALIDATE (\d+) (.+)$", text, re.M)
    assert m, f"没有 VALIDATE 行：\n{text}"
    code = int(m.group(1))
    hdr = re.search(r"^HDR n_states=(\d+) main=(\d+) len=(\d+) crc=0x([0-9A-F]{4})$", text, re.M)
    states = [
        {"i": int(a), "name": b, "n_enter": int(c), "n_body": int(d)}
        for a, b, c, d in re.findall(r'^STATE (\d+) name="(.*)" n_enter=(\d+) n_body=(\d+)$', text, re.M)
    ]
    blocks = [
        {"state": int(a), "seq": b, "j": int(c), "op": int(d, 16), "flags": int(e),
         "p0": int(f), "p1": int(g), "p2": int(h), "p3": int(i2)}
        for a, b, c, d, e, f, g, h, i2 in re.findall(
            r"^BLOCK (\d+) (\w+) (\d+) op=0x([0-9A-F]{2}) flags=(\d+) p0=(\d+) p1=(-?\d+) p2=(\d+) p3=(\d+)$",
            text, re.M)
    ]
    return {
        "code": code,
        "err": m.group(2),
        "hdr": None if not hdr else {
            "n_states": int(hdr.group(1)), "main": int(hdr.group(2)),
            "len": int(hdr.group(3)), "crc": int(hdr.group(4), 16),
        },
        "states": states,
        "blocks": blocks,
    }


def test_每个镜像都被车上校验器接受(harness):
    """除"故意缺收尾块"那个之外，全部必须 `VALIDATE 0 ok`。"""
    for item in harness["manifest"]:
        r = _parse(harness["readings"][item["name"]])
        if item["name"] == "no_tail":
            assert r["code"] == 9, f"缺收尾块应该是 TASK_E_TAIL(9)，实际 {r}"
            continue
        assert r["code"] == 0, f"{item['name']} 车上校验没通过：{r['code']} {r['err']}\n{harness['readings'][item['name']]}"


def test_长度与CRC以车上算的为准(harness):
    """manifest 里的 len/crc16 是上位机算的；车上按自己的 CRC 覆盖范围再算一遍，必须一样
    —— 这条能抓住"C 的 CRC 覆盖 [6,len) 而 TS 覆盖 [12,len)"这类偏移错。"""
    checked = 0
    for item in harness["manifest"]:
        if item["name"] == "no_tail":
            continue
        r = _parse(harness["readings"][item["name"]])
        assert r["hdr"] is not None, f"{item['name']} 车上没给出头（校验码 {r['code']} {r['err']}）"
        assert r["hdr"]["len"] == item["len"], f"{item['name']} 长度不一致"
        assert r["hdr"]["crc"] == item["crc16"], f"{item['name']} CRC 不一致"
        assert r["hdr"]["n_states"] == item["nStates"], f"{item['name']} 状态数不一致"
        checked += 1
    assert checked == len(harness["manifest"]) - 1 and checked >= 5, f"只验了 {checked} 个镜像"


def test_状态名按字节存活(harness):
    """中文名（含按 32 字节截断的那个）经 C 读回来必须还是同一串 —— UTF-8 不能被切坏。"""
    checked = 0
    for item in harness["manifest"]:
        if item["name"] == "no_tail":
            continue
        r = _parse(harness["readings"][item["name"]])
        assert r["states"], f"{item['name']} 一个状态都没读出来"
        assert [s["name"] for s in r["states"]] == item["names"], f"{item['name']} 名字不一致"
        checked += 1
    assert checked >= 5, f"只验了 {checked} 个镜像"


def test_负角度与超时字段的极性(harness):
    """三状态那个镜像里：TURN_DEG 的 p1 = −90（i16）、WAIT_COND 的 p3 = 1、
    p2 超时 3000、SET_SERVO 的 p1 = 180。字段宽度错一个就会在这里炸。"""
    r = _parse(harness["readings"]["three_states"])
    body1 = [b for b in r["blocks"] if b["state"] == 1 and b["seq"] == "body"]
    assert body1[0]["op"] == 0x03 and body1[0]["p1"] == -90 and body1[0]["p0"] == 2000
    assert body1[0]["flags"] == 1, "TURN_DEG 的相对标志位丢了"
    body0 = [b for b in r["blocks"] if b["state"] == 0 and b["seq"] == "body"]
    assert body0[1]["op"] == 0x06 and body0[1]["p0"] == 4 and body0[1]["p2"] == 3000 and body0[1]["p3"] == 1
    body2 = [b for b in r["blocks"] if b["state"] == 2 and b["seq"] == "body"]
    assert body2[0]["op"] == 0x09 and body2[0]["p0"] == 1 and body2[0]["p1"] == 180
    assert body2[-1]["op"] == 0x08, "最后一个状态应该以 END 收尾"


def test_两个校验器对缺收尾块的判断一致(harness):
    """上位机的 `validateTask()` 报错 ⇒ 车上必须也拒收（TASK_E_TAIL）。两边都松就是静默的事故。"""
    item = next(i for i in harness["manifest"] if i["name"] == "no_tail")
    assert any("最后一块" in e for e in item["validatorErrors"]), item["validatorErrors"]
    r = _parse(harness["readings"]["no_tail"])
    assert r["code"] == 9, r
    assert "末尾" in r["err"], r["err"]


def test_上位机编的一律能被上位机解回来(harness):
    for item in harness["roundtrip"]:
        assert item["ok"], f"{item['name']} 自己编的自己解不开"


def test_块数与状态数的上限没被写松(harness):
    """32 状态 / 64 块这两个上限：造到边界必须仍然合法（车上 `TASK_MAX_STATES`/`TASK_MAX_BLOCKS`）。"""
    mx = _parse(harness["readings"]["max_states"])
    assert mx["code"] == 0, mx
    assert mx["hdr"]["n_states"] == 32
    many = _parse(harness["readings"]["many_blocks"])
    assert many["code"] == 0, many
    assert max(b["j"] for b in many["blocks"] if b["seq"] == "body") == 63


def test_镜像都是冷冰冰的二进制而不是JSON(harness):
    """编辑器里那个"烧写"按字节发；顺手钉一下产物确实是 .bin（不是 0xFF 空槽、也不是文本）。"""
    p = pathlib.Path(harness["dir"])
    for item in harness["manifest"]:
        data = (p / item["file"]).read_bytes()
        assert data[:2] == b"TK" or int.from_bytes(data[:2], "little") == 0x4B54, item["file"]
        assert len(data) == item["len"]
