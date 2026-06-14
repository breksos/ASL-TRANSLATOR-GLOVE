# Python tooling

Two tiers:

| Tier | Purpose |
|---|---|
| `record.py`, `strip_palm.py` | Primary tools used during normal workflow |
| `diagnostics/*.py` | Link-quality + sensor-health monitors used when something seems off |

All scripts assume:
- Python 3.10+
- `pyserial` installed (`pip install pyserial`)
- Receiver is flashed with `asl_csv_logger` (CSV output, 921 600 baud)
- The repo's `data/` and `data_nopalm/` folders live at the project root

## Primary tools

### `record.py`

```sh
python scripts/record.py --port COM15 --label A
```

Interactive sample recorder. SPACE = record one 3-second window. Type a letter
+ Enter to change label. q = quit. Files land in `data/<label>/<label>_NNN.csv`.

Per-source staleness is checked at end of each window:
- Finger stale > 500 ms → prompt y/n
- Palm stale > 1500 ms → informational note, sample saved

### `strip_palm.py`

```sh
python scripts/strip_palm.py
```

Reads `data/<label>/*.csv` (55 columns: timestamp + 6 sources × 9 fields).
Writes `data_nopalm/<label>/*.csv` (46 columns: timestamp + 5 sources × 9
fields, palm channels removed).

Use after the A/B test verified palm doesn't help accuracy on your data.

## Diagnostics

All take 10-300 seconds; all open COM15 directly (DTR-resets the receiver on
open). Close Arduino Serial Monitor first or they'll fail with a permission
error.

### `diagnostics/sniff.py`

10-second raw capture; writes lines to `_sniff.log` in the project root.
General-purpose "what's actually coming out of the receiver right now".

### `diagnostics/palm_check.py`

20-second per-source health: packet rate, max inter-packet gap, % loss.
Used for "is the link clean right now?"

### `diagnostics/motion_check.py`

30 s test where you wear the glove and perform signs. Reports max gap per
source. Distinguishes static-pose link quality vs motion-induced loss.

### `diagnostics/long_check.py`

5-minute soak using the live-PRY receiver firmware (not CSV).

### `diagnostics/long_csv_check.py`

5-minute soak using the CSV-logger receiver firmware. Per-30-second bucket
packet counts per source. Used to confirm palm survival across a long session.

### `diagnostics/soak_30min.py`

30-minute passive soak (glove on desk, not worn). Identifies thermal /
firmware-state failures distinct from motion-induced RF issues.

### `diagnostics/inspect.py`

```sh
python scripts/diagnostics/inspect.py path/to/data/A
```

Sanity-checks every CSV in a label folder: row count, NaN, all-zero detection,
intra-sample std, across-sample drift. Run after a recording session before
uploading to Edge Impulse.

## Conventions

- All output files (CSVs, logs) go to the project root or `data/` — never to
  `scripts/`.
- COM port is hardcoded to **COM15** in many diagnostics; edit the file or
  pass `--port` where supported.
- No path is relative — they use absolute paths anchored at
  `C:\Users\ASUS\Desktop\Projeler\ASL`. If you move the repo, update them.
