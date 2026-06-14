"""5-minute monitor of asl_hand_receiver output on COM15.
Captures every [stats] line, prints a summary table at the end."""
import serial, time, re

p = serial.Serial("COM15", 921600, timeout=0.1)
try:
    p.set_buffer_size(rx_size=262144)
except Exception:
    pass

# stats line format:
# [stats]  thumb  rx/s=99 loss=0  index  rx/s=100 loss=0  middle ...  palm   rate=50
re_finger = re.compile(r"(thumb|index|middle|ring|pinky)\s+rx/s=(\d+)\s+loss=(\d+)")
re_palm   = re.compile(r"palm\s+rate=(\d+)")

duration = 300.0
start = time.time()
buf = b""
stats_lines = []
print(f"capturing for {duration:.0f} s...")

while time.time() - start < duration:
    chunk = p.read(8192)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = line.decode("utf-8", errors="replace").strip()
        if "[stats]" in s:
            stats_lines.append((time.time() - start, s))
p.close()

print(f"\ncaptured {len(stats_lines)} stats lines in {duration:.0f} s\n")

# Show first 3, last 3, and any with palm rate < 30 (likely dying)
print("--- first 3 ---")
for t, s in stats_lines[:3]:
    print(f"  [{t:6.1f}s] {s}")

print("\n--- last 3 ---")
for t, s in stats_lines[-3:]:
    print(f"  [{t:6.1f}s] {s}")

print("\n--- any palm anomalies (rate<30 or finger loss/s > 5) ---")
prev_loss = {}
anomalies = 0
for t, s in stats_lines:
    pm = re_palm.search(s)
    palm_rate = int(pm.group(1)) if pm else -1
    fingers = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in re_finger.finditer(s)}
    new_loss = {k: v[1] for k, v in fingers.items()}
    # delta in loss since previous stat
    bad_fingers = []
    for f, l in new_loss.items():
        d = l - prev_loss.get(f, l)
        if d > 5:
            bad_fingers.append(f"{f}+{d}")
    if palm_rate >= 0 and palm_rate < 30:
        print(f"  [{t:6.1f}s] palm_rate={palm_rate}  {s}")
        anomalies += 1
    elif bad_fingers:
        print(f"  [{t:6.1f}s] bursts: {bad_fingers}")
        anomalies += 1
    prev_loss = new_loss

if anomalies == 0:
    print("  none — link healthy throughout")

# Final loss numbers
print("\n--- final cumulative loss per finger ---")
if stats_lines:
    last = stats_lines[-1][1]
    for f, (rxs, l) in {m.group(1): (int(m.group(2)), int(m.group(3))) for m in re_finger.finditer(last)}.items():
        print(f"  {f:7s} rx/s={rxs:3d} cumulative_loss={l}")
    pm = re_palm.search(last)
    if pm:
        print(f"  palm    rate={pm.group(1)}")
