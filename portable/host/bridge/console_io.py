"""控制台输出的编码加固：别让最后一行结论把整个量测结果带走。

2026-10-03 实测：`python -m bridge.probe --port COM26 --seconds 2` 前面两个波特率的
数字都打出来了，**结论行**（`结论：有字节但解不出帧 ⇒ 波特率不匹配…`）却撞上

    UnicodeEncodeError: 'gbk' codec can't encode character '\\u21d2' in position 12

—— Windows 上 stdout 默认按 locale 编码（cp936）。GBK 收得下 `→`/`←`/`×`/`·`/`─`，
但**收不下 `⇒`**（用它的地方不止 probe：`bridge.bench` 的判定行、`send_wait` 的失败说明
也用它）。崩在最后一行等于把前面量到的数字全浪费掉。

加固策略：

- 输出**被重定向**（不是 tty，消费者是管道/文件/CI）⇒ 改成 UTF-8，别再让上游按错编码解码；
- **直接对着终端** ⇒ 保留控制台自己的编码（不然全是乱码），只把编不出的字符降级成 `?`。

加固是兜底，不是许可：新写的输出尽量别用 GBK 之外的字符。
"""
from __future__ import annotations

import sys


def harden_stdio(streams=None) -> None:
    """把 stdout/stderr 调成"写不出去也不崩"。`streams=` 只为测试注入假流用。"""
    for st in (sys.stdout, sys.stderr) if streams is None else streams:
        try:
            if st.isatty():
                st.reconfigure(errors="replace")
            else:
                st.reconfigure(encoding="utf-8", errors="replace")
        except Exception:      # 不是 TextIOWrapper、已被包装、已关掉……都无所谓
            pass
