"""车上任务程序（0x60..0x69）：帧层 ↔ 仿真车 ↔ 桥端点 三层对拍。

契约：`docs/任务编程.md` §5（帧表）与 `tools/console/docs/API.md` §3.6（HTTP）。
权威字节串是 `fixtures/task.golden.json`（真固件格式层编出来的，见 test_task_bytes.py），
这个文件只负责"桥这一侧说得和写的那一版一样"：
  1. 帧层：载荷布局必须逐字节等于文档（手写期望值，不复用被测代码）；
  2. 仿真车：把金样本分片写进去 → 提交 → 列目录 → 读回，必须**逐字节**一致；
  3. HTTP：8 个端点的形状、以及"语义性失败必须 200 + ok:false"（前端把 404/501 当"还没支持"）。
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request

import pytest

from bridge import protocol as pr
from bridge.sim import SimCar

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN_PATH = os.path.join(HERE, "fixtures", "task.golden.json")


def golden() -> dict:
    with open(GOLDEN_PATH, encoding="utf-8") as f:
        return json.load(f)


def golden_image() -> bytes:
    return bytes.fromhex(golden()["hex"])


# ---------------------------------------------------------------- 工具

def car_req(car: SimCar, ftype: int, payload: bytes = b"", seq: int = 1,
            now: int = 1000) -> list[pr.Frame]:
    car.send(pr.build_frame(ftype, seq, payload))
    return car.poll(now)


def ack_status(frames: list[pr.Frame]) -> int:
    hit = [f for f in frames if f.type == pr.T_ACK]
    assert hit, f"没收到 ACK（收到 {[pr.TYPE_NAMES.get(f.type) for f in frames]}）"
    return pr.decode_ack(hit[-1].payload)["status"]


def http(url: str, method: str = "GET", body: dict | None = None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method,
                                headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"raw": raw}


def frame_parts(raw: bytes) -> tuple[int, int, bytes]:
    """从组好的整帧里取 `(type, seq, payload)`：`A5 5A | type | seq | len | payload | crc16`。"""
    assert raw[0] == pr.SOF0 and raw[1] == pr.SOF1, f"帧头不对：{raw[:2].hex()}"
    return raw[2], raw[3], raw[5:5 + raw[4]]


# ---------------------------------------------------------------- 1. 帧层

def test_task_frame_ids_are_the_reserved_block():
    """§5：0x60..0x69 十个帧号整块归任务，且都在 TYPE_NAMES 里（否则日志只剩数字）。"""
    ids = [pr.T_TASK_LIST, pr.T_TASK_LIST_R, pr.T_TASK_PUT, pr.T_TASK_COMMIT, pr.T_TASK_GET,
           pr.T_TASK_DATA, pr.T_TASK_DEL, pr.T_TASK_RUN, pr.T_TASK_STOP, pr.T_TASK_STATUS]
    assert ids == list(range(0x60, 0x6A))
    for t in ids:
        assert t in pr.TYPE_NAMES
    assert pr.CAP_TASK == 0x80, "HOST_CAP_TASK 是 HELLO 能力字的 bit7"
    assert pr.TASK_PUT_MAX == pr.MAX_PAYLOAD - 4, "PUT 一片 = 整帧载荷减去 4 字节头"
    assert pr.TASK_DATA_CHUNK == 64 and pr.TASK_SLOT_BYTES == 4096


def test_list_r_layout_matches_the_doc_and_empty_means_done():
    """§5：`id u16 | len u16 | crc u16 | flags u8 | name_len u8 | name…`（名字无 NUL 终止）。"""
    p = pr.encode_task_list_r(2, 208, 570, 0, "循迹")
    nb = "循迹".encode("utf-8")
    assert p == (pr.pack_u16(2) + pr.pack_u16(208) + pr.pack_u16(570) + bytes([0, len(nb)]) + nb)
    assert pr.decode_task_list_r(p) == {"id": 2, "len": 208, "crc": 570, "flags": 0, "name": "循迹"}
    # 空载荷 = 目录发完（不是"一条空任务"）
    assert pr.decode_task_list_r(b"") is None
    t, _seq, pl = frame_parts(pr.frame_task_list(1))
    assert t == pr.T_TASK_LIST and pl == b""


def test_data_layout_and_empty_means_eof():
    """§5：`id u16 | off u16 | 数据`；只有 4 字节（没有数据）= 读完。"""
    p = pr.encode_task_data(1, 64, b"\x01\x02\x03")
    assert p == pr.pack_u16(1) + pr.pack_u16(64) + b"\x01\x02\x03"
    assert pr.decode_task_data(p) == (1, 64, b"\x01\x02\x03")
    assert pr.decode_task_data(pr.encode_task_data(1, 208, b"")) == (1, 208, b"")


def test_status_is_11_bytes_and_carries_both_spellings():
    """§5：`id u16 | state u8 | running u8 | block u16 | elapsed_ms u32 | flags u8`。"""
    p = pr.payload_task_status({"id": 2, "state": 1, "running": True, "block": 7,
                                "elapsed_ms": 1234, "flags": 1})
    assert p.hex() == "02000101" + "0700" + "d2040000" + "01"
    assert len(p) == 11
    f = pr.frame_task_status(5, {"id": 2, "state": 1, "running": True, "block": 7,
                                 "elapsed_ms": 1234, "flags": 1})
    t, seq, pl = frame_parts(f)
    assert t == pr.T_TASK_STATUS and seq == 5 and pl == p
    d = pr.decode_task_status(p)
    assert (d["id"], d["state"], d["block"], d["flags"]) == (2, 1, 7, 1)
    assert d["running"] is True and d["running_u8"] == 1
    assert d["elapsed_ms"] == 1234 and d["elapsedMs"] == 1234, "前端读驼峰名，两边都要给"
    # 车上没载程序：id=0xFFFF / state=0xFF
    idle = pr.decode_task_status(pr.payload_task_status({"id": 0xFFFF, "state": 0xFF}))
    assert idle["id"] == 0xFFFF and idle["running"] is False and idle["elapsed_ms"] == 0
    with pytest.raises(ValueError):
        pr.decode_task_status(p[:10])


def test_commit_payload_is_the_base_seven_bytes_plus_optional_name():
    """§5：COMMIT 前 7 字节固定（`flags` 在 p[6]）；`len>7` 时 p[7] 是名字长度。"""
    base = pr.payload_task_commit(3, 208, 570)
    assert base == pr.pack_u16(3) + pr.pack_u16(208) + pr.pack_u16(570) + b"\x00" and len(base) == 7
    assert pr.payload_task_commit(3, 208, 570, "", 1)[6] == 1, "bit0 = 开机自启，原样下发"
    nb = "过障碍".encode("utf-8")
    full = pr.payload_task_commit(3, 208, 570, "过障碍")
    assert full == base + bytes([len(nb)]) + nb
    assert len(full) > 7 and full[7] == len(nb)
    # 名字超 30 字节要截断（记录头里就 30 字节）
    long_name = "x" * 40
    assert pr.payload_task_commit(0, 12, 0, long_name)[7] == 30
    # flags 与名字可以同时给：p[6]=flags、p[7]=name_len
    both = pr.payload_task_commit(0, 12, 0, "abc", 1)
    assert both[6] == 1 and both[7] == 3 and both[8:] == b"abc"


def test_frame_helpers_have_the_documented_payload_sizes():
    assert frame_parts(pr.frame_task_list(1)) == (pr.T_TASK_LIST, 1, b"")
    assert frame_parts(pr.frame_task_stop(1)) == (pr.T_TASK_STOP, 1, b"")
    assert len(frame_parts(pr.frame_task_get(1, 2))[2]) == 2
    assert len(frame_parts(pr.frame_task_del(1, 2))[2]) == 2
    assert len(frame_parts(pr.frame_task_run(1, 2))[2]) == 2
    t, _seq, pl = frame_parts(pr.frame_task_put(1, 2, 120, bytes(120)))
    assert t == pr.T_TASK_PUT and len(pl) == 4 + 120
    with pytest.raises(ValueError):
        pr.frame_task_put(1, 2, 0, bytes(pr.TASK_PUT_MAX + 1))


# ---------------------------------------------------------------- 2. 仿真车

def burn(car: SimCar, tid: int, img: bytes, crc: int, name: str = "循迹",
         commit_ok: bool = True, flags: int = 0) -> None:
    """照前端 `taskApi.burn()` 的走法：分片 PUT（每片 ≤120 B）→ COMMIT。"""
    off, seq = 0, 10
    while off < len(img):
        chunk = img[off:off + pr.TASK_PUT_MAX]
        st = ack_status(car_req(car, pr.T_TASK_PUT,
                                pr.pack_u16(tid) + pr.pack_u16(off) + chunk, seq=seq, now=1000 + seq))
        assert st == pr.ST_OK, f"PUT off={off} 被拒：status={st}"
        off += len(chunk)
        seq += 1
    st = ack_status(car_req(car, pr.T_TASK_COMMIT,
                            pr.payload_task_commit(tid, len(img), crc, name, flags),
                            seq=seq, now=1000 + seq))
    if commit_ok:
        assert st == pr.ST_OK, f"COMMIT 被拒：status={st}"
    else:
        assert st != pr.ST_OK, "这份镜像本来就该被拒，结果 COMMIT 成功了"


def test_sim_task_burn_list_get_byte_exact():
    car = SimCar()
    car.start(1000)
    img = golden_image()
    g = golden()
    burn(car, 1, img, g["crc16"])

    frames = car_req(car, pr.T_TASK_LIST, b"", seq=50, now=1100)
    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert items[-1] is None, "目录最后必须有一帧空载荷收尾"
    assert items[:-1] == [{"id": 1, "len": len(img), "crc": g["crc16"], "flags": 0, "name": "循迹"}]

    frames = car_req(car, pr.T_TASK_GET, pr.pack_u16(1), seq=51, now=1200)
    chunks = [pr.decode_task_data(f.payload) for f in frames if f.type == pr.T_TASK_DATA]
    assert chunks[-1][2] == b"", "数据最后必须有一帧空数据收尾"
    got = bytearray()
    for tid, off, data in chunks[:-1]:
        assert tid == 1 and off == len(got), "分片必须从 0 开始连号"
        got += data
    assert bytes(got) == img, "读回来的必须和写进去的逐字节一致"
    assert not [f for f in frames if f.type == pr.T_ACK], "GET 成功时不回 ACK（只回数据帧）"


def test_sim_task_run_status_stop():
    car = SimCar()
    car.start(1000)
    img, g = golden_image(), golden()
    burn(car, 0, img, g["crc16"])

    car.poll(2000)          # 车的时钟跟着 poll 走，RUN 那一帧用的是"上一拍"的时刻
    frames = car_req(car, pr.T_TASK_RUN, pr.pack_u16(0), seq=60, now=2000)
    assert ack_status(frames) == pr.ST_OK
    frames = car_req(car, pr.T_TASK_STATUS, b"", seq=61, now=2600)
    st = pr.decode_task_status([f for f in frames if f.type == pr.T_TASK_STATUS][-1].payload)
    assert st["id"] == 0 and st["running"] is True and st["elapsed_ms"] == 600
    assert st["block"] == 1, "每 400ms 走一块"

    frames = car_req(car, pr.T_TASK_STOP, b"", seq=62, now=3000)
    assert ack_status(frames) == pr.ST_OK
    frames = car_req(car, pr.T_TASK_STATUS, b"", seq=63, now=3100)
    st = pr.decode_task_status([f for f in frames if f.type == pr.T_TASK_STATUS][-1].payload)
    assert st["running"] is False
    # 停两次也得成功（幂等）
    assert ack_status(car_req(car, pr.T_TASK_STOP, b"", seq=64, now=3200)) == pr.ST_OK


def test_sim_task_rejects_half_burned_program_and_out_of_range():
    car = SimCar()
    car.start(1000)
    img, g = golden_image(), golden()

    # 没 PUT 过就 COMMIT：暂存区是空的
    st = ack_status(car_req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(2, len(img), g["crc16"]),
                            seq=70, now=1100))
    assert st == pr.ST_NOT_READY

    burn(car, 2, img, g["crc16"])
    # CRC 不对 = 烧了半个程序，必须拒
    st = ack_status(car_req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(2, len(img), g["crc16"] ^ 1),
                            seq=71, now=1200))
    assert st == pr.ST_BAD_PARAM
    # 版本号不对（镜像头 ver != 1）也得拒 —— 分片收下了，提交那一步必须拦
    bad = bytearray(img)
    bad[2] = 2
    badcrc = pr.crc16_ccitt(bytes(bad[12:]))
    burn(car, 1, bytes(bad), badcrc, name="坏版本", commit_ok=False)

    # 槽号越界 / 写超单个任务上限（TASK_SLOT_BYTES = 4096）
    assert ack_status(car_req(car, pr.T_TASK_PUT, pr.pack_u16(car.TASK_SLOTS) + pr.pack_u16(0) + b"\x00",
                              seq=73, now=1400)) == pr.ST_OUT_OF_RANGE
    assert ack_status(car_req(car, pr.T_TASK_PUT,
                              pr.pack_u16(0) + pr.pack_u16(pr.TASK_SLOT_BYTES - 8) + bytes(120),
                              seq=74, now=1500)) == pr.ST_OUT_OF_RANGE

    # 空槽读/跑：明确说"还没准备好"，而不是装作成功
    assert ack_status(car_req(car, pr.T_TASK_GET, pr.pack_u16(1), seq=75, now=1600)) == pr.ST_NOT_READY
    assert ack_status(car_req(car, pr.T_TASK_RUN, pr.pack_u16(1), seq=76, now=1700)) == pr.ST_NOT_READY


def test_sim_task_delete_frees_the_slot():
    car = SimCar()
    car.start(1000)
    img, g = golden_image(), golden()
    burn(car, car.TASK_SLOTS - 1, img, g["crc16"])
    assert ack_status(car_req(car, pr.T_TASK_DEL, pr.pack_u16(car.TASK_SLOTS - 1),
                              seq=80, now=1100)) == pr.ST_OK
    frames = car_req(car, pr.T_TASK_LIST, b"", seq=81, now=1200)
    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert items == [None], "删完只剩收尾帧"


def test_sim_task_auto_flag_roundtrip_and_lying_name_len():
    """COMMIT 的 flags 字节：bit0（开机自启）落到目录记录里、不认的位丢掉；名字长度撒谎要拒。"""
    car = SimCar()
    car.start(1000)
    img, g = golden_image(), golden()

    burn(car, 1, img, g["crc16"], name="自启", flags=0x01)
    frames = car_req(car, pr.T_TASK_LIST, b"", seq=90, now=1100)
    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert items[:-1] == [{"id": 1, "len": len(img), "crc": g["crc16"], "flags": 1, "name": "自启"}]

    # 再烧一次不带 flags：标志要跟着清掉（不是"只增不减"的锁存位）
    burn(car, 1, img, g["crc16"], name="自启", flags=0)
    items = [pr.decode_task_list_r(f.payload) for f in
             car_req(car, pr.T_TASK_LIST, b"", seq=91, now=1200) if f.type == pr.T_TASK_LIST_R]
    assert items[:-1][0]["flags"] == 0

    # 不认的位（0xFE）由车上过滤掉，只留 bit0
    burn(car, 1, img, g["crc16"], name="自启", flags=0xFE)
    items = [pr.decode_task_list_r(f.payload) for f in
             car_req(car, pr.T_TASK_LIST, b"", seq=92, now=1300) if f.type == pr.T_TASK_LIST_R]
    assert items[:-1][0]["flags"] == 0

    # 名字长度撒谎：p[7]=30 但帧里只有 1 个名字字节 ⇒ ST_BAD_PARAM（别读越界）
    st = ack_status(car_req(car, pr.T_TASK_COMMIT,
                            pr.pack_u16(1) + pr.pack_u16(len(img)) + pr.pack_u16(g["crc16"])
                            + bytes([0, 30]) + b"a", seq=93, now=1400))
    assert st == pr.ST_BAD_PARAM


# ---------------------------------------------------------------- 3. 桥端点

def test_api_tasks_end_to_end(live):
    """§3.6 八个端点走一遍：分片 PUT → COMMIT → LIST → GET 逐字节 → RUN → STATUS → STOP → DELETE。"""
    base, _bridge, _con = live
    img, g = golden_image(), golden()
    tid = 2

    code, j = http(base + "/api/tasks")
    assert code == 200 and j["ok"] is True and j["supported"] is True and isinstance(j["items"], list)

    off = 0
    while off < len(img):
        chunk = img[off:off + 120]
        code, j = http(base + "/api/tasks/put", "POST",
                       {"id": tid, "offset": off, "hex": chunk.hex()})
        assert code == 200 and j["ok"] is True, f"put off={off} 失败：{j}"
        off += len(chunk)

    code, j = http(base + "/api/tasks/commit", "POST",
                   {"id": tid, "len": len(img), "crc": g["crc16"], "name": "循迹金样本", "flags": 1})
    assert code == 200 and j["ok"] is True, f"commit 失败：{j}"

    code, j = http(base + "/api/tasks")
    hit = [it for it in j["items"] if it["id"] == tid]
    assert len(hit) == 1 and hit[0]["len"] == len(img) and hit[0]["crc"] == g["crc16"]
    assert hit[0]["flags"] == 1, "开机自启标志要随 COMMIT 下发并出现在目录里"
    assert hit[0]["name"] == "循迹金样本"

    code, j = http(f"{base}/api/tasks/get?id={tid}")
    assert code == 200 and j["ok"] is True
    assert bytes.fromhex(j["hex"]) == img, "回读的整镜像必须和写进去的一致"
    assert j["len"] == len(img) and j["crc"] == g["crc16"]

    code, j = http(base + "/api/tasks/run", "POST", {"id": tid})
    assert code == 200 and j["ok"] is True, f"run 失败：{j}"
    code, j = http(base + "/api/tasks/status")
    assert code == 200 and j["ok"] is True and j["status"] is not None
    st = j["status"]
    assert st["id"] == tid and st["running"] is True
    assert st["elapsedMs"] >= 0 and st["elapsed_ms"] == st["elapsedMs"], "两种拼写都要给"

    code, j = http(base + "/api/tasks/stop", "POST", {})
    assert code == 200 and j["ok"] is True

    code, j = http(base + "/api/tasks/delete", "POST", {"id": tid})
    assert code == 200 and j["ok"] is True
    code, j = http(base + "/api/tasks")
    assert [it for it in j["items"] if it["id"] == tid] == []


def test_api_tasks_semantic_failures_are_200_not_404(live):
    """语义性失败必须 200 + ok:false —— 前端把 404/501 理解成"桥还没实现这个功能"。"""
    base, _bridge, _con = live
    code, j = http(base + "/api/tasks/get?id=15")
    assert code == 200 and j["ok"] is False and j.get("supported") is True, j
    code, j = http(base + "/api/tasks/run", "POST", {"id": 15})
    assert code == 200 and j["ok"] is False and j.get("supported") is True, j
    # 桥自己就能判的参数错才用 4xx
    code, j = http(base + "/api/tasks/put", "POST", {"id": 0, "offset": 0, "hex": "zz"})
    assert code == 400 and j["ok"] is False
    code, j = http(base + "/api/tasks/commit", "POST", {"id": 0, "len": 12})
    assert code == 400 and j["ok"] is False
    code, j = http(base + "/api/tasks/get?id=abc")
    assert code == 400 and j["ok"] is False


def test_api_tasks_reports_unsupported_when_cap_bit_is_missing(live):
    """车上固件没置 HOST_CAP_TASK 时：200 + supported:false + 文案里带"不支持"（前端认这个）。"""
    base, _bridge, con = live
    saved = con.hello
    try:
        con.hello = dict(saved or {})
        con.hello["caps"] = 0x7F          # 7 个老能力位，没有 bit7
        assert con.cap_task() is False
        code, j = http(base + "/api/tasks")
        assert code == 200 and j["ok"] is False and j["supported"] is False
        assert "不支持" in j["error"]
        code, j = http(base + "/api/tasks/status")
        assert code == 200 and j["ok"] is True and j["supported"] is False
        code, j = http(base + "/api/tasks/run", "POST", {"id": 0})
        assert code == 200 and j["ok"] is False and j["supported"] is False
    finally:
        con.hello = saved


def test_api_tasks_reject_bad_crc_oversized_and_unknown_id(live):
    """错误路径：坏 CRC 的 commit 被拒且**不留半个程序**、超长分片两头都拦住、未知 id 的删/读。"""
    base, _bridge, _con = live
    img, g = golden_image(), golden()
    tid = 3

    # 字节先按正确方式分片写进暂存，只有 COMMIT 的 CRC 是错的 ⇒ 车必须拒，且槽位仍是空的
    off = 0
    while off < len(img):
        chunk = img[off:off + 120]
        code, j = http(base + "/api/tasks/put", "POST",
                       {"id": tid, "offset": off, "hex": chunk.hex()})
        assert code == 200 and j["ok"] is True, f"put off={off}：{j}"
        off += len(chunk)
    code, j = http(base + "/api/tasks/commit", "POST",
                   {"id": tid, "len": len(img), "crc": int(g["crc16"]) ^ 0x1234, "name": "坏 CRC"})
    assert code == 200 and j["ok"] is False, f"坏 CRC 居然被收下了：{j}"
    # 断言协议真相（状态码）而不是文案：error 里的人话随时可能被改
    assert j["status"] == pr.ST_BAD_PARAM and j["error"], j
    code, j = http(base + "/api/tasks")
    assert [it for it in j["items"] if it["id"] == tid] == [], "被拒的程序不能留在车上"

    # 写超过程序区尾（off+len > 2048）⇒ 车回 ST_OUT_OF_RANGE（语义失败仍 200 + ok:false）
    code, j = http(base + "/api/tasks/put", "POST",
                   {"id": tid, "offset": pr.TASK_SLOT_BYTES - 8, "hex": "00" * 16})
    assert code == 200 and j["ok"] is False and j["status"] == pr.ST_OUT_OF_RANGE, j
    # 槽号越界（8 个槽位：0..7）⇒ 同上
    code, j = http(base + "/api/tasks/put", "POST", {"id": 99, "offset": 0, "hex": "0000"})
    assert code == 200 and j["ok"] is False and j["status"] == pr.ST_OUT_OF_RANGE, j
    # 一片 > TASK_PUT_MAX(124) 在桥这一侧就被拦下 —— 不许组出一个超长帧发出去
    over = pr.TASK_PUT_MAX + 1
    code, j = http(base + "/api/tasks/put", "POST",
                   {"id": tid, "offset": 0, "hex": "00" * over})
    assert code == 200 and j["ok"] is False and str(pr.TASK_PUT_MAX) in j["error"], j

    # 未知 id：删除是幂等的（车上回 ok），随后读它就是"没有这个任务"
    code, j = http(base + "/api/tasks/delete", "POST", {"id": 15})
    assert code == 200 and j["ok"] is True, j
    code, j = http(base + "/api/tasks/get?id=15")
    assert code == 200 and j["ok"] is False, j


def test_api_state_carries_task_mirrors(live):
    """`/api/state`（SSE 首帧同源）要带上任务目录与运行状态镜像，刷新页面就能直接恢复。"""
    base, _bridge, _con = live
    code, j = http(base + "/api/tasks")
    assert code == 200 and j["ok"] is True, j
    code, j = http(base + "/api/state")
    assert code == 200 and j["ok"] is True
    assert isinstance(j.get("tasks"), list), f"/api/state 少了 tasks（现有键：{sorted(j)[:6]}…）"
    assert "task_status" in j, "/api/state 少了 task_status"
