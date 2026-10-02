# tools/ — 一次性/辅助脚本

放**不复用**的提取、渲染、计算脚本。产物一律写到 `out/`，不要写回仓库各处。

数据手册在 `docs/datasheets/`，脚本里的路径都是相对**仓库根**算出来的，
从任何目录运行都可以：`python tools/xxx.py`。

| 脚本 | 用途 |
|---|---|
| `extract_pins.py` | 从 `stm32g474re.pdf` 提 PA0–PA7 的引脚/复用/电平（DS12288 表） |
| `extract_ssd.py` | 从 `SSD1315.pdf` 提命令表 / 电荷泵 / 对比度相关页 |
| `extract_gme.py` | 从 `GME12864-41.pdf` 提引脚表 / 初始化代码 / 参考电路页 |
| `render_gme.py` | 把 `GME12864-41.pdf` 参考电路页渲染成 PNG 到 `out/`（看接法用） |
| `i2c_bus.py` | I2C 上拉/总线电容的 RC 计算（tr≈0.847·Rp·Cb），当前取 4.7k |

依赖：`pip install pymupdf`（`fitz`）。
