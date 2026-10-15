# 上位机整定工具（串口）

这些脚本通过 COM4（DAPLink CDC，921600）与固件通信，用于整定三环参数。

| 工具 | 用途 |
|---|---|
| `param.py` | **参数读写（唯一入口）**：ASCII 输出 + 逐条回读确认。`get` / `set k=v ...` / `save` |
| `linelog.py` | **循迹诊断**：边驱动边记录遥测里的 gray[8]/trace.pos/lost/corr/rate_ref ⇒ CSV + 摘要 |
| `trace.py` | 逐帧采集（带符号 vl/vr/pwm/vbat/ibat）到 CSV；`--mode trace --a <基准>` 跑循迹 |
| `linechk.py` | 灰度标定表读取（注意：T_CAL_REPORT 报的是**标定表**，实时值要看遥测） |
| `angchk.py` | 角度环直查（a=0 只让角度环出力，量实际转角与修正量） |
| `gyrochk.py` | 陀螺数据流验证 |
| `probe.py` | 读固件文本探针（`[fw] sp en=... reqL/R outL/R loadA/B regL/R`） |

## 注意
- 上位机（GUI）开着会占 COM4；测前先停掉监听 8770 的进程与 python 进程（**按进程名杀**）
- 这颗 DAPLink 的 CDC 与 SWD 同芯片：**用过串口后 SWD 必卡**，只能整机断电恢复
  ⇒ 顺序必须是「断电 → 先烧录 → 之后只用串口」
- 参数有范围限制，被拒时 param.py 会显示 `[FAIL ...]`；改完要 `save` 固化

## 三环最终参数（2026-10-15 实测标定）
- 速度环：full_counts=26、ff_slope=0.314、ff_fric=4558、kp=40、ki=0、kd=0
- 角度环：ang_kp=6、rate_kp=10、rate_kd=0、rate_max=300、corr_limit=5000、corr_ref=3000
- 循迹环：line_kp=2700、line_ki=10.8、**line_kd=25**、line_ilimit=50、trace_rate_max=300
- 比赛推荐基准速度 **6500**（丢线 0.5%、跟随比 0.95）
