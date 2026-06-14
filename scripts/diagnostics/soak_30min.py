"""30-minute passive soak test on COM15.
Glove is powered and stationary (NOT worn). Logs:
  - per-minute packet counts per source
  - any inter-packet gap > 500 ms (precise event log)
  - cumulative health summary at end

If the glove fails during this test → problem is thermal/firmware/power.
If everything stays healthy → recording failures are motion-mechanical.
"""
import serial, time, re
from collections import defaultdict

p = serial.Serial("COM15", 921600, timeout=0.1)
try:
    p.set_buffer_size(rx_size=262144)
except Exception:
    pass

rx_re = re.compile(r"^(\d+),([1-6]),(\d+),")

DURATION = 30 * 60                  # 30 minutes
BUCKET   = 60.0                     # 1-minute buckets
GAP_THRESHOLD_MS = 500              # log any gap > this

start = time.time()
buckets = defaultdict(lambda: defaultdict(int))
last_pkt_t = {i: None for i in range(1, 7)}     # per-source last wall-time
max_gap = {i: 0.0 for i in range(1, 7)}
total_pkts = defaultdict(int)
gap_events = []   # (wall_t_seconds, source, gap_ms)
buf = b""

names = ["", "thumb", "index", "middle", "ring", "pinky", "palm"]
print(f"soaking for {DURATION // 60} min, log gap >{GAP_THRESHOLD_MS} ms...")
print(f"start={time.strftime('%H:%M:%S')}, end ~={time.strftime('%H:%M:%S', time.localtime(time.time()+DURATION))}")
print()

# print per-minute heartbeat
last_heartbeat = 0

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

        # detect gap from previous packet of THIS source
        if last_pkt_t[fid] is not None:
            gap_ms = (wall_t - last_pkt_t[fid]) * 1000.0
            if gap_ms > max_gap[fid]:
                max_gap[fid] = gap_ms
            if gap_ms > GAP_THRESHOLD_MS:
                gap_events.append((wall_t, fid, gap_ms))
                print(f"  [{wall_t:7.1f}s] GAP {names[fid]:7s} {gap_ms:.0f} ms")
        last_pkt_t[fid] = wall_t

        bucket = int(wall_t / BUCKET)
        buckets[bucket][fid] += 1
        total_pkts[fid] += 1

    # per-minute heartbeat
    elapsed_min = int((time.time() - start) / 60)
    if elapsed_min > last_heartbeat:
        last_heartbeat = elapsed_min
        cur_bucket = elapsed_min - 1
        if cur_bucket >= 0:
            counts = buckets[cur_bucket]
            line = f"[min {elapsed_min:>2}]"
            for i in range(1, 7):
                c = counts.get(i, 0)
                flag = "*" if c < 2700 else " "       # expect ~3000/min at 50 Hz
                line += f" {names[i][:5]}={c:>4}{flag}"
            print(line)
p.close()

print()
print("=" * 60)
print("FINAL SUMMARY")
print("=" * 60)
print(f"total duration: {DURATION/60:.0f} min")
print(f"gap events (>{GAP_THRESHOLD_MS} ms): {len(gap_events)}")
print()
print(f"{'source':>8} | {'total pkts':>10} | {'expected':>9} | {'max gap (ms)':>12}")
print("-" * 50)
for i in range(1, 7):
    expected = int(DURATION * 50)   # 50 Hz
    pct = 100.0 * total_pkts[i] / expected if expected else 0
    flag = " ✓" if pct > 95 and max_gap[i] < 500 else " <-- ISSUE"
    print(f"  {names[i]:6s} | {total_pkts[i]:>10} | {expected:>9} | {max_gap[i]:>12.0f}{flag}")

if gap_events:
    print()
    print(f"top 10 worst gap events:")
    for t, fid, ms in sorted(gap_events, key=lambda x: -x[2])[:10]:
        print(f"  [{t:7.1f}s] {names[fid]:7s} gap={ms:.0f} ms")
