#!/usr/bin/env python3
"""
compute_stats.py — fit sklearn StandardScaler on the training CSVs and
export the per-channel mean/std to stats.json for the Flutter Normalizer.

Reads every CSV under <datadir>/<label>/*.csv (skipping the TEST subdir
by default — see --exclude), stacks all rows into one (N, 45) matrix,
fits sklearn.preprocessing.StandardScaler, and writes:

    { "mean": [m0, ..., m44], "std": [s0, ..., s44] }

The same stats are then applied to both train AND test sets via
normalize_csvs.py — the test set must never contribute to the stats or
your held-out accuracy is meaningless.

The output format matches what Flutter's Normalizer.dart expects, so
dropping stats.json into flutter_app/assets/normalization/ closes the
loop: model trained on z-scored data, app applies the same z-score at
inference time.

Requirements
------------
    pip install numpy scikit-learn

Usage
-----
    python scripts/compute_stats.py \\
        --datadir scripts/data \\
        --out flutter_app/assets/normalization/stats.json
"""

import argparse
import csv
import json
import sys
from pathlib import Path

try:
    import numpy as np
    from sklearn.preprocessing import StandardScaler
except ImportError:
    sys.exit("requires numpy + scikit-learn: pip install numpy scikit-learn")

NUM_CHANNELS = 45   # 5 fingers × 9 fields


def load_csvs(root, exclude):
    """Yield (label, np.ndarray of shape (rows, NUM_CHANNELS))."""
    for label_dir in sorted(root.iterdir()):
        if not label_dir.is_dir():
            continue
        if label_dir.name in exclude:
            print(f"[skip] {label_dir.name}/ (in --exclude)")
            continue
        csvs = sorted(label_dir.glob("*.csv"))
        if not csvs:
            continue
        rows = []
        for path in csvs:
            with open(path, newline="") as f:
                rdr = csv.reader(f)
                header = next(rdr)
                if len(header) != NUM_CHANNELS + 1:
                    print(f"  WARN {path.name}: header has {len(header)} cols, "
                          f"expected {NUM_CHANNELS + 1}; skipping")
                    continue
                for row in rdr:
                    if len(row) != NUM_CHANNELS + 1:
                        continue
                    try:
                        rows.append([float(x) for x in row[1:]])  # skip timestamp
                    except ValueError:
                        continue
        if rows:
            arr = np.asarray(rows, dtype=np.float64)
            print(f"[label] {label_dir.name}: {len(csvs)} files, {arr.shape[0]} rows")
            yield label_dir.name, arr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True,
                    help="root dir holding <label>/*.csv (e.g. scripts/data)")
    ap.add_argument("--out", required=True,
                    help="output stats.json path")
    ap.add_argument("--exclude", nargs="*", default=["TEST"],
                    help="subdir names to skip (default: TEST)")
    args = ap.parse_args()

    root = Path(args.datadir)
    if not root.is_dir():
        sys.exit(f"not a directory: {root}")

    blocks = list(load_csvs(root, args.exclude))
    if not blocks:
        sys.exit("no training rows found")

    X = np.vstack([arr for _, arr in blocks])
    print(f"\nstacked training matrix: {X.shape}")

    scaler = StandardScaler()
    scaler.fit(X)

    # StandardScaler protects against zero-variance channels by setting
    # scale_[i] = 1.0, matching the guard in Normalizer.dart.
    mean = scaler.mean_.tolist()
    std  = scaler.scale_.tolist()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump({"mean": mean, "std": std}, f, indent=2)

    print(f"wrote: {out_path.resolve()}")

    # Sanity print — first finger's 9 channels.
    print()
    names = ("thumb_P thumb_R thumb_Y thumb_gx thumb_gy thumb_gz "
             "thumb_ax thumb_ay thumb_az").split()
    print(f"{'channel':<10s} {'mean':>10s} {'std':>10s}")
    for i, name in enumerate(names):
        print(f"{name:<10s} {mean[i]:>10.3f} {std[i]:>10.3f}")
    print(f"... ({NUM_CHANNELS - len(names)} more channels in stats.json)")

    # sklearn quirk: zero-variance becomes scale_=1, but the channel still
    # carries no info. Flag for the user.
    constant_channels = [i for i in range(NUM_CHANNELS)
                         if scaler.var_[i] < 1e-9]
    if constant_channels:
        print()
        print(f"WARNING: {len(constant_channels)} channel(s) have ~zero variance: "
              f"{constant_channels}")
        print("These are constant in your training data — likely a stuck sensor")
        print("or a finger that never moved. Model will see zeros for these")
        print("channels at inference. Investigate before training.")


if __name__ == "__main__":
    main()
