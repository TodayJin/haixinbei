"""链路体检工具（`bridge/probe.py`）的判定逻辑：用**合成字节**钉住 `decode_bytes`/`verdict`。

真串口与真车不在这里测（CI 没有板子）。这里只回答一个问题：同一批字节，工具会不会把
「波特率不匹配的乱码」和「干净的一帧」判成同一类 —— 那正是 2026-10-03 在 921600 上
`rx:0` 时要分清的两件事。
"""
from __future__ import annotations

from bridge import protocol as pr
from bridge.probe import DEFAULT_BAUDS, Result, decode_bytes, verdict


def _frame(ftype: int = pr.T_TELEM, seq: int = 1, payload: bytes = b"\x00" * 32) -> bytes:
    return pr.build_frame(ftype, seq, payload)


def test_default_ladder_is_the_fallback_order():
    # 用户要的 921600 在最前，历史值 115200 在最后，且整体单调下降
    assert DEFAULT_BAUDS[0] == 921600
    assert DEFAULT_BAUDS[-1] == 115200
    assert list(DEFAULT_BAUDS) == sorted(DEFAULT_BAUDS, reverse=True)


def test_valid_frame_decodes_to_one_clean_frame():
    raw = _frame(payload=bytes(range(32)))
    frames, crc, resyncs = decode_bytes(raw)
    assert len(frames) == 1
    assert crc == 0
    assert resyncs == 0
    assert frames[0].type == pr.T_TELEM
    assert frames[0].seq == 1
    assert frames[0].payload == bytes(range(32))
    assert verdict(len(raw), len(frames), crc) == "1 帧，零 CRC 错"


def test_zero_bytes_points_at_wiring_not_baud():
    text = verdict(0, 0, 0)
    assert "0 字节" in text
    assert "线没通" in text
    assert "波特率" not in text.split("：")[0]          # 别把「什么都没来」说成波特率问题


def test_pure_garbage_is_reported_as_baud_mismatch():
    junk = bytes(range(1, 60))                          # 里面一个 A5 都没有
    frames, crc, _ = decode_bytes(junk)
    assert frames == []
    assert crc == 0
    text = verdict(len(junk), len(frames), crc)
    assert "一帧都解不出" in text
    assert "波特率不匹配" in text


def test_broken_crc_is_counted_and_not_a_frame():
    raw = bytearray(_frame())
    raw[-1] ^= 0xFF                                     # 只坏 CRC 高字节
    frames, crc, _ = decode_bytes(bytes(raw))
    assert frames == []
    assert crc == 1
    # 一帧都没解出来时判定词走「波特率不匹配」那一支（对现场是对的：满屏 CRC 错就是波特率错配），
    # CRC 次数由数字行单独报出来。
    assert "一帧都解不出" in verdict(len(raw), len(frames), crc)


def test_a_few_crc_errors_between_good_frames_is_flaky_not_usable():
    text = verdict(500, 3, 2)
    assert "3 帧" in text
    assert "2 次 CRC 错" in text
    assert "边缘" in text
    assert Result(baud=921600, rx_bytes=500, frames=[pr.Frame(pr.T_TELEM, 1, b"")],
                  crc_errors=2).usable is False


def test_half_frame_waits_instead_of_failing():
    raw = _frame()
    frames, crc, _ = decode_bytes(raw[:6])               # 半截帧：既不是帧，也不算错
    assert frames == []
    assert crc == 0
    assert "解不出" in verdict(6, 0, 0)


def test_garbage_prefix_then_good_frame_recovers():
    # 现场最真实的形态：先吃了一串乱码（或从一帧中间开始听），随后对齐到真帧
    data = b"\x11\x22\x33" + _frame(seq=7)
    frames, crc, _ = decode_bytes(data)
    assert len(frames) == 1
    assert frames[0].seq == 7
    assert crc == 0


def test_result_sentence_and_usable():
    ok = Result(baud=921600, rx_bytes=99, frames=[pr.Frame(pr.T_TELEM, 1, b"\x00" * 32)])
    assert ok.usable is True
    assert ok.sentence == "1 帧，零 CRC 错"

    flaky = Result(baud=921600, rx_bytes=99, frames=[pr.Frame(pr.T_TELEM, 1, b"")], crc_errors=2)
    assert flaky.usable is False                        # 有帧但夹 CRC 错 ⇒ 不能当链路用

    silent = Result(baud=921600)
    assert silent.usable is False
    assert "0 字节" in silent.sentence

    unopenable = Result(baud=921600, error="拒绝访问")
    assert unopenable.usable is False
    assert unopenable.sentence == "打不开：拒绝访问"


def test_result_type_histogram_counts_by_type():
    r = Result(baud=921600, frames=[
        pr.Frame(pr.T_TELEM, 1, b""),
        pr.Frame(pr.T_TELEM, 2, b""),
        pr.Frame(pr.T_PARAM_GET, 3, b""),
    ])
    text = r.type_histogram()
    assert text == "%s×2、%s×1" % (pr.TYPE_NAMES[pr.T_TELEM], pr.TYPE_NAMES[pr.T_PARAM_GET])
    assert Result(baud=921600).type_histogram() == "—"


# ------------------------------------------- 汇总那几行的口径（用假的 probe 结果）
def _main_with(monkeypatch, results):
    from bridge import probe as pb
    it = iter(results)
    monkeypatch.setattr(pb, "probe", lambda *a, **k: next(it))
    # 汇总逻辑跟"本机那条口是什么型号"无关：串口安全闸换成照单全收
    # （闸自己的判定在 tests/test_serial_safety.py 里钉；接线在下面两条钉）
    monkeypatch.setattr(pb, "cap_bauds", lambda port, bauds, force=False: (list(bauds), ""))
    return pb.main


def test_summary_all_unopenable_says_port_not_baud(monkeypatch, capsys):
    # 今天真实遇到过的形态：COM26 @921600 报「信号灯超时时间已到」，
    # 后续几档直接 PermissionError —— 一个字都没读到，不能说成"波特率不匹配"
    main = _main_with(monkeypatch, [
        Result(baud=921600, error="Cannot configure port: OSError(22, '信号灯超时时间已到')"),
        Result(baud=115200, error="could not open port 'COM26': PermissionError(13, ...)"),
    ])
    rc = main(["--port", "COM26", "--bauds", "921600,115200"])
    out = capsys.readouterr().out
    assert rc == 2
    assert "打不开端口" in out
    assert "JLink CDC UART Port" in out                 # 指向真正该查的东西
    assert "解不出帧" not in out


def test_summary_opens_but_silent(monkeypatch, capsys):
    main = _main_with(monkeypatch, [Result(baud=921600), Result(baud=115200)])
    rc = main(["--port", "COM26", "--bauds", "921600,115200"])
    out = capsys.readouterr().out
    assert rc == 1
    assert "0 字节" in out
    assert "打不开端口" not in out


def test_summary_picks_highest_usable(monkeypatch, capsys):
    good = Result(baud=921600, rx_bytes=100, frames=[pr.Frame(pr.T_TELEM, 1, b"\x00" * 32)])
    main = _main_with(monkeypatch, [Result(baud=921600), good])
    rc = main(["--port", "COM26", "--bauds", "921600,115200"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "最高可用波特率 = 921600" in out


# ------------------------------------------- 串口安全闸的接线（假的 cap_bauds）
def test_main_hands_the_ladder_to_the_safety_gate(monkeypatch, capsys):
    from bridge import probe as pb

    gate: list[tuple] = []
    probed: list[int] = []

    def fake_gate(port, bauds, force=False):
        gate.append((port, list(bauds), force))
        return [230400], "闸：跳过 921600"

    monkeypatch.setattr(pb, "cap_bauds", fake_gate)
    monkeypatch.setattr(pb, "probe",
                        lambda port, baud, sec, hello: (probed.append(baud), Result(baud=baud))[1])

    rc = pb.main(["--port", "COM26", "--bauds", "921600,230400"])
    err = capsys.readouterr().err
    assert gate == [("COM26", [921600, 230400], False)]     # 用户给的梯子原样进闸
    assert probed == [230400]                               # 只听闸放行的档
    assert "闸：跳过 921600" in err                          # 警告走 stderr，不混进结论
    assert rc == 1                                          # 一帧没解出（0 字节）


def test_force_baud_reaches_the_gate(monkeypatch, capsys):
    from bridge import probe as pb

    seen: list[bool] = []
    monkeypatch.setattr(pb, "cap_bauds",
                        lambda port, bauds, force=False: (seen.append(force), (list(bauds), ""))[1])
    monkeypatch.setattr(pb, "probe", lambda port, baud, sec, hello: Result(baud=baud))
    pb.main(["--port", "COM26", "--bauds", "921600", "--force-baud"])
    assert seen == [True]
