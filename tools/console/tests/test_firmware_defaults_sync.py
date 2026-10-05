"""钉住出厂默认值的**第三处**：`app_ui.c` 的 `g_cfg` 镜像 + `pull_pid_params()` 兜底。

为什么需要这条（2026-10-06 真车抓到的 bug）：
  `HP_LINE_KD` 的默认值 2026-10-05 从 360.0（旧量纲 ×7.2）改成 12.0（串级外环的阻尼项），
  同步改了 `host_link.c` 的 SPECS[] 和上位机镜像 `bridge/params.py`，
  **但漏改了 `firmware/App/Src/app_ui.c` 的 `g_cfg.line_kd_x100 = 36000` 与
  `pull_pid_params()` 的兜底 `360.0f`**。

  而 `g_cfg` 是**存在 NVRAM 里、开机由 `push_pid_params()` 盖回参数表**的那一份
  （`app_ui.c:444-463`、`nvram.c` 的 snapshot/apply）⇒ 参数表里 SPECS 的 12.0 被 360.0 覆盖，
  车上真正跑的 D 项一直是 12 的 30 倍。实测：上位机 `POST /api/params/reload` 读回
  `line_kd = 360.0`，而 SPECS / params.py 都写着 12.0。

  `test_params.py` 已经钉住「host_link.c SPECS[] ↔ params.py」两处；本文件补第三处，
  不然下次改默认值还会漏，而且漏了之后**只有真车读回才能发现**（车上跑的是存档，
  源码怎么改都不报错）。
"""
from __future__ import annotations

import re
from pathlib import Path

from bridge import params as P

ROOT = Path(__file__).resolve().parents[3]
APP_UI_C = ROOT / "firmware" / "App" / "Src" / "app_ui.c"

# g_cfg 字段 -> (参数表 key, 量化倍数)。与 app_ui.c 的 push_pid_params()/pull_pid_params() 一一对应。
SYNC = [
    ("trace_speed",          "base_speed",      1.0),
    ("speed_kp_x100",        "speed_kp",        100.0),
    ("speed_ki_x100",        "speed_ki",        100.0),
    ("speed_kd_x100",        "speed_kd",        100.0),
    ("speed_ilim",           "speed_ilimit",    1.0),
    ("speed_ff_slope_x1000", "speed_ff_slope",  1000.0),
    ("speed_ff_fric",        "speed_ff_fric",   1.0),
    ("line_kp_x100",         "line_kp",         100.0),
    ("line_ki_x100",         "line_ki",         100.0),
    ("line_kd_x100",         "line_kd",         100.0),
    ("line_ilim",            "line_ilimit",     1.0),
    ("gyro_kp_x100",         "gyro_kp",         100.0),
    ("gyro_ki_x100",         "gyro_ki",         100.0),
    ("gyro_kd_x100",         "gyro_kd",         100.0),
    ("gyro_ilim",            "gyro_ilimit",     1.0),
]


def _src() -> str:
    assert APP_UI_C.is_file(), "找不到 %s" % APP_UI_C
    # 源文件里有中文注释，按 utf-8 读；读不动就直接失败（别静默 skip）
    return APP_UI_C.read_text(encoding="utf-8")


def _block(text: str, start_pat: str) -> str:
    """从 start_pat 匹配处切到该语句块结束的 `\\n}`（够这段代码用，都是简单的函数体/初始化器）。"""
    m = re.search(start_pat, text)
    assert m, "app_ui.c 里找不到 %r —— 结构改名了就得同步改本测试" % start_pat
    tail = text[m.end():]
    end = re.search(r"\n\}", tail)
    assert end, "从 %r 往后找不到块的结尾 `}`" % start_pat
    return tail[: end.start()]


def test_g_cfg_defaults_match_the_param_table():
    """g_cfg 的出厂默认（×100/×1000 量化后）必须等于参数表默认值。"""
    block = _block(_src(), r"app_settings_t\s+g_cfg\s*=\s*\{")
    got = {k: int(v) for k, v in re.findall(r"\.(\w+)\s*=\s*(-?\d+)", block)}
    bad = []
    for field, key, scale in SYNC:
        assert field in got, "g_cfg 里没有字段 %s（本测试的 SYNC 表要跟着改）" % field
        want = P.BY_KEY[key].default
        mine = got[field] / scale
        if abs(mine - want) > 1e-6:
            bad.append("%s: g_cfg 给了 %g（%d/%g），参数表默认是 %g"
                       % (key, mine, got[field], scale, want))
    assert not bad, "app_ui.c 的 g_cfg 默认值与参数表不一致：\n  " + "\n  ".join(bad)


def test_pull_pid_params_fallbacks_match_the_param_table():
    """pull_pid_params() 里的 `host_param_get/param_to_x100(HP_X, 兜底)` 也必须一致。"""
    block = _block(_src(), r"static\s+void\s+pull_pid_params\s*\(\s*void\s*\)\s*\{")
    got = {}
    for fn, hp, val in re.findall(
            r"(param_to_x1000|param_to_x100|host_param_get)\s*\(\s*(HP_\w+)\s*,\s*"
            r"([-+]?[0-9]*\.?[0-9]+)f?\s*\)", block):
        got[hp] = float(val)
    assert got, "pull_pid_params() 里一条兜底都没解析到 —— 结构改名了就得同步改本测试"

    # HP_LINE_KD -> 'line_kd' 这类映射直接查上位机表（params.py 的 key 与固件宏同名不同大小写）
    bad = []
    for hp, val in got.items():
        key = hp[3:].lower()                      # 去掉 'HP_'，转小写
        spec = P.BY_KEY.get(key)
        assert spec is not None, "参数表里没有 %s（%s）" % (key, hp)
        if abs(val - spec.default) > 1e-6:
            bad.append("%s: pull 兜底 %g，参数表默认 %g" % (key, val, spec.default))
    assert not bad, "pull_pid_params() 的兜底值与参数表不一致：\n  " + "\n  ".join(bad)


def test_push_covers_exactly_the_fields_this_test_watches():
    """push_pid_params() 往参数表写的字段集合，必须正好是 SYNC 覆盖的那些。

    防的是"新加一个被 push 的参数，却没人把它加进同步断言" —— 那正是 line_kd 当初的漏法。
    """
    block = _block(_src(), r"static\s+void\s+push_pid_params\s*\(\s*void\s*\)\s*\{")
    pushed = set(re.findall(r"host_param_set_local\(\s*(HP_\w+)", block))
    assert pushed, "push_pid_params() 里没解析到 host_param_set_local()"
    keys = set()
    for hp in pushed:
        key = hp[3:].lower()
        assert key in P.BY_KEY, "参数表里没有 %s（%s）" % (key, hp)
        keys.add(key)
    watched = {key for _, key, _ in SYNC}
    missing = keys - watched
    assert not missing, (
        "push_pid_params() 会覆盖这些参数，但本测试的 SYNC 表没盯着它们：%s\n"
        "（它们同样是「存档盖掉出厂默认」的那一类，必须一起断言）" % sorted(missing))
