#!/usr/bin/env python3
"""
record.py — ASL glove gesture recorder.

Reads the CSV stream produced by the `asl_middle_master` firmware (no-DevKit
architecture, 2026-05-29 onward) running on the middle-finger XIAO. Collects
fixed-length labelled windows and writes one CSV per sample in an Edge
Impulse-friendly format (timestamp + 45 channels).

Architecture this script targets:
  - 4 slave XIAOs (thumb=1, index=2, ring=4, pinky=5) ESP-NOW unicast to the
    middle XIAO. Middle (id=3) reads its own MPU. Master emits CSV @ 921600
    over USB-C at 25 Hz per finger.
  - Palm IMU is gone (A/B test #22 confirmed no-palm model wins). 5 sources
    × 9 fields = 45 channels.

For the legacy DevKit + palm flow (54 channels @ 50 Hz), check out the
pre-2026-05-29 commit of this script from git history.

Setup
-----
1. Flash `firmware/arduino/asl_middle_master/` to the middle XIAO.
2. Flash `firmware/arduino/asl_finger_slave/` to the other four XIAOs with
   FINGER_ID set to 1, 2, 4, 5 (one per board). Paste the master MAC into
   MASTER_MAC before flashing each.
3. Plug the master into the laptop with a USB-C cable that has DATA lines.
4. Find its COM port:
       python -m serial.tools.list_ports
5. Run:
       pip install pyserial
       python record.py --port COM10 --label A
6. Inside the recorder:
       SPACE  — record a new sample with the current label
       l      — change the current label (will prompt for new name)
       q      — quit (also Ctrl-C)

Each sample is saved to `./data/<label>/<label>_<NNN>.csv` with header:
    timestamp,thumb_P,thumb_R,thumb_Y,thumb_gx,...,pinky_az
and (window_sec * 25) rows. Sources are forward-filled onto a uniform 40 ms
grid from the latest packet received per source.
"""

import argparse
import csv
import os
import re
import sys
import threading
import time
from collections import deque
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# ---------------- config ----------------
# Source order matches the master's per-packet finger_id, indexed 1..5.
# Index 0 is unused so SOURCES[fid-1] gives the right name.
SOURCES       = ["thumb", "index", "middle", "ring", "pinky"]  # ids 1..5
NUM_SOURCES   = len(SOURCES)
FIELDS        = ["P", "R", "Y", "gx", "gy", "gz", "ax", "ay", "az"]
SAMPLE_HZ     = 25                               # master cycle: 40 ms / 25 Hz
WINDOW_SEC    = 3.0
PREP_SEC      = 1.0
PERIOD_MS     = int(1000 / SAMPLE_HZ)            # 40 ms
NUM_TIMESTEPS = int(WINDOW_SEC * SAMPLE_HZ)      # 75 @ 25 Hz × 3 s
NUM_CHANNELS  = len(SOURCES) * len(FIELDS)       # 45

# Largest acceptable "how many ms since the latest packet from this source"
# observed during the window. If exceeded, prompt to keep or discard.
# Bumped 500 → 600 ms because the per-packet period is 40 ms (vs 20 ms before),
# so even a healthy 5-packet gap looks like 200 ms of staleness.
MAX_STALE_MS  = 600

ROW_RE = re.compile(
    r"^(?P<rx>\d+),(?P<id>[1-5]),(?P<seq>\d+),(?P<t>\d+),"
    r"(?P<P>[-\d.eE+]+),(?P<R>[-\d.eE+]+),(?P<Y>[-\d.eE+]+),"
    r"(?P<gx>[-\d.eE+]+),(?P<gy>[-\d.eE+]+),(?P<gz>[-\d.eE+]+),"
    r"(?P<ax>[-\d.eE+]+),(?P<ay>[-\d.eE+]+),(?P<az>[-\d.eE+]+)$"
)


def parse_row(line):
    m = ROW_RE.match(line.strip())
    if not m:
        return None
    return (
        int(m["rx"]),
        int(m["id"]),
        {
            "P":  float(m["P"]),  "R":  float(m["R"]),  "Y":  float(m["Y"]),
            "gx": float(m["gx"]), "gy": float(m["gy"]), "gz": float(m["gz"]),
            "ax": float(m["ax"]), "ay": float(m["ay"]), "az": float(m["az"]),
        },
    )


# ---------------- serial reader ----------------
class SerialBuffer:
    """Background thread: parses CSV rows into per-source deques of (rx_ms, vals)."""

    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        # Windows default USB serial RX buffer is 4 KB — overflows in 0.2 s at
        # our data rate. Bump to 256 KB. set_buffer_size is Windows-only;
        # silently ignored elsewhere.
        try:
            self.ser.set_buffer_size(rx_size=262144, tx_size=8192)
        except Exception:
            pass
        # buf[fid] holds (rx_ms, vals) tuples for finger id 1..5.
        # 4000 entries @ 25 Hz = ~160 s of history per source — plenty.
        self.buf = {i: deque(maxlen=4000) for i in range(1, NUM_SOURCES + 1)}
        self.last_rx = {i: 0 for i in range(1, NUM_SOURCES + 1)}
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.bad_lines = 0
        self.good_lines = 0
        self.wall_at_start = 0.0     # wall clock when reader saw first packet
        self.board_at_start = 0      # board ms at first packet
        self.thread = threading.Thread(target=self._reader, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1.0)
        try:
            self.ser.close()
        except Exception:
            pass

    def _reader(self):
        # Chunked reads instead of readline(): pyserial's readline() does single-byte
        # reads in a loop, which can't keep up with 25 Hz × 5 sources @ 921600 baud
        # (≈10 KB/s) on Windows. We read in 4 KB chunks and split on '\n' in
        # Python — much faster.
        self.ser.timeout = 0.05
        try:
            self.ser.read(self.ser.in_waiting or 1)   # discard initial mid-line
        except Exception:
            pass
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except Exception:
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                try:
                    line = raw.decode("utf-8", errors="replace").strip()
                except Exception:
                    continue
                if not line or line.startswith("#"):
                    continue
                parsed = parse_row(line)
                if parsed is None:
                    self.bad_lines += 1
                    continue
                rx, sid, vals = parsed
                with self.lock:
                    if self.wall_at_start == 0.0:
                        self.wall_at_start = time.time()
                        self.board_at_start = rx
                    self.buf[sid].append((rx, vals))
                    self.last_rx[sid] = rx
                    self.good_lines += 1

    def latest_rx_all_sources(self):
        with self.lock:
            return dict(self.last_rx)

    def reader_lag_ms(self):
        """How far behind real time the parsed buffer is, in ms.
        Positive number = Python reader is behind the receiver."""
        with self.lock:
            if self.wall_at_start == 0.0:
                return 0
            latest = max(self.last_rx.values())
            wall_elapsed_ms = (time.time() - self.wall_at_start) * 1000.0
            board_elapsed_ms = latest - self.board_at_start
            return max(0, wall_elapsed_ms - board_elapsed_ms)

    def snapshot_window(self, t0_ms, duration_ms):
        """Build an (N, 45) matrix by forward-filling each source to a 40 ms grid.
        Returns (matrix, worst_stale_ms) or (None, error_reason)."""
        end_ms = t0_ms + duration_ms
        with self.lock:
            data = {i: list(self.buf[i]) for i in range(1, NUM_SOURCES + 1)}

        # Make sure every source has at least one packet within or before window.
        for i in range(1, NUM_SOURCES + 1):
            data[i] = [p for p in data[i] if p[0] <= end_ms]
            if not data[i]:
                return None, f"no packets from {SOURCES[i-1]} (source {i})"

        N = int(duration_ms / PERIOD_MS)
        matrix = [[0.0] * NUM_CHANNELS for _ in range(N)]
        worst_per_src = {i: 0 for i in range(1, NUM_SOURCES + 1)}

        # For each source we walk a pointer forward through its sorted packets.
        pointers = {i: 0 for i in range(1, NUM_SOURCES + 1)}
        for n in range(N):
            t = t0_ms + n * PERIOD_MS
            for src_idx, sid in enumerate(range(1, NUM_SOURCES + 1)):
                pkts = data[sid]
                p = pointers[sid]
                # advance while next packet still <= t
                while p + 1 < len(pkts) and pkts[p + 1][0] <= t:
                    p += 1
                pointers[sid] = p
                rx, vals = pkts[p]
                stale = t - rx
                if stale > worst_per_src[sid]:
                    worst_per_src[sid] = stale
                base = src_idx * len(FIELDS)
                for fi, f in enumerate(FIELDS):
                    matrix[n][base + fi] = vals[f]
        return matrix, worst_per_src


# ---------------- cross-platform single-key input ----------------
def _getch_windows():
    import msvcrt
    while True:
        ch = msvcrt.getwch()
        # Function/arrow keys come as two reads on Windows; consume the second.
        if ch in ("\x00", "\xe0"):
            msvcrt.getwch()
            continue
        return ch


def _getch_unix():
    import sys, tty, termios
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        return sys.stdin.read(1)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def getch():
    if os.name == "nt":
        return _getch_windows()
    return _getch_unix()


# ---------------- file I/O ----------------
def header_row():
    cols = ["timestamp"]
    for s in SOURCES:
        for f in FIELDS:
            cols.append(f"{s}_{f}")
    return cols


def next_sample_path(label_dir, label):
    existing = list(label_dir.glob(f"{label}_*.csv"))
    nums = []
    for p in existing:
        m = re.match(rf"{re.escape(label)}_(\d+)\.csv$", p.name)
        if m:
            nums.append(int(m.group(1)))
    n = (max(nums) + 1) if nums else 1
    return label_dir / f"{label}_{n:03d}.csv"


def save_window(matrix, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header_row())
        for n, row in enumerate(matrix):
            w.writerow([n * PERIOD_MS] + [f"{x:.4f}" for x in row])


def beep():
    try:
        sys.stdout.write("\a")
        sys.stdout.flush()
    except Exception:
        pass


# ---------------- main ----------------
def main():
    ap = argparse.ArgumentParser(description="ASL glove gesture recorder")
    ap.add_argument("--port", required=True, help="serial port (COM15, /dev/ttyUSB0, ...)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--label", default="A", help="initial label")
    ap.add_argument("--datadir", default="data", help="output root directory")
    ap.add_argument("--window", type=float, default=WINDOW_SEC, help="window length in seconds")
    ap.add_argument("--prep", type=float, default=PREP_SEC, help="prep time before window starts")
    args = ap.parse_args()

    duration_ms = int(args.window * 1000)
    prep_ms     = int(args.prep * 1000)
    data_root = Path(args.datadir)
    data_root.mkdir(parents=True, exist_ok=True)

    print(f"Opening {args.port} @ {args.baud} baud...")
    try:
        buf = SerialBuffer(args.port, args.baud)
    except serial.SerialException as e:
        sys.exit(f"could not open {args.port}: {e}")
    buf.start()

    # The master XIAO resets when we open the COM port (DTR pulse), then spends
    # ~3 s booting + calibrating its own MPU before ESP-NOW receive comes up.
    # Slaves only show up once their boards are powered AND the master is alive
    # to ack their unicast frames. Wait up to 30 s for all 5 sources.
    print("Warming up (waiting for master + all 4 slaves to come online)...")
    deadline = time.time() + 30
    last_good = -1
    while time.time() < deadline:
        if buf.good_lines >= 200 and all(len(buf.buf[i]) > 0
                                        for i in range(1, NUM_SOURCES + 1)):
            break
        if buf.good_lines != last_good:
            alive = sum(1 for i in range(1, NUM_SOURCES + 1) if len(buf.buf[i]) > 0)
            print(f"  ...{buf.good_lines:5d} valid rows so far, "
                  f"sources alive: {alive}/{NUM_SOURCES}",
                  end="\r", flush=True)
            last_good = buf.good_lines
        time.sleep(0.5)
    print()

    counts = {i: len(buf.buf[i]) for i in range(1, NUM_SOURCES + 1)}
    print("Packets received per source after warmup:")
    for i, name in enumerate(SOURCES, start=1):
        flag = "" if counts[i] > 0 else "  <-- NO DATA"
        print(f"  {name:7s} (id {i}): {counts[i]:4d}{flag}")
    if buf.good_lines == 0:
        print()
        print("ERROR: 0 valid CSV rows after 30 s. Likely causes:")
        print("  - Master flashed with old BLE-mode build? Reflash asl_middle_master")
        print("    (CSV mode @ 921600).")
        print("  - Wrong COM port or wrong baud (expecting 921600).")
        print("  - USB-C cable is power-only (no data lines).")
        print("  - Master's own MPU calibration stuck retrying (board not still).")
        buf.close()
        sys.exit(1)
    if any(c == 0 for c in counts.values()):
        silent = [SOURCES[i-1] for i in range(1, NUM_SOURCES + 1) if counts[i] == 0]
        print(f"WARNING: silent source(s): {silent}. "
              f"Power on those slave XIAOs (or check MASTER_MAC matches) "
              f"or recording will fail.")

    label = args.label.strip() or "A"

    print()
    print("=" * 60)
    print("Recording")
    print("=" * 60)
    print(f"  current label : {label!r}")
    print(f"  window length : {args.window:.1f} s")
    print(f"  prep before   : {args.prep:.1f} s")
    print(f"  output dir    : {data_root.resolve()}")
    print()
    print("  SPACE = record    l = change label    q = quit")
    print()

    try:
        while True:
            label_dir = data_root / label
            label_dir.mkdir(exist_ok=True)
            on_disk = len(list(label_dir.glob(f"{label}_*.csv")))
            lag = buf.reader_lag_ms()
            lag_str = f" lag={lag:.0f}ms" if lag > 50 else ""
            print(f"[{label}] {on_disk} saved{lag_str}. Press SPACE/l/q: ",
                  end="", flush=True)
            ch = getch()
            print()

            if ch in ("q", "Q", "\x1b", "\x03"):    # q, Q, ESC, Ctrl-C
                break

            if ch in ("l", "L"):
                print("  new label: ", end="", flush=True)
                # we already consumed one keystroke; switch back to line input
                new_label = input().strip()
                if new_label:
                    label = new_label
                continue

            if ch != " ":
                continue

            # ---- prep countdown + GO/STOP cues ----
            with buf.lock:
                latest = max(buf.last_rx.values())
            if latest == 0:
                print("  FAIL: no serial data")
                continue
            t0 = latest + prep_ms

            for s in range(int(args.prep), 0, -1):
                print(f"  ready in {s}...", end="\r", flush=True)
                time.sleep(1.0 if args.prep >= 1 else args.prep)
                if args.prep < 1:
                    break
            beep()
            print("  GO!" + " " * 20)
            time.sleep(args.window)
            beep()
            print("  STOP")

            # Wait for all 5 fingers to catch up to end_ms. (Palm is gone in
            # the no-DevKit architecture — the master IS finger 3.)
            end_ms = t0 + duration_ms
            wait_deadline = time.time() + 3.0
            finger_ids = list(range(1, NUM_SOURCES + 1))
            laggers = []
            while time.time() < wait_deadline:
                with buf.lock:
                    laggers = [SOURCES[i-1] for i in finger_ids
                               if buf.last_rx[i] < end_ms]
                if not laggers:
                    break
                time.sleep(0.02)
            if laggers:
                print(f"  WARN: still waiting on {laggers} after 3 s — taking the window anyway")

            mat, per_src = buf.snapshot_window(t0, duration_ms)
            if mat is None:
                print(f"  FAIL: {per_src}")
                continue

            # Per-source staleness report
            finger_max = max(per_src[i] for i in finger_ids)
            worst_label = max(per_src.items(), key=lambda kv: kv[1])
            worst_src_name = SOURCES[worst_label[0]-1]
            worst_ms = worst_label[1]
            detail = ", ".join(f"{SOURCES[i-1]}={per_src[i]}"
                               for i in range(1, NUM_SOURCES + 1))

            if finger_max > MAX_STALE_MS:
                print(f"  WARN finger worst={finger_max} ms (> {MAX_STALE_MS})  [{detail}]")
                print(f"  Keep this sample? (y/n): ", end="", flush=True)
                ans = getch().lower()
                print()
                if ans != "y":
                    print("  discarded.")
                    continue

            path = next_sample_path(label_dir, label)
            save_window(mat, path)
            print(f"  saved {path}  (worst={worst_src_name}:{worst_ms} ms)")

    except KeyboardInterrupt:
        pass
    finally:
        buf.close()
        print()
        print("Summary:")
        for d in sorted(data_root.iterdir()):
            if d.is_dir():
                n = len(list(d.glob(f"{d.name}_*.csv")))
                print(f"  {d.name:10s} : {n} samples")
        print("Bye.")


if __name__ == "__main__":
    main()
