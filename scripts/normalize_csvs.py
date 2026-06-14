#!/usr/bin/env python3
"""
normalize_csvs.py — apply stats.json to every CSV in a dataset.

Reads <input_dir>/<label>/*.csv and writes z-scored versions to
<output_dir>/<label>/*.csv, leaving the timestamp column untouched.
Header is preserved.

Use the SAME stats.json for train + test. The stats must come from the
training set only (see compute_stats.py).

Usage
-----
    # Training set:
    python scripts/normalize_csvs.py \\
        --input scripts/data \\
        --output scripts/data_norm \\
        --stats flutter_app/assets/normalization/stats.json \\
        --exclude TEST

    # Test set:
    python scripts/normalize_csvs.py \\
        --input scripts/data/TEST \\
        --output scripts/data_norm/TEST \\
        --stats flutter_app/assets/normalization/stats.json

After this, upload data_norm/* to Edge Impulse as the training set and
data_norm/TEST/* as the test set. The model will be trained on
zero-mean / unit-std inputs and the Flutter Normalizer (using the same
stats.json) will produce the same distribution at inference time.
"""

import argparse
import csv
import json
import sys
from pathlib import Path

NUM_CHANNELS = 45


def load_stats(path):
    with open(path) as f:
        s = json.load(f)
    mean = s["mean"]
    std  = s["std"]
    if len(mean) != NUM_CHANNELS or len(std) != NUM_CHANNELS:
        sys.exit(f"stats.json has wrong length (mean={len(mean)}, "
                 f"std={len(std)}, expected {NUM_CHANNELS})")
    # Guard against div-by-zero (same logic as Normalizer.dart).
    std_safe = [s if s > 1e-6 else 1.0 for s in std]
    return mean, std_safe


def normalize_file(in_path, out_path, mean, std):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    rows_in = 0
    rows_out = 0
    with open(in_path, newline="") as fin, open(out_path, "w", newline="") as fout:
        rdr = csv.reader(fin)
        wtr = csv.writer(fout)
        header = next(rdr)
        if len(header) != NUM_CHANNELS + 1:
            return 0, 0, f"header has {len(header)} cols, expected {NUM_CHANNELS + 1}"
        wtr.writerow(header)
        for row in rdr:
            rows_in += 1
            if len(row) != NUM_CHANNELS + 1:
                continue
            try:
                ts = row[0]
                vals = [float(x) for x in row[1:]]
            except ValueError:
                continue
            normed = [(vals[i] - mean[i]) / std[i] for i in range(NUM_CHANNELS)]
            wtr.writerow([ts] + [f"{x:.6f}" for x in normed])
            rows_out += 1
    return rows_in, rows_out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input",  required=True, help="input dataset root")
    ap.add_argument("--output", required=True, help="output dataset root")
    ap.add_argument("--stats",  required=True, help="stats.json path")
    ap.add_argument("--exclude", nargs="*", default=[],
                    help="subdir names to skip (e.g. TEST when normalising train)")
    args = ap.parse_args()

    mean, std = load_stats(args.stats)
    root_in  = Path(args.input)
    root_out = Path(args.output)

    if not root_in.is_dir():
        sys.exit(f"not a directory: {root_in}")

    total_files = 0
    total_rows  = 0
    for label_dir in sorted(root_in.iterdir()):
        if not label_dir.is_dir():
            continue
        if label_dir.name in args.exclude:
            print(f"[skip] {label_dir.name}/ (in --exclude)")
            continue
        csvs = sorted(label_dir.glob("*.csv"))
        if not csvs:
            continue
        out_dir = root_out / label_dir.name
        for path in csvs:
            out_path = out_dir / path.name
            ri, ro, err = normalize_file(path, out_path, mean, std)
            if err:
                print(f"  WARN {path}: {err}")
                continue
            total_files += 1
            total_rows  += ro
        print(f"[label] {label_dir.name}: {len(csvs)} files -> {out_dir}")

    print()
    print(f"normalised {total_files} files, {total_rows} rows")
    print(f"output dir: {root_out.resolve()}")


if __name__ == "__main__":
    main()
