"""上位机桥（Python，只依赖标准库 + pyserial）。

    python -m bridge.main --sim            # 无车也能跑（内置仿真车）
    python -m bridge.main --serial COM26   # 接真车（无线下载器 / J-Link VCOM）
    python -m bridge.main --list-ports     # 看有哪些串口

冻结契约见 ../docs/API.md；前端是 ../web（Vite + React），桥默认托管它的 dist。
"""
__all__ = ["protocol", "params", "link", "sim", "state", "server", "main"]
VERSION = "1.0"
