"""30 s motion check on COM15. Wear the glove and perform any signs you like.
Reports per-source packet count, rate, max gap, and any gaps > 200 ms."""
import serial, time, re

p = serial.Serial("COM15", 921600, timeout=0.1)
try:
    p.set_buffer_size(rx_size=262144)
except Exception:
    pass

rx_re = re.compile(r"^(\d+),([1-6]),(\d+),")
DURATION = 30.0
GAP_LOG_MS = 200

print("waiting for receiver to boot + calibrate (hold still)...", flush=True)
warmup_buf = b""
seen_sources = set()
warmup_deadline = time.time() + 30
while time.time() < warmup_deadline and len(seen_sources) < 6:
    chunk = p.read(4096)
    if chunk:
        warmup_buf += chunk
    while b"\n" in warmup_buf:
        line, warmup_buf = warmup_buf.split(b"\n", 1)
        s = line.decode("utf-8", errors="replace").strip()
        m = rx_re.match(s)
        if m:
            seen_sources.add(int(m.group(2)))
if len(seen_sources) < 6:
    print(f"WARN: only {len(seen_sources)}/6 sources seen during warmup, continuing anyway")
print("WEAR THE GLOVE AND START SIGNING — capturing 30 s", flush=True)

start = time.time()
last_pkt_t = {i: None for i in range(1, 7)}
max_gap = {i: 0.0 for i in range(1, 7)}
total = {i: 0 for i in range(1, 7)}
gap_events = []
buf = b""
names = ["", "thumb", "index", "middle", "ring", "pinky", "palm"]
while time.time() - start < DURATION:
    chunk = p.read(8192)
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
        fid = int(m.group(2))
        wall_t = time.time() - start
        if last_pkt_t[fid] is not None:
            gap_ms = (wall_t - last_pkt_t[fid]) * 1000.0
            if gap_ms > max_gap[fid]:
                max_gap[fid] = gap_ms
            if gap_ms > GAP_LOG_MS:
                gap_events.append((wall_t, fid, gap_ms))
        last_pkt_t[fid] = wall_t
        total[fid] += 1
p.close()

print()
print(f"{'source':>8} | {'packets':>8} | {'rate Hz':>8} | {'max gap':>8} | {'effective loss':>14}")
print("-" * 60)
for i in range(1, 7):
    rate = total[i] / DURATION
    expected = 50 * DURATION
    loss_pct = 100.0 * max(0, expected - total[i]) / expected
    flag = "" if loss_pct < 5 and max_gap[i] < 500 else "  <-- DEGRADED"
    print(f"  {names[i]:6s} | {total[i]:>8} | {rate:>7.1f}  | {max_gap[i]:>6.0f}ms | {loss_pct:>12.1f} %{flag}")

print()
if gap_events:
    print(f"all gap events >{GAP_LOG_MS} ms during signing ({len(gap_events)} total):")
    for t, fid, ms in gap_events:
        print(f"  [{t:5.1f}s] {names[fid]:6s} gap={ms:.0f} ms")
else:
    print(f"no gap events > {GAP_LOG_MS} ms — link held cleanly through motion")
