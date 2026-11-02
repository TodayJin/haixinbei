"""串口安全闸（`bridge/serial_safety.py`）的逻辑：J-Link 的 CDC VCOM 上限**按探针型号分档**。

2026-10-03 的实测代价：为了找「921600 下一帧都收不到」的原因，对旧的 J-Link V9（PID `0105`）
的 VCOM 试了 460800 与 921600，结果探针的 USB 栈崩进 `PID_0101` bootloader 态，
`pnputil /restart-device` 救不回来，只能物理拔插。那颗探针的上限就是 230400。

2026-10-04 换上第二颗（J-Link OB-RA4M2-Full，PID **1024**，CDC = COM27）后逐档实测
230400/460800/921600/1500000/2000000/3000000 全通 ⇒ 上限 3000000，2026-10-05 定档 921600。
所以这些小函数要单独钉住：**认得出口、认得是哪一颗、按那颗的上限拦、认不出的口不误伤。**

不需要真硬件：`comports=` 参数注入假的口列表即可（`port_info` 只在没给时才去问 pyserial）。
"""
from __future__ import annotations

import os

import pytest

from bridge import serial_safety as ss

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.dirname(HERE)


class _Port:
    """pyserial `ListPortInfo` 的最小替身（只用得到这三个字段）。"""

    def __init__(self, device: str, hwid: str = "", description: str = "") -> None:
        self.device = device
        self.hwid = hwid
        self.description = description


JLINK = [_Port("COM26", "USB VID:PID=1366:0105 SER=000602711039", "JLink CDC UART Port")]
#: 2026-10-04 起的第二颗探针（OB-RA4M2-Full），CDC 面在 COM27 —— 车上现在就是它
JLINK_NEW = [_Port("COM27", "USB VID:PID=1366:1024 SER=24060505", "JLink CDC UART Port")]
CH340 = [_Port("COM11", "USB VID:PID=1A86:7523 SER=5B62037777", "USB-SERIAL CH340")]


def test_pyserial_style_hwid_is_recognised():
    assert ss.is_jlink_cdc("USB VID:PID=1366:0105 SER=000602711039")
    assert ss.is_jlink_cdc("usb vid:pid=1366:0105", "")          # 大小写不敏感


def test_pnp_style_hwid_is_recognised():
    # 设备管理器/PnP 的那套写法，probe 之外的脚本也会拿它来判
    assert ss.is_jlink_cdc("USB\\VID_1366&PID_0105\\000602711039")


def test_description_alone_is_enough_but_other_adapter_is_not():
    assert ss.is_jlink_cdc("", "JLink CDC UART Port")
    assert not ss.is_jlink_cdc("USB VID:PID=1A86:7523", "USB-SERIAL CH340")
    assert not ss.is_jlink_cdc("", "")


def test_port_info_finds_device_case_insensitively():
    hwid, desc = ss.port_info("com26", comports=JLINK)
    assert "1366:0105" in hwid
    assert desc == "JLink CDC UART Port"


def test_port_info_returns_empties_when_port_is_not_there():
    assert ss.port_info("COM99", comports=JLINK) == ("", "")


def test_cap_bauds_drops_the_rungs_that_brick_the_probe():
    kept, warn = ss.cap_bauds("COM26", (921600, 460800, 230400, 115200), comports=JLINK)
    assert kept == [230400, 115200]
    assert warn
    assert "921600" in warn and "460800" in warn
    assert "230400" in warn                                   # 告诉人上限是多少
    assert "--force-baud" in warn


def test_cap_bauds_leaves_other_adapters_untouched():
    bauds = (921600, 460800, 230400, 115200)
    kept, warn = ss.cap_bauds("COM11", bauds, comports=CH340)
    assert kept == list(bauds)
    assert warn == ""


def test_cap_bauds_leaves_unknown_ports_untouched():
    # 口不在场（拔了/换机器/没装 pyserial）时不猜、不拦 —— 别误伤一条不认识的线
    bauds = (921600, 460800, 230400, 115200)
    assert ss.cap_bauds("COM26", bauds, comports=[]) == (list(bauds), "")


def test_cap_bauds_keeps_one_rung_when_everything_was_dropped():
    kept, warn = ss.cap_bauds("COM26", (921600,), comports=JLINK)
    assert kept == [ss.J_LINK_MAX_BAUD]                       # 至少听一档，别什么都不听
    assert warn


def test_cap_bauds_force_keeps_everything():
    bauds = (921600, 460800)
    assert ss.cap_bauds("COM26", bauds, force=True, comports=JLINK) == (list(bauds), "")


def test_check_baud_refuses_above_the_ceiling_on_jlink():
    err = ss.check_baud("COM26", 921600, comports=JLINK)
    assert err
    assert "230400" in err and "921600" in err
    assert "--baud" in err and "--force-baud" in err
    assert ss.check_baud("COM26", 460800, comports=JLINK)


def test_check_baud_accepts_the_two_bauds_that_actually_work():
    assert ss.check_baud("COM26", 230400, comports=JLINK) == ""
    assert ss.check_baud("COM26", 115200, comports=JLINK) == ""


def test_check_baud_ignores_other_and_unknown_ports():
    assert ss.check_baud("COM11", 921600, comports=CH340) == ""
    assert ss.check_baud("COM26", 921600, comports=[]) == ""
    assert ss.check_baud("COM26", 921600, force=True, comports=JLINK) == ""


# ---------------------------------------------------------------- 第二颗探针（PID 1024）


def test_jlink_pid_reads_both_hwid_styles():
    assert ss.jlink_pid("USB VID:PID=1366:1024 SER=24060505") == "1024"
    assert ss.jlink_pid("USB\\VID_1366&PID_0105\\000602711039") == "0105"
    assert ss.jlink_pid("USB VID:PID=1A86:7523") == ""
    assert ss.jlink_pid("") == ""


def test_new_probe_recognised_and_cap_is_three_mbaud():
    assert ss.is_jlink_cdc("USB VID:PID=1366:1024 SER=24060505", "JLink CDC UART Port")
    assert ss.max_baud("USB VID:PID=1366:1024", "JLink CDC UART Port") == ss.J_LINK_MAX_BAUD_NEW  # 3000000


def test_new_probe_keeps_every_rung_we_actually_use():
    # 定档 921600 之后，probe/bench 扫的那几档在新探针上一档都不能砍
    bauds = (921600, 460800, 230400, 115200)
    assert ss.cap_bauds("COM27", bauds, comports=JLINK_NEW) == (list(bauds), "")
    assert ss.check_baud("COM27", 921600, comports=JLINK_NEW) == ""
    assert ss.check_baud("COM27", 3000000, comports=JLINK_NEW) == ""


def test_new_probe_still_caps_above_its_own_ceiling():
    kept, warn = ss.cap_bauds("COM27", (6000000, 3000000), comports=JLINK_NEW)
    assert kept == [3000000]
    assert warn and "6000000" in warn and "3000000" in warn
    err = ss.check_baud("COM27", 6000000, comports=JLINK_NEW)
    assert err and "3000000" in err and "--force-baud" in err


def test_unknown_jlink_pid_falls_back_to_the_conservative_ceiling():
    # 又换了一颗不认识的型号（VID 还是 1366）：按最保守的 230400 拦，别拿探针去赌
    unknown = [_Port("COM30", "USB VID:PID=1366:9999 SER=x", "JLink CDC UART Port")]
    assert ss.is_jlink_cdc("USB VID:PID=1366:9999", "JLink CDC UART Port")
    assert ss.max_baud("USB VID:PID=1366:9999", "JLink CDC UART Port") == ss.J_LINK_MAX_BAUD
    kept, warn = ss.cap_bauds("COM30", (921600, 230400), comports=unknown)
    assert kept == [230400] and warn
    assert ss.check_baud("COM30", 921600, comports=unknown)


# ---------------------------------------------------------------- 接线：桥真的过闸
# ★2026-11-02（审计 P1-2）：上面钉的是闸自己的判定；这里钉"网页/桥那条路真的调了它"。
# 只 grep 到调用不算数 —— 要断言 switch() 被拦时返回 ok:false、且**旧链路没被停**，
# 以及闸在 SerialLink 之前执行（否则探针已经被打开了）。


def _bridge(tmp_path, **kw):
    from bridge import server
    return server.Bridge(mode=kw.pop("mode", "sim"), console_dir=CONSOLE,
                         preset_dir=str(tmp_path / "presets"),
                         log_dir=str(tmp_path / "logs"), sse_batch_ms=0, **kw)


def test_bridge_switch_runs_the_baud_guard_and_keeps_the_old_link(monkeypatch, tmp_path):
    from bridge import server
    called = {}

    def fake_check(port, baud, force=False):
        called.update(port=port, baud=baud, force=force)
        return "COM26 是 J-Link V9 的 CDC VCOM：上限实测 230400（选 921600 会进 bootloader）"

    monkeypatch.setattr(server, "check_baud", fake_check)
    b = _bridge(tmp_path)
    b.start()
    try:
        old = b.runner
        r = b.switch("serial", "COM26", 921600)
        assert r["ok"] is False, r
        assert "230400" in r["error"] and "bootloader" in r["error"]
        assert called == {"port": "COM26", "baud": 921600, "force": False}
        # 旧链路（sim）必须还在跑：switch() 只在新链路建成功之后才停旧的
        assert b.runner is old
        assert b.runner.status()["mode"] == "sim"
        assert b.mode == "sim"
    finally:
        b.stop()


def test_bridge_switch_reaches_seriallink_only_after_the_guard_passes(monkeypatch, tmp_path):
    from bridge import server
    from bridge import link as linkmod
    opened = []

    def fake_serial(port, baud):
        opened.append((port, baud))
        return linkmod.SimLink()          # 用仿真顶替真串口：只验证接线顺序，不碰硬件

    monkeypatch.setattr(server, "check_baud", lambda port, baud, force=False: "")
    monkeypatch.setattr(server, "SerialLink", fake_serial)
    b = _bridge(tmp_path)
    b.start()
    try:
        r = b.switch("serial", "COM26", 921600)
        assert r["ok"] is True, r
        assert opened == [("COM26", 921600)]
        assert b.mode == "serial"
    finally:
        b.stop()


def test_bridge_start_refuses_bad_baud_before_opening_the_probe(monkeypatch, tmp_path):
    """start() 也必须过闸：真串口分支被拦时不能已经 `SerialLink(...)` 过（那就算打开探针了）。"""
    from bridge import server
    monkeypatch.setattr(server, "check_baud",
                        lambda port, baud, force=False: "refused: 921600 on probe 0105")
    opened = []
    monkeypatch.setattr(server, "SerialLink", lambda p, b: opened.append((p, b)))
    b = _bridge(tmp_path, mode="serial", port="COM26", baud=921600)
    with pytest.raises(SystemExit) as ei:
        b.start()
    assert "refused" in str(ei.value)
    assert opened == [], "闸必须在 SerialLink() 之前拦，否则探针已经被打开了"
