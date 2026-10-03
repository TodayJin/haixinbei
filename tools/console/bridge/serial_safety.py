"""串口安全闸：别为了试一个波特率把 J-Link 探针再打死一次。

2026-10-03 实测（J-Link V9，DP 复合设备 `USB\\VID_1366&PID_0105\\000602711039` 的 MI_00 面
= 「JLink CDC UART Port」）：

- **115200 / 230400**：能打开、能读到字节（230400 那档读到 2791 字节，正是 115200 的约 2 倍）；
- **460800**：`OSError(22, '信号灯超时时间已到')` = ERROR_SEM_TIMEOUT(121)，卡在 SET_LINE_CODING；
- **921600**：`PermissionError(13, '连到系统上的设备没有发挥作用。')` = ERROR_GEN_FAILURE(31)。

更麻烦的是后果：**只要对它试 ≥460800，探针的 USB 栈就崩进 PID_0101 bootloader 态**
（`J-Link driver`，Status OK 但 JLink.exe 报 `Could not read J-Link capabilities` /
`Cannot connect to J-Link`）。软件救不回来 —— `pnputil /restart-device` 跑完连 VID_1366
都从设备树里消失，**只能物理拔插 USB**（拔掉等 ~10s 再插；若回来时报问题码 22
= CM_PROB_DISABLED，再 `Enable-PnpDevice` 一次）。

所以凡是"拿一串波特率去开某条口"的工具（`bridge.probe` 扫档、`bridge.bench --serial`、
`bridge.main --serial`）都先过这里：**J-Link CDC 口上把 > 230400 的档砍掉**。
要越过这道闸必须显式 `--force-baud`（比如换了一颗真支持 921600 的探针/转接板时）。

判定只用 pyserial 的 `list_ports`，查不到就**不拦**（不认识的线不该被误伤）。
"""
from __future__ import annotations

J_LINK_VID_PID = "1366:0105"

#: 这颗 J-Link 的 CDC VCOM 上限（实测）；超过它打不开，且会把探针打进 bootloader 态。
J_LINK_MAX_BAUD = 230400

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


def is_jlink_cdc(hwid: str, description: str = "") -> bool:
    """这条口是不是 J-Link 的 CDC VCOM。

    `hwid` 认两种写法：pyserial 的 `USB VID:PID=1366:0105 SER=...`
    与 PnP 的 `USB\\VID_1366&PID_0105\\...`；`description` 里带 "JLink CDC" 也算。
    """
    h = str(hwid or "").upper()
    d = str(description or "").upper()
    pyserial_style = ("VID:PID=" + J_LINK_VID_PID).upper() in h
    pnp_style = ("VID_" + J_LINK_VID_PID.replace(":", "&PID_")).upper() in h
    return pyserial_style or pnp_style or J_LINK_HINT in d


def cap_bauds(port: str, bauds, force: bool = False, comports=None) -> tuple[list[int], str]:
    """扫档前过滤：J-Link CDC 口上只留 ≤ `J_LINK_MAX_BAUD` 的档。

    返回 `(留下的档, 警告文字)`；非 J-Link 口（或名字查不到）原样返回、警告为空。
    全被砍光时留一档 `J_LINK_MAX_BAUD`（那颗探针能跑的最高值），免得一次都不听。
    """
    vals = [int(b) for b in bauds]
    if force or not vals:
        return vals, ""
    hwid, desc = port_info(port, comports)
    if not is_jlink_cdc(hwid, desc):
        return vals, ""
    kept = [b for b in vals if b <= J_LINK_MAX_BAUD]
    if len(kept) == len(vals):
        return vals, ""
    dropped = ", ".join(str(b) for b in vals if b > J_LINK_MAX_BAUD)
    if not kept:
        kept = [J_LINK_MAX_BAUD]
    warn = ("%s 是 J-Link 的 CDC VCOM：实测上限 %d，试 %s 打不开、还会把探针打进 bootloader 态"
            "（只能物理拔插 USB 才能恢复）。已跳过这些档；确实要试请加 --force-baud。"
            % (port, J_LINK_MAX_BAUD, dropped))
    return kept, warn


def check_baud(port: str, baud: int, force: bool = False, comports=None) -> str:
    """开这条口之前问一句合不合适：**空串 = 可以开**，非空 = 该拒绝的理由（给人看的整句）。"""
    if force:
        return ""
    hwid, desc = port_info(port, comports)
    if not is_jlink_cdc(hwid, desc):
        return ""
    if int(baud) <= J_LINK_MAX_BAUD:
        return ""
    return ("%s 是 J-Link 的 CDC VCOM，上限实测 %d：用 %d 打不开（ERROR_GEN_FAILURE），"
            "还会把探针打进 bootloader 态（只能物理拔插 USB 才能恢复）。"
            "请改用 --baud %d；确实要试请加 --force-baud。"
            % (port, J_LINK_MAX_BAUD, int(baud), J_LINK_MAX_BAUD))
