"""Sanity-check the saved sample CSVs.
For each file: row count, column count, NaN presence, all-zero cols.
Then aggregate across the label: per-channel mean, std, drift between files.
"""
import csv
import glob
import os
import statistics
import sys

label_dir = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\ASUS\Desktop\Projeler\ASL\data\A"
files = sorted(glob.glob(os.path.join(label_dir, "*.csv")))
print(f"label dir: {label_dir}")
print(f"files: {len(files)}")
if not files:
    sys.exit("no files")

# Per-file checks
bad_rows = 0
bad_cols = 0
nan_files = 0
zero_files = 0
expected_rows = 150
expected_cols = 55     # timestamp + 54 channels
hdr = None
per_file_means = []        # list of 54-vectors
per_file_stds  = []        # list of 54-vectors

for f in files:
    rows = []
    with open(f, newline="") as fp:
        r = csv.reader(fp)
        try:
            this_hdr = next(r)
        except StopIteration:
            print(f"  {os.path.basename(f)}: EMPTY")
            continue
        if hdr is None:
            hdr = this_hdr
        for row in r:
            rows.append(row)
    if len(rows) != expected_rows:
        bad_rows += 1
        print(f"  {os.path.basename(f)}: {len(rows)} rows (expected {expected_rows})")
    if rows and len(rows[0]) != expected_cols:
        bad_cols += 1
        print(f"  {os.path.basename(f)}: {len(rows[0])} cols (expected {expected_cols})")
    if not rows:
        continue
    # parse floats, skip timestamp col
    mat = []
    has_nan = False
    for row in rows:
        try:
            vals = [float(x) for x in row[1:]]
        except ValueError:
            has_nan = True
            continue
        if any(v != v for v in vals):   # NaN check
            has_nan = True
        mat.append(vals)
    if has_nan:
        nan_files += 1
        print(f"  {os.path.basename(f)}: contains NaN/parse error")
    # per-channel mean/std over the 150 timesteps
    if mat:
        ncols = len(mat[0])
        means = [statistics.fmean(col) for col in zip(*mat)]
        stds  = [statistics.pstdev(col) for col in zip(*mat)]
        per_file_means.append(means)
        per_file_stds.append(stds)
        if all(abs(m) < 1e-6 for m in means):
            zero_files += 1
            print(f"  {os.path.basename(f)}: all values ~zero")

print()
print("=" * 60)
print("Summary")
print("=" * 60)
print(f"  files: {len(files)}, expected {expected_rows} rows + 1 header, {expected_cols} cols")
print(f"  short/wrong-row files : {bad_rows}")
print(f"  wrong-col files       : {bad_cols}")
print(f"  files with NaN        : {nan_files}")
print(f"  all-zero files        : {zero_files}")

# Per-channel: variance ACROSS files (how consistent the sign is across reps).
# For a static letter, low intra-sample std + low across-sample variance = stable.
SOURCES = ["thumb","index","middle","ring","pinky","palm"]
FIELDS  = ["P","R","Y","gx","gy","gz","ax","ay","az"]
ch_names = [f"{s}_{f}" for s in SOURCES for f in FIELDS]

print()
print("Per-channel intra-sample std (avg over files) and across-sample drift:")
print(f"  {'channel':<12} {'intra_std':>10} {'across_drift':>14}   notes")
for i, name in enumerate(ch_names):
    intra = statistics.fmean(s[i] for s in per_file_stds)
    means_i = [m[i] for m in per_file_means]
    across = statistics.pstdev(means_i) if len(means_i) > 1 else 0.0
    note = ""
    if intra > 5.0:        note += " HIGH-NOISE"
    if across > 5.0:       note += " INCONSISTENT"
    if intra < 0.01 and across < 0.01: note += " STUCK"
    print(f"  {name:<12} {intra:10.3f} {across:14.3f}   {note}")
