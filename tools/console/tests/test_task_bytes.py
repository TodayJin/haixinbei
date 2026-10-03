"""任务程序格式：**真固件源码**编出来的字节 ↔ 金样本 ↔ 上位机 `task.ts`，多边对拍。

为什么要有这个文件：`firmware/App/Svc/task_prog.c` 的字节布局与
`tools/console/web/src/lib/task.ts` 的编码器必须逐字节一致，而两边是**两个作者、两套类型宽度**
（C 的 u16/i16 对 TS 的 number）。谁都不该靠人读文档对齐，所以走一条生成链：

  1. 用 gcc 把 `tools/console/tests/c/taskgen.c` 链上**真** `firmware/App/Svc/task_prog.c`
     编成 PC 程序，跑出镜像（程序内部先用 `TaskProg_Validate()` 自检，不合格就退出码 1）；
  2. 用 **python 手写的独立解码器**（`make_golden.py` 里的 `decode()`）把它解回场景，
     和 fixture 里那份"人写的场景"逐字段比 —— 编码器与解码器是两个作者、两套类型宽度；
  3. 已提交的 `fixtures/task.golden.json` 与 `web/src/lib/task.golden.ts` 必须**还是**这份
     镜像的产物（不新鲜 = 有人改了格式层却没跑 `make_golden.ps1`）。

`task.golden.test.ts` 负责最后一环：TS 的 `encodeTask()` 必须编出同一个字节串。
**两侧都不再手抄十六进制**（`firmware/test/t_task.c` 里那条字面量已删）：唯一的权威字节串
就是这条链的产物，谁改了格式层，这里或 vitest 至少红一个。
没有 gcc 时整组 skip（`TASK_GCC` / `PIPE_GCC` 可指定）。
"""
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GCC = (os.environ.get("TASK_GCC") or os.environ.get("PIPE_GCC") or shutil.which("gcc")
       or r"C:\mingw64\bin\gcc.exe")
FIXTURE = os.path.join(HERE, "fixtures", "task.golden.json")
TS_GOLDEN = os.path.join(REPO, "tools", "console", "web", "src", "lib", "task.golden.ts")


def _load_make_golden():
    """借用生成器里的解码器（同一个文件，避免这里再手抄一份布局）"""
    path = os.path.join(HERE, "c", "make_golden.py")
    spec = importlib.util.spec_from_file_location("_make_golden", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


MG = _load_make_golden()


def _fixture() -> dict:
    with open(FIXTURE, "r", encoding="utf-8") as fh:
        return json.load(fh)


@pytest.fixture(scope="module")
def c_image(tmp_path_factory):
    """编一次 taskgen.c，返回 (镜像字节, 程序 stdout)。"""
    if not (os.path.isfile(GCC) or shutil.which(GCC)):
        pytest.skip(f"没有 gcc，跳过任务格式跨语言对拍（可用 TASK_GCC 指定）：{GCC}")
    tmp = tmp_path_factory.mktemp("taskgen")
    exe = os.path.join(str(tmp), "taskgen.exe" if os.name == "nt" else "taskgen")
    cmd = [GCC, "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-I" + os.path.join(REPO, "firmware", "App", "Svc"),
           os.path.join(HERE, "c", "taskgen.c"),
           os.path.join(REPO, "firmware", "App", "Svc", "task_prog.c"),
           "-o", exe]
    cp = subprocess.run(cmd, capture_output=True, timeout=600)
    # gcc 在中文 Windows 上按 GBK 吐诊断，一律按 bytes 收再容错解码
    assert cp.returncode == 0, ("taskgen.c 编译失败：\n" + cp.stdout.decode("utf-8", "replace")
                               + cp.stderr.decode("utf-8", "replace"))
    blob = os.path.join(str(tmp), "task.bin")
    p = subprocess.run([exe, blob, "-hex"], capture_output=True, timeout=120)
    err = p.stderr.decode("utf-8", "replace").strip()
    assert p.returncode == 0, ("taskgen 自检失败（真 task_prog.c 的 TaskProg_Validate 拒了它）：" + err)
    assert err == "", f"taskgen 不该往 stderr 写东西：{err}"
    with open(blob, "rb") as fh:
        return fh.read(), p.stdout.decode("utf-8", "replace")


def test_c_image_matches_fixture(c_image):
    """真固件格式层编出来的字节 == 提交在仓库里的金样本（长度 + CRC + 全字节）"""
    img, out = c_image
    fx = _fixture()
    assert fx["len"] == len(img), "fixture 的长度与真 C 编出来的不一致（格式变了？跑 make_golden.ps1）"
    assert fx["crc16"] == MG.crc16_ccitt(img[12:]), "fixture 的 CRC 与真 C 编出来的不一致"
    assert fx["hex"] == img.hex().upper(), "fixture 的字节与真 C 编出来的不一致（跑 make_golden.ps1）"
    assert f"len={len(img)}" in out and f"crc16=0x{MG.crc16_ccitt(img[12:]):04X}" in out


def test_fixture_scenario_is_what_c_encoded(c_image):
    """独立解码器：C 编出来的确实是 fixture 里那个人写的场景（字段宽度、有符号性都对）"""
    img, _ = c_image
    fx = _fixture()
    got = MG.decode(img)
    want = MG.scene_from_fixture(fx["scenario"])
    assert got == want, "C 侧 taskgen.c 里的场景与 fixture 的 scenario 不一致"
    # 顺手钉住几个"只有类型宽度写对了才对"的值
    assert got["states"][1]["body"][0]["p1"] == -90        # i16 负角度
    assert got["states"][0]["body"][0]["p0"] == 6480       # u16 速度
    assert got["states"][2]["body"][2]["p3"] == 0xFFFF     # 0xFFFF = 超时后继续
    assert [s["name"] for s in got["states"]] == ["常态循迹", "路口转90", "摆舵机"]


def test_ts_golden_file_matches_c(c_image):
    """上位机那份 TS 金样本与 C 的字节一致（vitest 再拿它钉住 task.ts 的编码器）"""
    img, _ = c_image
    with open(TS_GOLDEN, "r", encoding="utf-8") as fh:
        src = fh.read()
    hexs = "".join(re.findall(r"'([0-9A-Fa-f]{2,})'", src.split("GOLDEN_HEX")[1].split(";")[0]))
    assert hexs.upper() == img.hex().upper(), "task.golden.ts 的 GOLDEN_HEX 与真 C 的字节不一致"
    assert int(re.search(r"GOLDEN_LEN = (\d+)", src).group(1)) == len(img)
    assert int(re.search(r"GOLDEN_CRC16 = 0x([0-9a-fA-F]+)", src).group(1), 16) == MG.crc16_ccitt(img[12:])
    m = re.search(r"GOLDEN_NAMES: string\[\] = \[(.*?)\]", src, re.S)
    names = json.loads("[" + m.group(1).replace("'", '"') + "]") if m else []
    assert names == ["常态循迹", "路口转90", "摆舵机"], f"task.golden.ts 的名字不对：{names}"


def test_crc_over_body_is_not_a_formality(c_image):
    """CRC 确实覆盖头之后的全部字节：翻掉最后一个字节，重算的值与存的就不一样"""
    img, _ = c_image
    bad = bytearray(img)
    bad[-1] ^= 0x01
    assert MG.crc16_ccitt(bytes(bad[12:])) != struct.unpack_from("<H", img, 6)[0]
    # 而且 python 解码器读出来的场景真的变了（说明那个字节在载荷区里，不是填充）
    assert MG.decode(bytes(bad)) != MG.decode(img)


def test_ts_golden_file_is_regenerated_text(tmp_path):
    """`task.golden.ts` 必须与生成器**现在**会写出的文本逐字符一致。

    只比 GOLDEN_HEX/LEN/CRC16/NAMES 会漏掉"生成器改了、文件没重跑"这类漂移
    （注释抬头、块的 flags/p3、状态名列表都可能变），所以直接重生成一遍比全文。
    本用例不需要 gcc：纯 python 拿已提交的 fixture 重新渲染。
    """
    fx = _fixture()
    out = pathlib.Path(str(tmp_path)) / "task.golden.ts"
    MG.write_ts(fx, out)
    got = out.read_text(encoding="utf-8")
    with open(TS_GOLDEN, "r", encoding="utf-8") as fh:
        want = fh.read()
    assert got == want, "task.golden.ts 不是 fixture 现在的产物（跑 make_golden.ps1）"
