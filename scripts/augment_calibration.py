#!/usr/bin/env python3
"""
augment_calibration.py — expand the dataset with small synthetic variations so
the model tolerates session-to-session differences WITHOUT recording more.

Applies to the already-relative-transformed data (run relative_features.py
first). Each training sample gets a few jittered copies simulating:
  - per-finger calibration-precision offsets (small constant offset on P,R,Y
    held across the whole window — different per finger, since each board's
    residual differs even after the relative transform)
  - per-finger gyro-bias offsets (thermal drift, constant across the window)
  - per-reading sensor noise on all channels

These are SMALL jitters (default ~8 deg) to absorb realistic session variance.
They are NOT meant to bridge a large structured pose change (e.g. a 90 deg
table-vs-rest calibration) — for that you must calibrate live in the same pose
the data was recorded in.

Leakage-safe split: a fraction of each label's ORIGINAL samples is held out as
a clean TEST set (relative only, NOT augmented). The remaining originals plus
their augmented copies form TRAIN. No augmented copy of a test sample ever
appears in train.

Output structure:
    <output>/train/<label>/*.csv   (originals + augmented)
    <output>/test/<label>/*.csv    (held-out originals, clean)

Usage
-----
    python scripts/augment_calibration.py \\
        --input scripts/data_rel --output scripts/data_rel_aug \\
        --copies 3 --holdout 0.2
"""

import argparse
import csv
import random
from pathlib import Path

FIELDS_PER_FINGER = 9
NUM_FINGERS = 5
NUM_CHANNELS = FIELDS_PER_FINGER * NUM_FINGERS
# field indices within a finger
P, R, Y, GX, GY, GZ, AX, AY, AZ = range(9)
ANGLE_FIELDS = [P, R, Y]
GYRO_FIELDS = [GX, GY, GZ]
ACCEL_FIELDS = [AX, AY, AZ]


def read_csv(path):
    with open(path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        rows = []
        for row in r:
            if len(row) != NUM_CHANNELS + 1:
                continue
            rows.append(row)
    return header, rows


def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def augment_rows(rows, args, rng):
    """Return a new list of rows with jitter applied."""
    # Per-finger, per-window constant offsets (drawn once for the whole sample).
    angle_off = [[rng.gauss(0, args.offset_deg) for _ in ANGLE_FIELDS]
                 for _ in range(NUM_FINGERS)]
    gyro_bias = [[rng.gauss(0, args.gyro_bias) for _ in GYRO_FIELDS]
                 for _ in range(NUM_FINGERS)]
    out_rows = []
    for row in rows:
        ts = row[0]
        vals = [float(x) for x in row[1:]]
        for fi in range(NUM_FINGERS):
            base = fi * FIELDS_PER_FINGER
            # angle: constant offset + per-reading noise
            for k, fj in enumerate(ANGLE_FIELDS):
                vals[base + fj] += angle_off[fi][k] + rng.gauss(0, args.noise_deg)
            # gyro: constant bias + per-reading noise
            for k, fj in enumerate(GYRO_FIELDS):
                vals[base + fj] += gyro_bias[fi][k] + rng.gauss(0, args.gyro_noise)
            # accel: per-reading noise only
            for fj in ACCEL_FIELDS:
                vals[base + fj] += rng.gauss(0, args.accel_noise)
        out_rows.append([ts] + [f"{v:.4f}" for v in vals])
    return out_rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="relative-transformed data root")
    ap.add_argument("--output", required=True)
    ap.add_argument("--copies", type=int, default=3, help="augmented copies per train sample")
    ap.add_argument("--holdout", type=float, default=0.2, help="fraction of originals -> test")
    ap.add_argument("--offset-deg", type=float, default=8.0, help="per-finger angle offset sigma")
    ap.add_argument("--noise-deg", type=float, default=1.0, help="per-reading angle noise sigma")
    ap.add_argument("--gyro-bias", type=float, default=2.0, help="per-finger gyro bias sigma (deg/s)")
    ap.add_argument("--gyro-noise", type=float, default=2.0, help="per-reading gyro noise sigma")
    ap.add_argument("--accel-noise", type=float, default=0.1, help="per-reading accel noise sigma")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    root_in = Path(args.input)
    out_train = Path(args.output) / "train"
    out_test = Path(args.output) / "test"
    if not root_in.is_dir():
        raise SystemExit(f"not a directory: {root_in}")

    n_train_files = 0
    n_test_files = 0
    for label_dir in sorted(root_in.iterdir()):
        if not label_dir.is_dir():
            continue
        csvs = sorted(label_dir.glob("*.csv"))
        if not csvs:
            continue
        order = csvs[:]
        rng.shuffle(order)
        n_test = max(1, int(round(len(order) * args.holdout))) if args.holdout > 0 else 0
        test_set = set(order[:n_test])

        idx = 1
        for path in csvs:
            header, rows = read_csv(path)
            if path in test_set:
                # clean held-out test sample (relative only, no augmentation)
                write_csv(out_test / label_dir.name / f"{label_dir.name}_{idx:04d}.csv",
                          header, rows)
                n_test_files += 1
            else:
                # original goes to train as-is...
                write_csv(out_train / label_dir.name / f"{label_dir.name}_{idx:04d}.csv",
                          header, rows)
                n_train_files += 1
                # ...plus N augmented copies
                for c in range(args.copies):
                    idx += 1
                    aug = augment_rows(rows, args, rng)
                    write_csv(out_train / label_dir.name / f"{label_dir.name}_{idx:04d}.csv",
                              header, aug)
                    n_train_files += 1
            idx += 1
        print(f"[label] {label_dir.name}: {len(csvs)} originals "
              f"-> {n_test} test, rest x{args.copies+1} train")

    print(f"\ntrain files: {n_train_files}")
    print(f"test  files: {n_test_files}")
    print(f"output: {Path(args.output).resolve()}")
    print(f"  upload <out>/train as --category training, <out>/test as --category testing")


if __name__ == "__main__":
    main()
