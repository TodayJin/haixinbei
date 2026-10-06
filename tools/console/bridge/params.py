"""参数表元数据（上位机侧的"哪个 id 叫什么/单位/几位小数"）。

**单一真值来源是固件**：`firmware/App/Svc/host_link.c` 的 `SPECS[]` + `host_link.h` 的
`HP_*` 宏。这里只是给 UI 用的展示信息（key/label/group/unit/decimals/step/note）；
默认值/范围也照抄一份，用于"还没连上小车"时也能显示（`source="local"`）。

`tools/console/tests/test_params.py` 会直接解析 host_link.c/host_link.h，断言：
  * 两边的 id 集合完全一致（不多不少）；
  * default/min/max 逐项相同。
所以任何一边改了参数表而另一边没跟，测试就会红。

group 取值（冻结契约 docs/API.md §3.2）：
  速度环 / 角度环 / 循迹环 / 串级 / 机械/车辆 / 电源 / 任务
"""
from __future__ import annotations

GROUPS = ["速度环", "角度环", "循迹环", "串级", "机械/车辆", "电源", "任务"]

# (id, key, label, group, unit, default, min, max, decimals, step, note)
_SPECS = [
    # ---- 循迹环（重心 PID）----
    (0x0201, "line_kp", "循迹 Kp", "循迹环", "", 2700.0, 0.0, 5000.0, 1, 10, "循迹外环比例"),
    (0x0202, "line_ki", "循迹 Ki", "循迹环", "", 10.8, 0.0, 200.0, 2, 0.5, "循迹外环积分"),
    (0x0203, "line_kd", "串级 重心变化率→转率", "循迹环", "°/s per (格/s)", 36.0, 0.0, 1000.0, 1, 1,
     "外环微分（阻尼）项：重心每 1 格/s 的变化额外要求多少转率。没有它外环是零阻尼振荡"
     "（斜着摆车会一直摆，用户 m33254）。2026-10-06 真车在 0.5m 线上整定 12→36（阻尼要加到"
     "理论值的 3 倍才见效，因为 TRACE_FF_LPF 那道 2Hz 低通在 ~1Hz 处吃掉约 27° 相位）；"
     "48 以上限幅%rate 会从 2.3% 跳到 15~18%（D 项超出执行器能力）。老值 360 是旧量纲，别再用"),
    (0x0204, "line_ilimit", "循迹积分限幅", "循迹环", "", 50.0, 0.0, 10000.0, 1, 10, "误差量纲的积分上限"),
    (0x0301, "base_speed", "循迹基准速度", "循迹环", "刻度", 6480.0, 0.0, 7200.0, 0, 60,
     "PWM 满量程 7200 刻度；0 = 原地转圈（pivot）"),
    (0x0302, "base_speed_slow", "循迹慢速档", "循迹环", "刻度", 2500.0, 0.0, 7200.0, 0, 60,
     "静摩擦起步死区下沿约 2500~2900"),
    (0x0304, "task_speed", "任务基准速度", "任务", "刻度", 7000.0, 0.0, 7200.0, 0, 60,
     "比赛流程巡线用的基准速度（2026-10-06 按用户要求 6480 → 7000；不受存档影响）"),
    (0x0207, "pivot_kp", "原地转圈 Kp", "循迹环", "", 900.0, 0.0, 20000.0, 1, 10,
     "base=0（左右轮反向对转）时的循迹环比例"),
    (0x0208, "pivot_ki", "原地转圈 Ki", "循迹环", "", 0.0, 0.0, 200.0, 2, 0.5,
     "实测 0 最好：积分会被静摩擦喂成常驻推力"),
    (0x0209, "pivot_kd", "原地转圈 Kd", "循迹环", "", 405.0, 0.0, 20000.0, 1, 10,
     "原地转圈微分"),
    # ---- 速度环 ----
    (0x0601, "speed_kp", "速度环 Kp", "速度环", "", 20.0, 0.0, 2000.0, 1, 1, "轮速 → PWM"),
    (0x0602, "speed_ki", "速度环 Ki", "速度环", "", 2.0, 0.0, 200.0, 2, 0.5, "轮速积分"),
    (0x0603, "speed_kd", "速度环 Kd", "速度环", "", 0.0, 0.0, 500.0, 1, 1, "默认 0"),
    (0x0604, "speed_ilimit", "速度环积分限幅", "速度环", "", 3000.0, 0.0, 20000.0, 0, 100, ""),
    (0x0605, "speed_ff_slope", "速度前馈斜率", "速度环", "", 0.662, 0.0, 2.0, 3, 0.01,
     "台架实测拟合的摩擦前馈（PWM/(mm/s)），默认 = 固件 host_link.h 的 SPEED_FF_SLOPE"),
    (0x0606, "speed_ff_fric", "速度前馈截距", "速度环", "", 2327.0, 0.0, 5000.0, 1, 10,
     "静摩擦补偿（PWM 刻度），默认 = 固件 host_link.h 的 SPEED_FF_FRIC"),
    # ---- 角度环（陀螺）----
    (0x0101, "gyro_kp", "角度环 Kp", "角度环", "", 150.0, 0.0, 2000.0, 1, 10,
     "旧刻度换算值 720 会自激，实测选定 150"),
    (0x0102, "gyro_ki", "角度环 Ki", "角度环", "", 0.0, 0.0, 200.0, 2, 0.5,
     "默认 0：积分会被静摩擦喂成常驻推力"),
    (0x0103, "gyro_kd", "角度环 Kd", "角度环", "", 15.0, 0.0, 500.0, 1, 1, "≈Kp/10，压过冲"),
    (0x0104, "gyro_ilimit", "角度环积分限幅", "角度环", "", 1000.0, 0.0, 20000.0, 0, 100, ""),
    (0x0105, "turn_angle_offset", "转弯前馈角", "角度环", "°", 90.0, 1.0, 180.0, 1, 5,
     "Toggle_Gyro_Control 每次转的基准角"),
    (0x0401, "turn_tolerance", "转弯到位容差", "角度环", "°", 2.0, 0.1, 20.0, 1, 0.5, ""),
    (0x0402, "turn_timeout", "转弯超时", "角度环", "ms", 1500.0, 100.0, 10000.0, 0, 100, ""),
    (0x0403, "yaw_rate_damp", "航向速率阻尼", "角度环", "", 1.0, 0.0, 20.0, 1, 0.5, ""),
    (0x010A, "gyro_ang_kp", "角度环外环 Kp", "角度环", "(°/s)/°", 10.0, 1.0, 20.0, 2, 0.1,
     "角度误差 → 期望角速度（2026-10-06 原地转圈定稿 10：唯一能压到位时间的旋钮，到 90% 0.67s→0.50s）"),
    (0x010B, "gyro_rate_kp", "角度环速率环 Kp", "角度环", "", 40.0, 10.0, 2000.0, 0, 10,
     "角速度误差 → 修正量（2026-10-06 原地转圈定稿 40：内环 P 的 stick-slip 是到位后来回踢的根因）"),
    (0x010C, "gyro_rate_kd", "角度环速率环 Kd", "角度环", "", 0.0, 0.0, 100.0, 2, 0.5,
     "law3 的阻尼项（默认关：对陀螺做差分是抖动源）"),
    (0x010D, "gyro_rate_max", "期望角速度上限", "角度环", "°/s", 120.0, 10.0, 300.0, 0, 5, ""),
    (0x010E, "gyro_corr_limit", "修正量限幅", "角度环", "刻度", 3000.0, 200.0, 7200.0, 0, 100,
     "PWM 刻度；静摩擦起步需要 ~2000 以上"),
    (0x010F, "gyro_corr_slew", "修正量斜率限幅", "角度环", "刻度/s", 300000.0, 1000.0, 2000000.0, 0, 5000, ""),
    (0x0110, "gyro_settle_deg", "停车带半宽", "角度环", "°", 0.8, 0.2, 10.0, 1, 0.2,
     "2026-10-06 定 0.8：起转助推在 |err| < 它时不武装，收到 0.8 才补得上最后 1°，且窗 p-p 全 ≤3.0°"),
    (0x0111, "gyro_settle_ms", "到位锁存时长", "角度环", "ms", 150.0, 0.0, 2000.0, 0, 10, ""),
    (0x0112, "gyro_corr_ref", "差速随速放大参考", "角度环", "刻度", 3000.0, 0.0, 7200.0, 0, 100,
     "满速转不动就调它：差速天花板 = 修正量限幅 × max(1, 基准速度/本值)，0 = 不放大（旧行为）。"
     "★2026-10-06 试过 1500（cap 7000→14000）**被真车否掉**：cap 一放大，看见线时的环路"
     "就成继电器（用户 m「拐回来之后一直震荡在直线上」，corr 打到 ±14000、车速 20~250 抽）。"
     "直角要的「轮子变负」改走另一条路：进找线时把基准打成 0 ⇒ 天然对转（pivot 工况）"),
    # ---- 串级（循迹 → 角度 → 速度）----
    (0x0205, "trace_angle_per_unit", "串级 误差→转率", "串级", "(°/s)/格", 30.0, 0.5, 60.0, 1, 0.5,
     "重心偏 1 格要求多少转率（每拍把目标航向按它推一点；用户 m32880 改口径，老值不可当基准）"),
    (0x0206, "trace_angle_max", "串级 前置量上限", "串级", "°", 35.0, 1.0, 90.0, 0, 1,
     "目标航向相对当前航向的前置量上限（防积分饱和；≈ 传感器杆半宽。不再限制弯道总转角）"),
    (0x020A, "trace_rate_max", "串级 期望角速度上限", "串级", "°/s", 300.0, 10.0, 300.0, 0, 5,
     "**只在循迹时**生效的期望角速度上限。corr 上限 = 角度环速率环 Kp × 它 ⇒ 它决定"
     "「直角这种大转弯时内侧轮能不能变成负数」（原地转圈仍用角度环那个 120，不受影响）"),
    (0x020B, "trace_corner", "直角主动转弯", "串级", "0/1", 0.0, 0.0, 1.0, 0, 1,
     "★0 = 关（默认，纯循迹）；1 = 开（探头半边 4 路压线就一次把目标航向转 100°、原地对转、"
     "见线即停）。实现一行没删，只由这个开关挡；屏上「设置 → 开关」里也能改（存 NVRAM）。"),
    # ---- 机械/车辆 ----
    (0x0305, "counts_per_rev", "编码器每圈计数", "机械/车辆", "count", 1980.0, 1.0, 100000.0, 0, 10, ""),
    (0x0306, "wheel_dia", "轮径", "机械/车辆", "mm", 65.0, 10.0, 300.0, 1, 0.5, ""),
    (0x0307, "wheel_base", "轮距", "机械/车辆", "mm", 150.0, 10.0, 500.0, 1, 1, ""),
    (0x0303, "pwm_max", "PWM 满量程", "机械/车辆", "刻度", 7200.0, 100.0, 7200.0, 0, 100,
     "本板比旧板 ×7.2（旧板 1000）"),
    (0x0404, "marker_hold", "标记保持", "任务", "", 3.0, 1.0, 50.0, 1, 1, ""),
    (0x0405, "marker_min_gap", "标记最小间隔", "任务", "", 50.0, 0.0, 5000.0, 0, 10, ""),
    # ---- 电源 ----
    (0x0501, "vbat_nominal", "标称电压", "电源", "mV", 12000.0, 3000.0, 30000.0, 0, 100, ""),
    (0x0502, "stall_current", "堵转电流", "电源", "mA", 2500.0, 100.0, 20000.0, 0, 50, ""),
    (0x0503, "vbat_low", "低压报警", "电源", "mV", 10500.0, 3000.0, 30000.0, 0, 100, ""),
]


class ParamSpec:
    __slots__ = ("id", "key", "label", "group", "unit", "default", "min", "max",
                 "decimals", "step", "note")

    def __init__(self, id, key, label, group, unit, default, lo, hi, decimals, step, note):
        self.id = id
        self.key = key
        self.label = label
        self.group = group
        self.unit = unit
        self.default = default
        self.min = lo
        self.max = hi
        self.decimals = decimals
        self.step = step
        self.note = note

    def item(self, value: float | None = None, dirty: bool = False, source: str = "fw") -> dict:
        v = self.default if value is None else value
        return {
            "id": self.id, "key": self.key, "label": self.label, "group": self.group,
            "unit": self.unit, "value": v, "default": self.default,
            "min": self.min, "max": self.max, "decimals": self.decimals,
            "step": self.step, "dirty": bool(dirty), "note": self.note, "source": source,
        }


PARAMS = [ParamSpec(*s) for s in _SPECS]
BY_ID = {p.id: p for p in PARAMS}
BY_KEY = {p.key: p for p in PARAMS}


def by_id_or_key(x) -> ParamSpec | None:
    """`/api/params/set` 允许传 id 或 key。"""
    if isinstance(x, str):
        if x in BY_KEY:
            return BY_KEY[x]
        try:
            x = int(x, 0)
        except ValueError:
            return None
    if isinstance(x, bool):
        return None
    if isinstance(x, int):
        return BY_ID.get(x)
    return None


DEFAULTS = {p.id: p.default for p in PARAMS}


def clamp(p: ParamSpec, v: float) -> float:
    """夹到范围里；NaN 直接判非法（和固件一样用取反形式比较）。"""
    if not (v == v):                      # NaN
        raise ValueError("数值非法")
    if not (v >= p.min and v <= p.max):
        raise ValueError("超出范围：%s 允许 %g..%g" % (p.label, p.min, p.max))
    return float(v)

# ------------------------------------------------------------------ 动作表
# 名称与出厂角度是 firmware/App/Bsp/servo.c 的 k_action_name[] / k_action_default[][] 的镜像；
# tools/console/tests/test_params_spec.py 会解析那份固件源文件逐项比对，防止两张表漂移。
ACTION_NAMES = ["收回", "打下准备", "打下1", "打下2", "台阶2收回", "大坡1", "三级12",
                "三级2收回", "三级3打", "高台", "高台下", "收回2",
                "打下1(另一路)", "打下准备(另一路)",
                # ★2026-10-07 用户 m：固件动作表扩到 **20 个位置**（servo.h 的 SERVO_ACTION_N=20），
                #   15~20 是空位、角度由舵机页/任务编排下发；这份镜像必须跟着长，否则 /api/actions
                #   永远只报 14 个（车上明明是 20）—— 就是它让上位机看不到新增动作的。
                "动作15", "动作16", "动作17", "动作18", "动作19", "动作20"]
ACTION_FACTORY = [[0, 0], [60, 120], [105, 120], [135, 150], [80, 170], [100, 120],
                  [95, 120], [40, 10], [100, 80], [115, 120], [85, 120], [40, 0],
                  [90, 60], [30, 60],
                  [0, 0], [0, 0], [0, 0], [0, 0], [0, 0], [0, 0]]
ACTION_COUNT = len(ACTION_NAMES)
# 复合动作（固件 Servo_Move 里有中间延时步骤，play 会走完整过程）
ACTION_COMPOUND = {
    5: "135/170 →0.4s→ 90 →0.1s→ 80",
    9: "70/10 →0.2s→ 100/50 →0.1s→ 100/80",
}


def _clamp_deg(v) -> int:
    """舵机角度一律夹到 0..180 —— 和固件 Servo_SetActionPose() 的夹法一致。

    页面上是 0..180 的滑杆，车上万一因为 Flash 损坏报出 999/-5，夹一下比原样显示安全；
    夹过之后仍然参与 dirty 比较（也就是"和出厂值/本机预设不一样"照样能看出来）。
    """
    try:
        v = int(v)
    except (TypeError, ValueError):
        return 0
    return 0 if v < 0 else (180 if v > 180 else v)


def action_items(values, preset=None) -> list[dict]:
    """把车上读到的 14 对角度拼成契约里的动作项。

    `values`/`preset` 都是 14 个 `[a1, a2]`；`preset` 缺省 = 固件出厂表。
    `dirty` = 车上值与本机预设（或出厂值）不同，也就是"改了还没固化"。
    """
    src = preset or ACTION_FACTORY
    out = []
    for i in range(ACTION_COUNT):
        n = i + 1
        try:
            a1, a2 = _clamp_deg(values[i][0]), _clamp_deg(values[i][1])
        except (IndexError, TypeError):
            a1, a2 = int(ACTION_FACTORY[i][0]), int(ACTION_FACTORY[i][1])
        try:
            p1, p2 = _clamp_deg(src[i][0]), _clamp_deg(src[i][1])
        except (IndexError, TypeError):
            p1, p2 = int(ACTION_FACTORY[i][0]), int(ACTION_FACTORY[i][1])
        out.append({"n": n, "name": ACTION_NAMES[i], "a1": a1, "a2": a2,
                    "preset_a1": p1, "preset_a2": p2,
                    "dirty": (a1 != p1 or a2 != p2),
                    "compound": n in ACTION_COMPOUND,
                    "steps": ACTION_COMPOUND.get(n)})
    return out
