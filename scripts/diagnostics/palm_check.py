"""Capture 20 s from COM15, compute per-source rate and latest rx_ms.
Tells us whether palm is structurally lagging vs fingers."""
import serial, time, re
from collections import defaultdict

p = serial.Serial("COM15", 921600, timeout=0.1)
try:
    p.set_buffer_size(rx_size=262144)
except Exception:
    pass

rx_re = re.compile(r"^(\d+),([1-6]),(\d+),")
counts = defaultdict(int)
latest_rx = {i: 0 for i in range(1, 7)}
first_rx = {i: 0 for i in range(1, 7)}
first_wall = None
start = time.time()
buf = b""
while time.time() - start < 20:
    chunk = p.read(4096)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = line.decode("utf-8", errors="replace").strip()
        if not s or s.startswith("#"):
            continue
        m = rx_re.match(s)
        if not m:
            continue
        rx, fid = int(m.group(1)), int(m.group(2))
        if first_wall is None:
            first_wall = time.time()
        if first_rx[fid] == 0:
            first_rx[fid] = rx
        latest_rx[fid] = rx
        counts[fid] += 1
p.close()

names = ["", "thumb", "index", "middle", "ring", "pinky", "palm"]
wall_elapsed_ms = (time.time() - first_wall) * 1000.0
print(f"wall elapsed: {wall_elapsed_ms:.0f} ms")
print(f"{'source':8s} {'rate Hz':>8s} {'board_span ms':>14s} {'latest_rx':>12s} {'delta vs max':>14s}")
max_latest = max(latest_rx.values())
for i in range(1, 7):
    if counts[i] == 0:
        print(f"  {names[i]:8s} NO DATA")
        continue
    span = latest_rx[i] - first_rx[i]
    rate = counts[i] * 1000.0 / max(span, 1)
    delta = max_latest - latest_rx[i]
    print(f"  {names[i]:6s} {rate:8.2f} {span:14d} {latest_rx[i]:12d} {delta:14d}")
