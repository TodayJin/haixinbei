"""仓库根路径与手册路径，供 tools/ 下脚本共用。"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATASHEETS = ROOT / "docs" / "datasheets"
OUT = ROOT / "out"
OUT.mkdir(exist_ok=True)


def ds(name):
    """docs/datasheets/ 下的手册绝对路径（字符串）。"""
    return str(DATASHEETS / name)


def out(name):
    """out/ 下的产物绝对路径（字符串），目录自动建好。"""
    return str(OUT / name)
