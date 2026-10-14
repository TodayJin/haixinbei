"""审计修复的回归测试（纯单元，不起串口/不起 HTTP）。

覆盖：
  · #23 预设名白名单真的被调用（不是死代码）—— 路径穿越必须被拒
  · #24 param_set 越界/NaN/非数字 ⇒ 返回 ok:false + 人话，**绝不抛异常**（否则 = HTTP 500）
  · #25 task_get 的"明确拒绝"要能立刻结束等待（不等满超时）
  · #30 速度环满刻度计数已是参数（换电机不用重烧）
"""
from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from bridge import params as P                      # noqa: E402
from bridge.state import Console, preset_name_ok    # noqa: E402


def _bare_console() -> Console:
    """不走 __init__（那会真开串口），只装出被这几个函数用到的字段。"""
    c = Console.__new__(Console)
    c.params = [{"id": sp.id, "key": sp.key, "value": sp.default, "dirty": False,
                 "label": sp.label} for sp in P.PARAMS]
    c.car_values = {}
    c.lock = threading.RLock()
    c.cv = threading.Condition(c.lock)
    c.preset_dir = str(ROOT / "data" / "presets")
    # _cap_guard() 会读 hello（HELLO 握手结果）；这些单元测试不握手 ⇒ 置 None
    c.hello = None
    c.status = {}
    # emit() 要用的（不真开 SSE，给个空订阅者列表）
    c.subs = []
    c.logs = []
    return c


# ---------------------------------------------------------------- #23 预设名
@pytest.mark.parametrize("name", ["../../../../tmp/pwn", "a/b", "..", ".hidden",
                                  "a\\b", "x" * 65, "", "  "])
def test_preset_name_rejects_traversal(name):
    assert preset_name_ok(name) is False, f"{name!r} 不该通过白名单"


@pytest.mark.parametrize("name", ["ab", "我的预设", "a b-1.2(3)", "preset_01"])
def test_preset_name_accepts_normal(name):
    assert preset_name_ok(name) is True


def test_preset_name_is_actually_enforced_not_dead_code(tmp_path):
    """★回归：`preset_name_ok` 以前**定义了但全工程没调用**（死代码）⇒ 路径穿越成立。
    这里断言 save/delete/apply 三个入口都真的拒了。"""
    c = _bare_console()
    c.preset_dir = str(tmp_path)
    c.logs = []
    c.emit = lambda *a, **k: None
    c.log = lambda *a, **k: None

    evil = "../../../../tmp/pwn"
    for fn, args in ((c.preset_save, (evil, "params")),
                     (c.preset_delete, (evil,)),
                     (c.preset_apply, (evil,))):
        r = fn(*args)
        assert r.get("ok") is False, f"{fn.__name__} 竟然接受了 {evil!r}"
        assert "不合法" in r.get("error", ""), r
    # 真文件也没被创建到外面
    assert not (tmp_path.parent.parent / "tmp" / "pwn.json").exists()


# ---------------------------------------------------------------- #24 clamp
@pytest.mark.parametrize("bad", [999999.0, -1.0, float("nan"), float("inf"), None, "abc"])
def test_param_set_never_raises(bad):
    """★回归：越界值原来用 ValueError 表达 ⇒ 异常冒到 HTTP 层变成 500 带 Python 异常名。
    现在必须返回 ok:false + 人话。"""
    c = _bare_console()
    r = c.param_set("speed_kp", bad)          # 不许抛
    assert r.get("ok") is False, r
    assert isinstance(r.get("error"), str) and r["error"], r


def test_param_set_ok_path_still_works():
    c = _bare_console()
    r = c.param_set("speed_kp", 100.0)
    assert r.get("ok") is True, r
    it = next(i for i in c.params if i["key"] == "speed_kp")
    assert it["value"] == 100.0


# ---------------------------------------------------------------- #25 task_get
def test_task_get_returns_immediately_on_clear_refusal():
    """★回归：车对空槽**立刻**回 ACK(ST_NOT_READY)，但原来只等"数据收尾帧"
    ⇒ 白等满超时并重发 3 次（约 6 秒）。现在明确拒绝要能立刻结束等待。"""
    c = _bare_console()
    c._task_data_chunks = []
    c._task_data_done = False
    c._task_data_id = 0xFFFF
    c._task_get_status = None
    c._cap_guard = lambda: None
    c.send = lambda *a, **k: 0
    c._sent_name = {}
    c._sent_type = {}

    def fake_send(*a, **k):
        # 模拟车立刻回一个"空槽"的 ACK（ST_NOT_READY=0x05）。
        # ★必须在 self.cv 里赋值并 notify_all —— on_frame 的真实实现就是这么做的
        #   （state.py 的 T_ACK 分支：with self.cv: self._task_get_status = st; self.cv.notify_all()）。
        #   只赋值不 notify，等待方要等满超时才醒 —— 那测的是测试自己写错，不是修复。
        def setref():
            with c.cv:
                c._task_get_status = 0x05
                c.cv.notify_all()
        threading.Timer(0.05, setref).start()
        return 0
    c.send = fake_send

    t0 = time.time()
    r = c.task_get(7, timeout=2.0)
    dt = time.time() - t0
    assert r.get("ok") is False, r
    assert "没有任务" in r.get("error", ""), r
    assert dt < 1.0, f'等了 {dt:.2f}s —— 说明「明确拒绝」又被当成超时了'


# ---------------------------------------------------------------- #30 新参数
def test_speed_full_counts_param_exists():
    sp = P.by_id_or_key("speed_full_counts")
    assert sp is not None, "HP_SPEED_FULL_COUNTS / speed_full_counts 没进参数表"
    assert sp.id == 0x0308
    assert sp.default == 25.0        # 与固件 SPECS 一致（test_params.py 也会逐项核对）
    assert sp.min == 1.0 and sp.max == 5000.0


# ---------------------------------------------------------------- #11 nvram 3 态
def test_nvram_state_matches_firmware():
    """★回归：上位机原来按 5 态解释 nvram（1 待保存 / 2 保存中 / 3 已保存 / 4 失败），
    而固件（nvram.h:49-51）只有 3 态（0 IDLE / 1 BUSY / 2 DONE）且**没有失败态**
    ⇒ 固件报 2（写成功）时界面显示"保存中"、徽章永远不变绿，"已保存"从来不会出现。

    这里像 test_params.py 那样**直接解析固件源码**，钉住"两边一致"，防止以后再漂。
    """
    import re
    from pathlib import Path as _P
    fw = _P(__file__).resolve().parents[3] / 'firmware' / 'App' / 'Svc' / 'nvram.h'
    assert fw.is_file(), f'找不到固件头文件 {fw}'
    src = fw.read_text(encoding='utf-8', errors='replace')

    ids = set()
    for m in re.finditer(r'#define\s+NVRAM_STATE_(\w+)\s+(\d+)U', src):
        ids.add(int(m.group(2)))
    assert ids == {0, 1, 2}, f'固件的 nvram 状态集合变了：{sorted(ids)}（本测试按 3 态钉住）'

    # 上位机的两张表都必须是同一组 key
    from bridge import protocol as pr
    from bridge.state import NVRAM_NAMES
    assert set(pr.NVRAM_NAMES) == ids, f'protocol.NVRAM_NAMES={sorted(pr.NVRAM_NAMES)} vs 固件 {sorted(ids)}'
    assert set(NVRAM_NAMES) == ids, f'state.NVRAM_NAMES={sorted(NVRAM_NAMES)} vs 固件 {sorted(ids)}'
    assert '失败' not in ''.join(NVRAM_NAMES.values()), '固件没有失败态，界面不该承诺它'

    # 前端那张表也要对齐（直接读源文件，不需要跑 tsc）
    ts = _P(__file__).resolve().parents[1] / 'web' / 'src' / 'lib' / 'types.ts'
    tsrc = ts.read_text(encoding='utf-8', errors='replace')
    blk = re.search(r'NVRAM_TEXT[^=]*=\s*\{(.*?)\}', tsrc, re.S)
    assert blk, '没找到 NVRAM_TEXT'
    keys = {int(x) for x in re.findall(r'^\s*(\d)\s*:', blk.group(1), re.M)}
    assert keys == ids, f'types.ts 的 NVRAM_TEXT key={sorted(keys)} vs 固件 {sorted(ids)}'


def test_sim_nvram_matches_firmware_states():
    """模拟器的 nvram 状态机也必须走固件那 3 个值（否则 --sim 下前端看不到"已保存"）。"""
    import re
    from pathlib import Path as _P
    sim = (_P(__file__).resolve().parents[1] / 'bridge' / 'sim.py').read_text(encoding='utf-8', errors='replace')
    vals = {int(v) for v in re.findall(r'self\.nvram\s*=\s*(\d)', sim)}
    assert vals <= {0, 1, 2}, f'sim.py 里出现了固件没有的 nvram 值：{sorted(vals - {0, 1, 2})}'
    assert vals == {0, 1, 2}, f'sim.py 没覆盖全部 3 态：{sorted(vals)}'
