"""串口安全闸：别为了试一个波特率把 J-Link 探针再打死一次。

按**哪一颗探针（VID:PID）**分档 —— 2026-10-05 车上换了第二颗探针，两颗的上限不一样：

- `1366:0105`（J-Link V9，DP 复合设备 `USB\\VID_1366&PID_0105\\...` 的 MI_00 面 =
  「JLink CDC UART Port」）2026-10-03 实测：**115200 / 230400** 能打开、能读到字节
  （230400 那档读到 2791 字节，正是 115200 的约 2 倍）；**460800** = `OSError(22, '信号灯超时
  时间已到')` = ERROR_SEM_TIMEOUT(121)，卡在 SET_LINE_CODING；**921600** =
  `PermissionError(13, '连到系统上的设备没有发挥作用。')` = ERROR_GEN_FAILURE(31)。
  后果更麻烦：**只要对它试 ≥460800，USB 栈就崩进 PID_0101 bootloader 态**，软件救不回来
  （`pnputil /restart-device` 跑完连 VID_1366 都从设备树里消失），**只能物理拔插 USB**。

- `1366:1024`（J-Link OB-RA4M2-Full，S/N 24060505，CDC = COM27）2026-10-04 逐档实测：
  230400 / 460800 / 921600 / 1500000 / 2000000 / 3000000 **全部打开成功且数据合法**
  （PID 全程 1024），真起桥 @3000000 + `set_telem_hz=200` 得 rx=201.3 帧/s、crc=0、
  resync=0、rtt=5.8ms ⇒ 上限按 3000000 对待。2026-10-05 用户定档 **921600**
  （固件 `firmware/Core/Src/board.c` 的 DL_BAUD + 桥/网页默认值一起切）。

所以凡是"拿一串波特率去开某条口"的工具（`bridge.probe` 扫档、`bridge.bench --serial`、
`bridge.main --serial`）都先过这里：**J-Link CDC 口上把超过该探针上限的档砍掉**。
要越过这道闸必须显式 `--force-baud`（比如又换了一颗上限未知的探针时）。

判定只用 pyserial 的 `list_ports`，查不到就**不拦**（不认识的线不该被误伤）；
认得出是 J-Link 但认不出是哪一颗时，按**最保守**的 230400 处理。
"""
from __future__ import annotations

J_LINK_VID_PID = "1366:0105"       # 旧探针：J-Link V9
J_LINK_VID_PID_NEW = "1366:1024"   # 新探针：J-Link OB-RA4M2-Full（车上那颗，COM27）

#: 旧探针的 CDC VCOM 实测上限：超过它打不开，而且会把探针打进 bootloader 态。
J_LINK_MAX_BAUD = 230400
#: 新探针（OB-RA4M2-Full）的 CDC VCOM 实测上限：3 Mbaud 档也是好的。
J_LINK_MAX_BAUD_NEW = 3000000

#: PID → 该颗探针的 CDC VCOM 上限。
J_LINK_MAX_BAUD_BY_PID = {
    "0105": J_LINK_MAX_BAUD,
    "1024": J_LINK_MAX_BAUD_NEW,
}

#: PID → 人话名字（警告里用，方便一眼看出是哪颗探针）。
J_LINK_NAME_BY_PID = {
    "0105": "J-Link V9",
    "1024": "J-Link OB-RA4M2-Full",
}

#: 设备描述里出现它就认为是 J-Link 的 CDC 面（pyserial 的 description 是 "JLink CDC UART Port"）。
J_LINK_HINT = "JLINK CDC"


def port_info(port: str, comports=None) -> tuple[str, str]:
    """查这条口在系统里的 `(hwid, description)`。

    查不到（口不在场 / 没装 pyserial / 名字对不上）返回两个空串 —— 调用方按"不认识的线"处理。
    测试用 `comports=` 注入假的口列表，不需要真硬件。
    """
    try:
        from serial.tools import list_ports
    except Exception:  # pragma: no cover - 环境问题，不是逻辑问题
        return "", ""
    src = list_ports.comports() if comports is None else comports
    want = str(port or "").strip().upper()
    if not want:
        return "", ""
    for p in src:
        if str(getattr(p, "device", "") or "").strip().upper() == want:
            return (str(getattr(p, "hwid", "") or ""),
                    str(getattr(p, "description", "") or ""))
    return "", ""


def jlink_pid(hwid: str) -> str:
    """从 hwid 里抠出 J-Link 的 PID（`"0105"` / `"1024"`）；不是 J-Link 或认不出哪颗时空串。

    `hwid` 认两种写法：pyserial 的 `USB VID:PID=1366:0105 SER=...`
    与 PnP 的 `USB\\VID_1366&PID_0105\\...`。
    """
    h = str(hwid or "").upper()
    for pid in J_LINK_MAX_BAUD_BY_PID:
        if ("VID:PID=1366:" + pid).upper() in h:
            return pid
        if ("VID_1366&PID_" + pid).upper() in h:
            return pid
    return ""


def _has_jlink_vid(hwid: str) -> bool:
    """hwid 里带 1366: 这个 VID 就算 J-Link —— 哪怕是不认识的型号（新探针换了型号也要拦住）。"""
    h = str(hwid or "").upper()
    return "VID:PID=1366:" in h or "VID_1366&PID_" in h


def is_jlink_cdc(hwid: str, description: str = "") -> bool:
    """这条口是不是 J-Link 的 CDC VCOM（任一颗型号都算）。

    `hwid` 认两种写法（见 `jlink_pid`）；`description` 里带 "JLink CDC" 也算。
    """
    h = str(hwid or "").upper()
    d = str(description or "").upper()
    return bool(jlink_pid(h)) or _has_jlink_vid(h) or J_LINK_HINT in d


def max_baud(hwid: str, description: str = "") -> int:
    """这条 J-Link CDC 口能开到多少：认得出是哪颗按那颗的上限，认不出就按最保守的 230400。"""
    pid = jlink_pid(str(hwid or "").upper())
    if pid:
        return J_LINK_MAX_BAUD_BY_PID.get(pid, J_LINK_MAX_BAUD)
    return J_LINK_MAX_BAUD


def jlink_name(hwid: str, description: str = "") -> str:
    """警告里用的探针名字；认不出型号就笼统叫 J-Link。"""
    pid = jlink_pid(str(hwid or "").upper())
    if pid:
        return J_LINK_NAME_BY_PID.get(pid, "J-Link")
    return "J-Link" + ("（型号认不出，按最保守的上限算）" if J_LINK_HINT in str(description or "").upper() else "")


def cap_bauds(port: str, bauds, force: bool = False, comports=None) -> tuple[list[int], str]:
    """扫档前过滤：J-Link CDC 口上只留 ≤ 该探针上限（`max_baud`）的档。

    返回 `(留下的档, 警告文字)`；非 J-Link 口（或名字查不到）原样返回、警告为空。
    全被砍光时留一档该探针的上限（它能跑的最高值），免得一次都不听。
    """
    vals = [int(b) for b in bauds]
    if force or not vals:
        return vals, ""
    hwid, desc = port_info(port, comports)
    if not is_jlink_cdc(hwid, desc):
        return vals, ""
    lim = max_baud(hwid, desc)
    kept = [b for b in vals if b <= lim]
    if len(kept) == len(vals):
        return vals, ""
    dropped = ", ".join(str(b) for b in vals if b > lim)
    if not kept:
        kept = [lim]
    warn = ("%s 是 %s 的 CDC VCOM：实测上限 %d，试 %s 打不开、还会有把它打进 bootloader 态的"
            "风险（只能物理拔插 USB 才能恢复）。已跳过这些档；确实要试请加 --force-baud。"
            % (port, jlink_name(hwid, desc), lim, dropped))
    return kept, warn


def check_baud(port: str, baud: int, force: bool = False, comports=None) -> str:
    """开这条口之前问一句合不合适：**空串 = 可以开**，非空 = 该拒绝的理由（给人看的整句）。"""
    if force:
        return ""
    hwid, desc = port_info(port, comports)
    if not is_jlink_cdc(hwid, desc):
        return ""
    lim = max_baud(hwid, desc)
    if int(baud) <= lim:
        return ""
    return ("%s 是 %s 的 CDC VCOM，上限实测 %d：用 %d 打不开，还会把探针打进 bootloader 态"
            "（只能物理拔插 USB 才能恢复）。请改用 --baud %d；确实要试请加 --force-baud。"
            % (port, jlink_name(hwid, desc), lim, int(baud), lim))
