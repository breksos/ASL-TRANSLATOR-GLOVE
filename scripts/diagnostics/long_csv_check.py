"""5-minute soak test on COM15. Verifies palm stays alive in the new
direct-print receiver firmware. Bucketizes packet counts per 30 s window
per source — palm should hold ~1500 per window throughout."""
import serial, time, re
from collections import defaultdict

p = serial.Serial("COM15", 921600, timeout=0.1)
try:
    p.set_buffer_size(rx_size=262144)
except Exception:
    pass

rx_re = re.compile(r"^(\d+),([1-6]),(\d+),")

duration = 300.0
bucket_sec = 30.0
start = time.time()
buckets = defaultdict(lambda: defaultdict(int))   # buckets[bucket_idx][fid] = count
gap_per_source = {i: {'last_t': None, 'max_gap': 0.0} for i in range(1, 7)}
alive_lines = []
buf = b""
print(f"soaking for {int(duration)} s, bucket = {int(bucket_sec)} s...")

while time.time() - start < duration:
    chunk = p.read(8192)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = line.decode("utf-8", errors="replace").strip()
        if not s:
            continue
        if s.startswith("#"):
            if "alive" in s:
                alive_lines.append((time.time() - start, s))
            continue
        m = rx_re.match(s)
        if not m:
            continue
        fid = int(m.group(2))
        wall_t = time.time() - start
        bucket = int(wall_t / bucket_sec)
        buckets[bucket][fid] += 1
        # track max gap per source
        prev = gap_per_source[fid]['last_t']
        if prev is not None:
            gap = wall_t - prev
            if gap > gap_per_source[fid]['max_gap']:
                gap_per_source[fid]['max_gap'] = gap
        gap_per_source[fid]['last_t'] = wall_t
p.close()

names = ["", "thumb", "index", "middle", "ring", "pinky", "palm"]

print(f"\ncaptured {len(alive_lines)} alive heartbeats")
if alive_lines:
    print(f"  first: [{alive_lines[0][0]:6.1f}s] {alive_lines[0][1]}")
    print(f"  last:  [{alive_lines[-1][0]:6.1f}s] {alive_lines[-1][1]}")

print("\nper-30 s bucket packet counts:")
print(f"  {'bucket':>8} | {'thumb':>5} {'index':>5} {'middle':>6} {'ring':>5} {'pinky':>5} {'palm':>5}")
for b in sorted(buckets.keys()):
    counts = buckets[b]
    palm_alarm = " <-- PALM DEAD" if counts.get(6, 0) < 1000 else ""
    print(f"  {b*int(bucket_sec):>4}-{(b+1)*int(bucket_sec):>3}s | "
          f"{counts.get(1,0):>5} {counts.get(2,0):>5} {counts.get(3,0):>6} "
          f"{counts.get(4,0):>5} {counts.get(5,0):>5} {counts.get(6,0):>5}{palm_alarm}")

print("\nmax inter-packet gap per source (s):")
for i in range(1, 7):
    g = gap_per_source[i]['max_gap']
    flag = " <-- BAD" if (i == 6 and g > 1.0) or (i < 6 and g > 0.5) else ""
    print(f"  {names[i]:7s}: {g:.3f}{flag}")
