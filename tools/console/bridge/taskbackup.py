"""写任务到车**之前**先备份：把车上任务目录 + 每条任务的**镜像字节**抄到磁盘（**只读**）。

为什么需要它（2026-10-07 的教训）：两次**参数填错**的 `commit`（把 `crc` 当成头部
offset 8..9、`len` 当成 4..5 —— 正确是 **`crc` 在 6..7、`len` 在 8..9**）把车上原有
三条任务**从目录里抹掉了** ✗（程序区页 59..61 没坏，丢的只是目录条目），只能重烧。
⇒ **写车前先跑这个**，目录和字节都先落到磁盘上。

只发 **GET**（绝不 put / commit / run / delete）：
  `GET /api/tasks`            → 目录（槽 / 名字 / len / crc / flags）
  `GET /api/tasks/get?id=N`   → 该槽的整条镜像；**`hex` 字段就是镜像字节的十六进制**
  （契约见 `bridge/server.py:313-323` 与 `bridge/state.py` 的 `task_list()` / `task_get()`）

产出（默认仓库根的 `out/taskbackup/`，`--out` 可改）：
  `<槽>_<名字>_<CRC>.bin`     每条任务的镜像，逐字节
  `index.json`                槽 / 名字 / 长度 / crc / 时间戳 / 文件名 / sha256

★「坏读不许覆盖好备份」（2026-10-07 修）：文件名里**只有目录的 crc**，而回读被截断时
  桥会把坏数据当成功返回（2026-10-07 实测：任务镜像超过 ~2.4KB 时，车端 DMA 发送队列
  丢的是**尾巴上的数据帧**、收尾帧却活着 ⇒ 桥收下"连续前缀 + 收尾"就当读完了），
  于是**坏数据算出与好备份同一个文件名**，把上一次的好备份盖掉 ✗（实测发生过）。
  现在的落盘顺序：**先写临时名 `<...>.bin.part` → 五项校验全过 → 才 `os.replace()` 成正式名**。
  失败时：临时文件**原样留着**（后缀还是 `.part`，一眼看出是坏读），正式名**一个字节都不碰**；
  正式名已存在且内容一致时连临时文件都不留（说明这条本来就有好备份）。

校验（**任一不符就报错、退出码 1**，不静默通过）——也是"能不能改名成正式名"的判据：
  ① 落盘文件长度 == 目录报的 `len`
  ② 镜像头 offset **6..7**（`crc16`）== 目录报的 `crc`
  ③ magic（offset 0..1）== `0x4B54`、头部 `len`（offset **8..9**）== 目录报的 `len`
  ④ 按 body 重算 CRC16/CCITT（覆盖 `bytes[12:]`，与固件 `TaskProg_Crc16()` 同口径）== 目录 `crc`

用法（workdir `tools/console`）：
  python -m bridge.taskbackup
  python -m bridge.taskbackup --base http://127.0.0.1:8793
  python -m bridge.taskbackup --out D:\\bak\\tasks
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request

from .console_io import harden_stdio                 # 输出编码加固（管道里自动转 UTF-8）
from .protocol import crc16_ccitt                    # 唯一一份 CRC 实现，别抄第二份

MAGIC = 0x4B54
HDR_LEN = 12
# 镜像头（12 B）：magic(2) ver(1) n_states(1) main(2) crc16(2) len(2) res(2)
OFF_CRC = 6
OFF_LEN = 8
REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "..", "..", ".."))
DEFAULT_OUT = os.path.join(REPO_ROOT, "out", "taskbackup")
DEFAULT_BASE = "http://127.0.0.1:8770"
PART_SUFFIX = ".part"          # 临时名后缀：五项校验全过才改名成正式 .bin（坏读只脏这个）


class BackupError(RuntimeError):
    """桥没在跑 / 端点报错 / 目录读不回来 —— 一律带着人话抛出来。"""


# ---------------------------------------------------------------- HTTP（只读）
def http_get(base: str, path: str, timeout: float = 6.0) -> dict:
    url = base.rstrip("/") + path
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            raw = r.read()
            code = r.status
    except urllib.error.HTTPError as e:              # 400/404/501 也带 JSON 正文
        raw, code = e.read(), e.code
    except urllib.error.URLError as e:
        raise BackupError(f"连不上桥 {url}（{e.reason}）—— 桥在跑吗？"
                          f"（python -m bridge.main --port 8770 --baud 921600 "
                          f"--serial COM27 --no-browser，workdir tools/console）") from e
    except OSError as e:
        raise BackupError(f"连不上桥 {url}（{e}）—— 桥在跑吗？") from e
    try:
        d = json.loads(raw.decode("utf-8", "replace"))
    except ValueError as e:
        raise BackupError(f"{url} 回的不是 JSON（HTTP {code}）：{raw[:200]!r}") from e
    if not isinstance(d, dict):
        raise BackupError(f"{url} 回的 JSON 不是对象：{d!r}")
    return d


def fetch_dir(base: str) -> list:
    """`/api/tasks` → 目录条目（槽号 / 名字 / len / crc / flags）。"""
    d = http_get(base, "/api/tasks")
    if not d.get("ok"):
        raise BackupError(f"读目录失败：{d.get('error') or d}")
    items = d.get("items") or []
    out = []
    for it in items:
        out.append({"id": int(it["id"]), "name": str(it.get("name", "")),
                    "len": int(it.get("len", 0)), "crc": int(it.get("crc", 0)),
                    "flags": int(it.get("flags", 0) or 0)})
    return sorted(out, key=lambda x: x["id"])


def fetch_image(base: str, slot: int) -> bytes:
    """`/api/tasks/get?id=N` → 镜像字节（桥回的是 `hex` 字符串）。"""
    d = http_get(base, f"/api/tasks/get?id={slot}")
    if not d.get("ok"):
        raise BackupError(f"读回槽 #{slot} 失败：{d.get('error') or d}")
    hexs = d.get("hex")
    if not isinstance(hexs, str):
        raise BackupError(f"槽 #{slot} 的回包里没有 hex 字段：{sorted(d)}")
    try:
        return bytes.fromhex(hexs)
    except ValueError as e:
        raise BackupError(f"槽 #{slot} 的 hex 不是合法十六进制：{e}") from e


# ---------------------------------------------------------------- 落盘与校验
def safe_name(name: str) -> str:
    """任务名 → 能当文件名的样子（Windows 非法字符换掉、去掉结尾的点/空格）。"""
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", name).strip().rstrip(". ")
    return s or "task"


def u16le(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def check_image(slot: int, img: bytes, want_len: int, want_crc: int) -> tuple:
    """返回 (ok, 校验细节字符串, 问题列表)。记号只用 ASCII（GBK 控制台也打得出来）。"""
    bad, notes = [], []
    notes.append(f"长度 {len(img)}B" + (" ok" if len(img) == want_len else f" !=目录 {want_len}"))
    if len(img) != want_len:
        bad.append(f"长度不符：文件 {len(img)} B ≠ 目录 {want_len} B")
    if len(img) < HDR_LEN:
        bad.append(f"镜像太短（{len(img)} B < 头 {HDR_LEN} B），不可能是任务镜像")
        return (not bad), " / ".join(notes + bad), bad
    magic = u16le(img, 0)
    notes.append(f"magic 0x{magic:04X}" + (" ok" if magic == MAGIC else f" !=0x{MAGIC:04X}"))
    if magic != MAGIC:
        bad.append(f"magic 不对：0x{magic:04X} ≠ 0x{MAGIC:04X}")
    hdr_crc, hdr_len = u16le(img, OFF_CRC), u16le(img, OFF_LEN)
    notes.append(f"头crc[6..7] 0x{hdr_crc:04X}"
                 + (" ok" if hdr_crc == want_crc else f" !=目录 0x{want_crc:04X}"))
    if hdr_crc != want_crc:
        bad.append(f"★头里 offset 6..7 的 crc = 0x{hdr_crc:04X} ≠ 目录报的 crc 0x{want_crc:04X}")
    notes.append(f"头len[8..9] {hdr_len}" + (" ok" if hdr_len == want_len else f" !=目录 {want_len}"))
    if hdr_len != want_len:
        bad.append(f"头里 offset 8..9 的 len = {hdr_len} ≠ 目录报的 len {want_len}")
    body_crc = crc16_ccitt(img[HDR_LEN:])
    notes.append(f"bodyCRC 0x{body_crc:04X}"
                 + (" ok" if body_crc == want_crc else f" !=目录 0x{want_crc:04X}"))
    if body_crc != want_crc:
        bad.append(f"按 `bytes[12:]` 重算的 CRC = 0x{body_crc:04X} ≠ 目录 0x{want_crc:04X}")
    return (not bad), " / ".join(notes), bad


def save_verified(out_dir: str, fname: str, slot: int, img: bytes,
                  want_len: int, want_crc: int) -> tuple:
    """把镜像落盘：**先写 `<fname>.part`，五项校验全过才改名成正式名**。

    返回 `(ok, detail, bad, saved_as, note)`：
      · ok=True  → `saved_as` = 正式名（内容已核对过），note 说明是不是"原地没动"；
      · ok=False → `saved_as` = 临时名（**原样留着**给人事后看），正式名一个字节都没碰，
                   note 里写清"原来那个好备份还在/不在"。
    """
    fpath = os.path.join(out_dir, fname)
    tpath = fpath + PART_SUFFIX
    with open(tpath, "wb") as f:                     # 先落临时名（坏数据也只脏这个文件）
        f.write(img)
    with open(tpath, "rb") as f:                     # 再读回磁盘对拍，防写坏
        back = f.read()
    ok, detail, bad = check_image(slot, back, want_len, want_crc)
    if back != img:
        ok, bad = False, bad + ["落盘后读回与收到的字节不一致"]
    if not ok:
        old = os.path.exists(fpath)
        return (False, detail, bad, os.path.basename(tpath),
                f"坏读留在 {os.path.basename(tpath)}；正式名 {fname} "
                + ("**原样保留**（上一次的好备份没被覆盖）" if old else "本来就不存在（没有好备份可保）"))
    # 校验全过：正式名已存在且内容一致 ⇒ 说明这条本来就有好备份，临时文件都别留
    if os.path.exists(fpath):
        with open(fpath, "rb") as f:
            old_img = f.read()
        if old_img == back:
            os.remove(tpath)
            return (True, detail, [], fname, "已有同名好备份且内容一致，未改动")
        # 同名但内容不同：理论上只有 CRC 碰撞才会走到这里 —— 宁可不写，也不覆盖
        os.remove(tpath)
        return (False, detail, [f"同名备份已存在但内容不同（crc 撞名？）—— 不覆盖，保留原文件"],
                f"{fname}（原文件未动）", "检测到同名不同内容：不覆盖")
    os.replace(tpath, fpath)                         # 同一分区内改名 = 原子
    return (True, detail, [], fname, "校验全过后改名")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="把车上任务目录 + 镜像字节备份到磁盘（只读，不发写指令）")
    ap.add_argument("--base", default=os.environ.get("CONSOLE_BASE", DEFAULT_BASE),
                    help=f"桥的地址（默认 {DEFAULT_BASE}）")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help=f"落盘目录（默认 {DEFAULT_OUT}）")
    ap.add_argument("--timeout", type=float, default=6.0, help="单次 HTTP 超时（秒）")
    a = ap.parse_args(argv)
    harden_stdio()                                   # 管道里转 UTF-8，别让最后一行结论把结果带走

    stamp = _dt.datetime.now().astimezone().isoformat(timespec="seconds")
    print(f"[backup] 桥 {a.base} | 现在 {stamp}")

    # 链路状态只是**提示**：连不上也照样试着读目录（读得到就说明链路是好的）
    try:
        lk = http_get(a.base, "/api/link", a.timeout)
        print(f"[backup] 链路 connected={lk.get('connected')} "
              f"port={lk.get('port', '?')} stats={lk.get('stats', {})}")
        if not lk.get("connected"):
            print("[backup] ! 桥报链路没连上 —— 下面读目录多半会失败")
    except BackupError as e:
        print(f"[backup] ! {e}")

    try:
        items = fetch_dir(a.base)
    except BackupError as e:
        print(f"[backup] FAIL {e}")
        return 1
    print(f"[backup] 目录 {len(items)} 条："
          + ("、".join(f"#{it['id']} {it['name'] or '(无名)'} {it['len']}B "
                       f"crc 0x{it['crc']:04X}" for it in items) or "（车上没有任务）"))

    os.makedirs(a.out, exist_ok=True)
    print(f"[backup] 落盘目录 {os.path.abspath(a.out)}")

    recs, failed = [], []
    for it in items:
        slot = it["id"]
        fname = f"{slot}_{safe_name(it['name'])}_{it['crc']:04X}.bin"
        try:
            img = fetch_image(a.base, slot)
        except BackupError as e:
            print(f"[backup] #{slot} FAIL {e}")
            failed.append(f"#{slot} {e}")
            continue
        ok, detail, bad, saved_as, note = save_verified(
            a.out, fname, slot, img, it["len"], it["crc"])
        recs.append({"slot": slot, "name": it["name"], "len": it["len"], "crc": it["crc"],
                     "crc_hex": f"0x{it['crc']:04X}", "flags": it["flags"], "time": stamp,
                     "file": fname, "saved_as": saved_as, "note": note,
                     "bytes": len(img),
                     "sha256": hashlib.sha256(img).hexdigest(),
                     "header_len": u16le(img, OFF_LEN) if len(img) >= HDR_LEN else None,
                     "header_crc": u16le(img, OFF_CRC) if len(img) >= HDR_LEN else None,
                     "verified": bool(ok)})
        print(f"[backup] #{slot} {it['name'] or '(无名)'} → {saved_as}  "
              f"{'OK' if ok else 'FAIL'}  {detail}")
        print(f"[backup]    · {note}")
        for b in bad:
            print(f"[backup]    - {b}")
            failed.append(f"#{slot} {b}")

    index = {"tool": "bridge.taskbackup", "version": 1, "base": a.base, "time": stamp,
             "out_dir": os.path.abspath(a.out), "dir_count": len(items),
             "items": recs, "ok": (not failed)}
    ipath = os.path.join(a.out, "index.json")
    with open(ipath, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"[backup] index.json 写好（{len(recs)} 条）→ {ipath}")

    if failed:
        print(f"[backup] FAIL 有 {len(failed)} 处不一致，**别急着写车**：")
        for b in failed:
            print(f"[backup]    {b}")
        print(f"[backup] 注意：坏读只留在 `*{PART_SUFFIX}` 里，正式名 .bin 一个字节都没被覆盖")
        return 1
    print(f"[backup] OK 全部 {len(recs)} 条对上了（长度 / 头 crc[6..7] / magic / 头 len[8..9] / body CRC 五项），"
          f"且都已改名成正式 .bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
