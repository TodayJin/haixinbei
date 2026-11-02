"""驱动 + 同时抓固件文本探针（同一个串口，所以必须一个进程干两件事）。

用法： python A/_tools/probe.py 0 -3000 --sec 3
      python A/_tools/probe.py 3000 0 --sec 3
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

a = int(sys.argv[1]) if len(sys.argv) > 1 else 0
b = int(sys.argv[2]) if len(sys.argv) > 2 else -3000
sec = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0

lk = SerialLink(port=os.environ.get("CAR_PORT", "COM4"), baud=921600)
lk.start()
time.sleep(0.4)
print("  驱动 wheels(%d, %d)，同时抓固件探针文本：" % (a, b))
t0 = time.time()
last = -1e9
k = 0
n = 0
while time.time() - t0 < sec:
    now = time.time() - t0
    if now - last >= 0.08:
        last = now
        lk.send(pr.frame_drive(50, 1, a, b))
    k += 1
    for f in (lk.poll(k) or []):
        if f.type == pr.T_TEXT:
            try:
                s = pr.decode_text(f.payload)
            except Exception:
                s = f.payload.decode("utf-8", "replace")
            if "sp " in s or "stops" in s:
                n += 1
                print("  %5.1fs  %s" % (now, s.strip()))
    time.sleep(0.002)
try:
    lk.send(pr.frame_drive(51, 0, 0, 0))
except Exception:
    pass
lk.stop()
print("  共 %d 条探针文本" % n)
