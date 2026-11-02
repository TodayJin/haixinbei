# 独立上位机包（portable/host）

这是**可以单独拷走运行**的上位机：Python 桥 + 已构建的网页界面 + 启动脚本，不需要仓库里的
固件源码，也不需要 Node/npm。完整使用说明在根目录手册
[《上位机介绍手册》](../../上位机介绍手册.md)，本文件只讲"这个包怎么跑、里面有什么"。

> ⚠ **这是生成物**：由 `project/tools/pack_portable.py` 从 `project/tools/console/` 打出。
> 除本 `README.md` 外，不要直接改包里的文件 —— 改 `project/` 下的源，再重跑脚本。
> 重打包时脚本会**保留**这个 README（`_keep_readme`），所以这里的说明不会丢。

## 1. 最快跑起来

1. 装 **Python 3.11+**，再装串口库：`python -m pip install pyserial`。
2. 双击下列之一（Windows）：

   | 双击 | 作用 |
   |---|---|
   | `启动上位机.bat` | 真车：自动找车的串口，921600 |
   | `启动上位机-仿真.bat` | 无车也能开界面（内置仿真车） |
   | `启动上位机-列串口.bat` | 只列出本机串口，看车在哪个 COM |

3. 浏览器会自动打开 **http://127.0.0.1:8770/**（桥只监听本机）。

不想双击时，在本目录里用命令行（**必须在包根目录，用 `-m`**）：

```bash
cd portable/host
python -m bridge.main --sim                  # 仿真
python -m bridge.main --serial COM27         # 真车；--port 是 HTTP 端口、不是串口
python -m bridge.main --list-ports
python -m bridge.main --help
```

> 不要 `python bridge/main.py`：它是包内模块（相对导入），当脚本跑会 ImportError。

连车、开波形、调参、任务编排的完整流程见根手册 §2～§4。第一次连真车请**先架空轮子**；
空格 / Esc 是急停。

## 2. 包里有什么

```
portable/host/
├─ 启动上位机.bat / -仿真.bat / -列串口.bat   双击入口
├─ launch.ps1          启动逻辑（找 Python/pyserial、查 8770 端口、试开串口、起桥）
├─ bridge/             Python 桥（只依赖标准库 + pyserial）
│  ├─ main.py          入口
│  ├─ server.py        HTTP + SSE（托管 web/dist）
│  ├─ state.py         参数/动作/任务/驾驶/日志状态机
│  ├─ protocol.py      帧编解码（与车上固件 host_link.c 对应）
│  ├─ params.py        58 项参数 / 20 个动作的展示信息
│  ├─ sim.py           内置仿真车（同协议，无硬件可用）
│  ├─ serial_safety.py J-Link 探针的波特率安全闸
│  └─ probe.py / bench.py / taskbackup.py   链路体检 / 承载测量 / 写车前备份
├─ web/dist/           已构建的前端（桥直接托管，共 3 个文件）
├─ docs/API.md         网页 ↔ 桥的接口契约
├─ tests/              pytest（不含依赖固件源码的那些，见下）
└─ data/               运行期数据（首次启动自动创建）
   ├─ presets/         本机预设（参数/动作/舵机）
   └─ logs/            桥日志 console_YYYYMMDD.log + 请求日志 requests.log
```

## 3. 数据、日志与备份

- **本机预设**：本目录 `data/presets/*.json`（设置页/舵机页保存的）。
- **桥日志**：本目录 `data/logs/console_YYYYMMDD.log`；网页请求日志 `data/logs/requests.log`
  （超过 2 MB 自动清空，排查"点了没发出去"时看它）。
- **任务备份**：先在本目录跑 `python -m bridge.taskbackup`。它**只读**（GET 目录 + 逐槽读镜像），
  默认写到"本包往上三级"的 `out/taskbackup/` —— 包放在仓库里时就是仓库根 `out/taskbackup/`；
  放在别处时就是包上级目录的 `out/taskbackup/`。想换位置用 `--out D:\bak\tasks`。
  **往车上写任务之前先跑一次**（历史上有过参数填错、把车上目录抹掉的事故）。
- **车上持久化**在车自己 Flash 里（参数/动作/标定最后一页，任务页 58～61），换电脑不影响。

## 4. 测试

```bash
cd portable/host
python -m pytest -q          # 当前：148 passed, 8 skipped
```

- 8 条 skip 是"用本机 gcc 编固件源码对拍"的用例，没装 gcc 时自动跳过，不是失败。
- 相比完整工程，本包**有意不带** 6 个依赖 `project/firmware` 或 `web/src` 金样本的测试文件
  （`pack_portable.py` 的 `HOST_DROP_TESTS`），所以数目比仓库里少。
- 完整工程的测试现状与已知 3 条红见根手册 §8.1。

## 5. 文档

| 想了解 | 看哪里 |
|---|---|
| 上位机完整使用说明（10 个页面、工作流、故障排查、二次开发） | [根手册《上位机介绍手册》](../../上位机介绍手册.md) |
| 上位机 ↔ 车通信协议（帧/遥测/参数/任务帧） | [../docs/上位机通信协议.md](../docs/上位机通信协议.md) |
| 网页 ↔ 桥的 HTTP/SSE 接口契约 | [docs/API.md](docs/API.md) |
| 任务块的格式与含义 | [../docs/任务编程.md](../docs/任务编程.md)、[../docs/任务编排模块与逻辑.md](../docs/任务编排模块与逻辑.md) |
| 把固件移植到别的板子 | [../移植手册.md](../移植手册.md) |
| 仓库总览与目录结构 | [../../项目总览.md](../../项目总览.md) |

## 6. 已知限制（摘要）

- 只监听 `127.0.0.1`，不支持远程/联网控制。
- 没有录制回放、没有赛道地图仿真；只有真车 / 内置仿真两种链路。
- 旧 J-Link V9（PID 0105）CDC 上限 230400，网页上选更高会被闸拦住；硬闯会把探针打进
  bootloader 态、只能物理拔插 USB 恢复。
- 参数「恢复默认」用的是参数表默认值，与车上 `g_cfg` 有 8 项不一致（详见根手册 §8.2）。
- 急停走前端串行写队列，链路丢 ACK 时可能延迟 1～2 s；真紧急请断电/关车。
- 环境要求：Windows + Python 3.11+ + pyserial；别的系统能跑桥（串口是 `/dev/ttyUSB*`），
  但没有那三个 `.bat`，要自己 `python -m bridge.main`。
