"""从当前工程重新生成 portable/ 交付包（可单独拿走的上位机 + 固件移植模块）。

用法（在仓库任意位置）：
    python project/tools/pack_portable.py

产物：
    portable/host/       独立上位机（Python 桥 + 已构建的 Web 界面 + 启动脚本）
    portable/firmware/   固件里"换个板子也要带走"的模块 + 参考板级适配层
    portable/docs/       两边共用的详细文档（通信协议 / 任务编程 / 任务编排）

原则：portable/ 里的东西都是**生成物**，不要直接改；要改请改 project/ 下的源，
再重跑本脚本。这样两个目录不会各改各的、慢慢漂移。
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent          # <repo>/project
PORTABLE = PROJECT.parent / "portable"                     # <repo>/portable

# 上位机里"必须跟固件同仓才能跑"的测试：独立包里没有固件源码，整份不带
HOST_DROP_TESTS = {
    "test_firmware_defaults_sync.py",   # 读 project/firmware/App/Src/app_ui.c 的三处默认值
    "test_firmware_pipe.py",            # 用 host gcc 编固件真源码做对拍
    "test_audit_guards.py",             # 其中一项读 project/firmware/App/Svc/nvram.h
    "test_actions_spec.py",             # 从 firmware/App/Bsp/servo.c/.h 解析动作表
    "test_params_spec.py",              # 从 firmware/App/Svc/host_link.c 解析 SPECS[]
    "test_task_bytes.py",               # 要 web/src/lib/task.golden.ts 与固件格式对拍
}

# 固件移植包要带的目录（全部相对 project/firmware）
FW_COPY = [
    "App/Inc",
    "App/Src",
    "App/Svc",
    "App/Bsp",
    "App/Fonts",
    "Middlewares/kk_ui",
    "Middlewares/kk_oled",
    "Middlewares/SEGGER_RTT",
]

# 参考板级适配层：新板子只需要重写这几个文件里的板级部分
FW_REFERENCE = [
    "Core/Inc/board.h",
    "Core/Src/board.c",
    "Core/Inc/main.h",
    "STM32G474RBTx_FLASH.ld",
]

HOST_DOCS = ["API.md"]
FW_DOCS = ["上位机通信协议.md", "任务编程.md", "任务编排模块与逻辑.md", "任务编排控件与代码对照.md"]

IGNORE = shutil.ignore_patterns(
    "data", "__pycache__", "*.pyc", "pytest-of-*", ".pytest_cache", "node_modules",
    "build-*", "*.o", "*.d", "*.map", "*.elf", "*.bin", "*.hex",
)


def copy_tree(src: Path, dst: Path) -> None:
    """把 src 覆盖式同步到 dst：先复制/覆盖，再删掉 dst 里 src 没有的文件。

    不整棵 rmtree 再重建 —— Windows 上一个进程把 dst（或其子目录）当 cwd 时
    rmtree 会 WinError 32，导致包只被删了一半。覆盖式同步没有这个风险。
    """
    if not dst.exists():
        shutil.copytree(src, dst, ignore=IGNORE)
        return
    shutil.copytree(src, dst, ignore=IGNORE, dirs_exist_ok=True)
    src_files = {p.relative_to(src) for p in src.rglob("*") if p.is_file()}
    for p in sorted(dst.rglob("*"), key=lambda x: len(x.parts), reverse=True):
        if p.is_file() or p.is_symlink():
            rel = p.relative_to(dst)
            if rel not in src_files:
                try:
                    p.unlink()
                except OSError:
                    pass
        elif p.is_dir():
            try:
                if not any(p.iterdir()):
                    p.rmdir()
            except OSError:
                pass


def _keep_readme(dst: Path) -> str | None:
    """复制前把目标目录里手写的 README.md 读出来，复制后再放回去。

    portable/ 是生成物，但每个包根上的 README.md 是人工文档（不是从 project/ 复制的），
    重打包时不能被 copytree 一起冲掉。
    """
    p = dst / "README.md"
    return p.read_text(encoding="utf-8") if p.is_file() else None


def _restore_readme(dst: Path, text: str | None) -> None:
    if text is not None:
        (dst / "README.md").write_text(text, encoding="utf-8")


def main() -> int:
    fw = PROJECT / "firmware"
    console = PROJECT / "tools" / "console"
    docs = PROJECT / "docs"
    for p in (fw, console):
        if not p.is_dir():
            print(f"[x] 找不到 {p}")
            return 2

    # ---------- 1. 独立上位机 ----------
    host_dst = PORTABLE / "host"
    host_readme = _keep_readme(host_dst)
    copy_tree(console, host_dst)
    for name in HOST_DROP_TESTS:
        p = host_dst / "tests" / name
        if p.is_file():
            p.unlink()
    # 独立包的测试里，gcc 相关夹具按路径找不到固件源码就跳过（不删 conftest）
    _restore_readme(host_dst, host_readme)
    print(f"[ok] portable/host  <- project/tools/console ({len(list(host_dst.rglob('*')))} 项)")

    # ---------- 2. 固件移植包 ----------
    fw_dst = PORTABLE / "firmware"
    fw_readme = _keep_readme(fw_dst)
    fw_dst.mkdir(parents=True, exist_ok=True)
    for rel in FW_COPY:
        src = fw / rel
        if not src.is_dir():
            print(f"[!] 跳过不存在的 {rel}")
            continue
        dst = fw_dst / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        copy_tree(src, dst)
    ref = fw_dst / "参考板级适配层"
    ref.mkdir(parents=True, exist_ok=True)
    for rel in FW_REFERENCE:
        src = fw / rel
        if src.is_file():
            shutil.copy2(src, ref / Path(rel).name)
    # 字模工具（改文案后重新生成字模 / 查字）
    copy_tree(PROJECT / "tools" / "font", fw_dst / "tools" / "font")
    _restore_readme(fw_dst, fw_readme)
    print(f"[ok] portable/firmware <- project/firmware 的可移植模块")

    # ---------- 3. 共用详细文档 ----------
    # 注意：这里**不**删整个 docs 目录 —— 手写的移植说明书也放在 portable/docs 里，
    # 只覆盖从 project/docs 复制过来的那几份。
    docs_dst = PORTABLE / "docs"
    docs_dst.mkdir(parents=True, exist_ok=True)
    for name in FW_DOCS:
        src = docs / name
        if src.is_file():
            shutil.copy2(src, docs_dst / name)
    if (console / "docs" / "API.md").is_file():
        shutil.copy2(console / "docs" / "API.md", docs_dst / "上位机-API.md")
    print(f"[ok] portable/docs     <- project/docs 的协议/任务文档 + 上位机 API")

    print("\n完成。portable/ 是生成物，改动请回 project/ 再重跑本脚本。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
