"""控制台编码加固（`bridge/console_io.py`）。

2026-10-03 真撞上的现场：`python -m bridge.probe --port COM26 --seconds 2` 两个波特率的
数字都打出来了，**结论行**里的 `⇒` 在 Windows 上按 locale（cp936）编不出去，
`UnicodeEncodeError` 崩在最后一行 —— 前面量到的数字等于白量。这里钉住加固的两条分支。
"""
from __future__ import annotations

from bridge.console_io import harden_stdio


class _Stream:
    """只记 `reconfigure()` 收到什么。"""

    def __init__(self, tty: bool) -> None:
        self._tty = tty
        self.calls: list[dict] = []

    def isatty(self) -> bool:
        return self._tty

    def reconfigure(self, **kw) -> None:
        self.calls.append(kw)


def test_redirected_output_switches_to_utf8():
    # 管道/文件/CI 的消费者本来就按 UTF-8 解码，别再用 locale 编码写出去
    st = _Stream(tty=False)
    harden_stdio([st])
    assert st.calls == [{"encoding": "utf-8", "errors": "replace"}]


def test_terminal_keeps_its_encoding_and_only_downgrades_bad_chars():
    st = _Stream(tty=True)
    harden_stdio([st])
    assert st.calls == [{"errors": "replace"}]      # 改终端编码只会换来满屏乱码


def test_odd_streams_never_raise():
    harden_stdio([object()])                        # 没有 isatty/reconfigure

    class _Boom:
        def isatty(self) -> bool:
            return False

        def reconfigure(self, **kw):
            raise ValueError("already written to")

    harden_stdio([_Boom()])                         # 加固失败也不该把工具带崩


def test_default_covers_both_std_streams(monkeypatch):
    out, err = _Stream(False), _Stream(False)
    monkeypatch.setattr("sys.stdout", out)
    monkeypatch.setattr("sys.stderr", err)
    harden_stdio()
    assert out.calls and err.calls


def test_the_arrow_that_crashed_us_is_really_outside_gbk():
    # 钉住"为什么需要加固"：⇒ 编不出去，而 → ← × · ─ ═ 一直都没问题（别顺手把它们改掉）
    assert "⇒".encode("cp936", errors="ignore") == b""
    assert "⇒".encode("gbk", errors="ignore") == b""
    for ch in "→←×·─═":
        assert ch.encode("cp936")
