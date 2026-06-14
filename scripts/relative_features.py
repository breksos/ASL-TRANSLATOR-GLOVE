#!/usr/bin/env python3
"""
relative_features.py — make handshape features invariant to global hand
orientation.

ASL letters are defined by how the fingers sit RELATIVE to each other, not by
the absolute tilt of the hand. Absolute orientation depends on the calibration
zero-reference (which can't be reproduced exactly) and on however the wrist is
angled — so the same letter produces different absolute numbers every session,
which makes the model confidently misclassify.

Fix: for the orientation channels (pitch, roll, yaw, ax, ay, az), subtract the
per-timestep MEAN across the 5 fingers. Any rotation/offset common to all
fingers (global hand tilt, calibration drift) cancels; only the inter-finger
differences — the actual handshape — survive. Gyro channels (gx, gy, gz) are
angular RATE (motion) and already orientation-independent, so they pass
through unchanged.

THE SAME TRANSFORM MUST RUN IN THE APP at inference time
(flutter_app/lib/utils/relative_features.dart) or train/inference will drift
apart. Keep the field list below in sync with that file.

Channel layout (matches record.py header): 5 fingers x 9 fields, finger-major.
  fields per finger: 0=P 1=R 2=Y 3=gx 4=gy 5=gz 6=ax 7=ay 8=az

Usage
-----
    python scripts/relative_features.py --input scripts/data --output scripts/data_rel
"""

import argparse
import csv
import os
from pathlib import Path

FIELDS_PER_FINGER = 9
NUM_FINGERS = 5
NUM_CHANNELS = FIELDS_PER_FINGER * NUM_FINGERS   # 45
# Fields mean-centered across fingers (orientation-bearing). Keep identical to
# the Dart transform. gyro (3,4,5) is left absolute.
ORIENTATION_FIELDS = [0, 1, 2, 6, 7, 8]          # P, R, Y, ax, ay, az


def transform_row(vals):
    """vals: list of 45 floats (one timestep). Returns mean-centered copy."""
    out = list(vals)
    for fj in ORIENTATION_FIELDS:
        s = 0.0
        for fi in range(NUM_FINGERS):
            s += vals[fi * FIELDS_PER_FINGER + fj]
        mean = s / NUM_FINGERS
        for fi in range(NUM_FINGERS):
            out[fi * FIELDS_PER_FINGER + fj] -= mean
    return out


def process_file(in_path, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(in_path, newline="") as fin, open(out_path, "w", newline="") as fout:
        r = csv.reader(fin)
        w = csv.writer(fout)
        header = next(r)
        if len(header) != NUM_CHANNELS + 1:
            return 0, f"header has {len(header)} cols, expected {NUM_CHANNELS + 1}"
        w.writerow(header)
        n = 0
        for row in r:
            if len(row) != NUM_CHANNELS + 1:
                continue
            ts = row[0]
            try:
                vals = [float(x) for x in row[1:]]
            except ValueError:
                continue
            out = transform_row(vals)
            w.writerow([ts] + [f"{x:.4f}" for x in out])
            n += 1
    return n, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    root_in = Path(args.input)
    root_out = Path(args.output)
    if not root_in.is_dir():
        raise SystemExit(f"not a directory: {root_in}")

    total_files = 0
    total_rows = 0
    for label_dir in sorted(root_in.iterdir()):
        if not label_dir.is_dir():
            continue
        csvs = sorted(label_dir.glob("*.csv"))
        if not csvs:
            continue
        for path in csvs:
            out_path = root_out / label_dir.name / path.name
            n, err = process_file(path, out_path)
            if err:
                print(f"  WARN {path}: {err}")
                continue
            total_files += 1
            total_rows += n
        print(f"[label] {label_dir.name}: {len(csvs)} files -> {root_out/label_dir.name}")

    print(f"\nrelative transform: {total_files} files, {total_rows} rows")
    print(f"output: {root_out.resolve()}")
    print(f"mean-centered fields (per finger): {ORIENTATION_FIELDS} "
          f"(P,R,Y,ax,ay,az); gyro left absolute")


if __name__ == "__main__":
    main()
