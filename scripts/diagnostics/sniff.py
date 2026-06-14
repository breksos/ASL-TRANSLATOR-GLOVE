import serial, time
p = serial.Serial("COM15", 921600, timeout=0.2)
deadline = time.time() + 10.0
lines = []
while time.time() < deadline:
    raw = p.readline()
    if not raw:
        continue
    try:
        s = raw.decode("utf-8", errors="replace").rstrip()
    except Exception:
        continue
    if s:
        lines.append(s)
p.close()
with open(r"C:\Users\ASUS\Desktop\Projeler\ASL\_sniff.log", "w", encoding="utf-8") as f:
    f.write(f"# captured {len(lines)} lines in 10 s\n")
    for ln in lines:
        f.write(ln + "\n")
print(f"wrote {len(lines)} lines to _sniff.log")
