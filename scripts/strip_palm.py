"""Copy data/ to data_nopalm/ with the 9 palm columns dropped.
Input  CSV: 55 columns (timestamp + 6 sources × 9 fields)
Output CSV: 46 columns (timestamp + 5 sources × 9 fields) — palm stripped
"""
import csv
import shutil
from pathlib import Path

SRC = Path(r"C:\Users\ASUS\Desktop\Projeler\ASL\data")
DST = Path(r"C:\Users\ASUS\Desktop\Projeler\ASL\data_nopalm")

if not SRC.is_dir():
    raise SystemExit(f"source folder not found: {SRC}")

# Wipe destination if it exists, fresh start
if DST.exists():
    print(f"removing existing {DST}")
    shutil.rmtree(DST)
DST.mkdir(parents=True)

# Number of columns to keep: timestamp + 5 sources × 9 fields = 46
KEEP_COLS = 1 + 5 * 9   # 46

total_files = 0
total_rows = 0
labels_seen = {}

for label_dir in sorted(SRC.iterdir()):
    if not label_dir.is_dir():
        continue
    label = label_dir.name
    out_dir = DST / label
    out_dir.mkdir()
    count = 0
    for csv_file in sorted(label_dir.glob("*.csv")):
        with open(csv_file, newline="") as fin:
            reader = csv.reader(fin)
            with open(out_dir / csv_file.name, "w", newline="") as fout:
                writer = csv.writer(fout)
                for row in reader:
                    if len(row) < KEEP_COLS:
                        # malformed row — keep as-is, will fail validation later
                        writer.writerow(row)
                    else:
                        writer.writerow(row[:KEEP_COLS])
        count += 1
        total_files += 1
    labels_seen[label] = count
    print(f"  {label:10s} : {count} files")
    total_rows += count

print()
print(f"copied {total_files} files into {DST}")
print(f"each CSV reduced from 55 cols (with palm) to {KEEP_COLS} cols (no palm)")

# Sanity-check the header of one output file
sample = next(DST.glob("*/*.csv"), None)
if sample:
    with open(sample) as f:
        header = next(csv.reader(f))
        print(f"\nheader of {sample.name} ({len(header)} cols):")
        print("  " + ",".join(header[:5]) + " , ... , " + ",".join(header[-5:]))
