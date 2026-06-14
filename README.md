# ASL Sign Language Glove Translator

**GTU CSE496 Graduation Project** — Berk Hakan ÖGE & Utku GÖKÇEK, May–June 2026.

A wearable smart glove that recognises American Sign Language (ASL) gestures
and translates them into spoken text in real time. The user wears the glove,
performs a sign, and a Flutter mobile app speaks the translated word aloud
via TTS.

For the original project brief see [GLOVE_PROJECT.md](GLOVE_PROJECT.md).

---

## Repository layout

```
ASL/
├── README.md                ← you are here
├── GLOVE_PROJECT.md         original brief / spec
│
├── firmware/                ESP32 firmware (sender + receiver + diagnostics)
│   ├── arduino/             Arduino IDE .ino sketches (primary development path)
│   └── idf/                 ESP-IDF projects (parallel alternative; same logic)
│
├── hardware/
│   └── ASL_Glove/           KiCad PCB design for the prototype board
│
├── flutter_app/             Mobile app — BLE receive + TFLite inference + TTS
│
├── scripts/                 Python tooling
│   ├── record.py            primary data-recording UI
│   ├── strip_palm.py        create no-palm variant of dataset
│   └── diagnostics/         link-quality and sensor-health diagnostics
│       ├── sniff.py
│       ├── palm_check.py
│       ├── motion_check.py
│       ├── long_check.py
│       ├── long_csv_check.py
│       ├── soak_30min.py
│       └── inspect.py
│
├── data/                    labelled training samples (with palm channels)
├── data_nopalm/             same samples, palm stripped (45 channels)
│
└── docs/
    ├── datasheets/          part datasheets (MPU6050, ESP32-C3, etc.)
    └── images/              project photos, diagrams
```

---

## System architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                              The Glove                              │
│                                                                     │
│  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐         │
│  │ thumb  │  │ index  │  │ middle │  │  ring  │  │ pinky  │         │
│  │ XIAO+  │  │ XIAO+  │  │ XIAO+  │  │ XIAO+  │  │ XIAO+  │         │
│  │ MPU6050│  │ MPU6050│  │ MPU6050│  │ MPU6050│  │ MPU6050│         │
│  └───┬────┘  └───┬────┘  └───┬────┘  └───┬────┘  └───┬────┘         │
│      │ ESP-NOW broadcast, channel 11, 50 Hz, dup-send               │
│      └────────────┬───────────┴───────────┴───────────┘             │
│                   ▼                                                 │
│         ┌────────────────────┐                                      │
│         │  ESP32 DevKit1     │                                      │
│         │  (back of wrist)   │                                      │
│         └─────┬──────────────┘                                      │
└───────────────┼─────────────────────────────────────────────────────┘
                │
                │  USB Serial (CSV) for recording sessions
                │  OR
                │  BLE GATT notify (45 floats × 50 Hz) for phone
                ▼
        ┌───────────────┐               ┌──────────────────────┐
        │   record.py   │               │   Flutter app        │
        │  (laptop)     │               │   (Android phone)    │
        │   → CSV files │               │   TFLite + TTS       │
        └───────┬───────┘               └──────────────────────┘
                ▼
        ┌──────────────────┐
        │  Edge Impulse    │
        │  CNN training    │
        │  → .tflite       │
        └──────────────────┘
                │
                └────────────► flutter_app/assets/models/
```

---

## Quick-start by role

### "I want to flash the firmware"

```sh
# Arduino — primary path
cd firmware/arduino
# Open the project you want in Arduino IDE 2.x (e.g. asl_finger_sender)
# Set FINGER_ID 1..5 per board, flash to each XIAO ESP32-C3
# Flash asl_hand_receiver to the DevKit1
```

Details: see [firmware/arduino/README.md](firmware/arduino/README.md).

For the IDF equivalent: see [firmware/idf/README.md](firmware/idf/README.md).

### "I want to record training data"

```sh
pip install pyserial
python scripts/record.py --port COM15 --label A
```

Press SPACE to record a sample, type a letter to change label, q to quit.
Saves `data/<label>/<label>_NNN.csv`. See `python scripts/record.py --help`.

### "I want to diagnose link health"

```sh
# Quick 20 s per-source rate + max gap
python scripts/diagnostics/palm_check.py

# 30 s while the user signs — checks for motion-induced loss
python scripts/diagnostics/motion_check.py

# 30 min passive soak — checks thermal/firmware stability
python scripts/diagnostics/soak_30min.py

# 5 min CSV-mode soak (asl_csv_logger firmware)
python scripts/diagnostics/long_csv_check.py
```

### "I want to train a model"

1. Record dataset to `data/<label>/*.csv` via `record.py`
2. (Optional) Strip palm channels: `python scripts/strip_palm.py` produces `data_nopalm/`
3. Upload to Edge Impulse with `edge-impulse-uploader --category split --label <X> data/<X>/*.csv` per class
4. Configure impulse: window 2000 ms, stride 500 ms, 50 Hz, **Raw Data**, **Classification (Keras)**
5. Normalize features: **scikit-learn StandardScaler**
6. Use the 1D CNN architecture (see `flutter_app/README.md` for the Keras code)
7. Train, evaluate on Model Testing tab
8. Deployment → TensorFlow Lite → download

### "I want to run the mobile app"

See [flutter_app/README.md](flutter_app/README.md) for full setup. The summary:

1. `cd flutter_app && flutter create --org com.gtu.aslglove --project-name asl_glove .`
2. Drop `asl-letters.tflite`, `asl-words.tflite` + labels into `assets/models/`
3. Drop normalization `stats.json` into `assets/normalization/`
4. `flutter run`

---

## Project status checklist

| Component | Status |
|---|---|
| Sender firmware (Arduino + IDF) | ✅ working, current |
| Receiver firmware (USB CSV path) | ✅ working |
| Receiver firmware (BLE bridge for phone) | ⏳ pending |
| Data recorder (`record.py`) | ✅ working |
| Diagnostic scripts | ✅ working |
| Smoke dataset (5 signs × 30) | ✅ collected |
| Smoke model (1D CNN, no-palm) | ✅ 87.7 % test accuracy |
| Letter dataset (A–Z × 60, multi-session) | ⏳ pending |
| Word dataset (15–20 signs × 60) | ⏳ pending |
| Flutter app skeleton | ✅ written, awaiting model file |
| Trie disambiguation engine | ⏳ pending |

---

## Key engineering decisions

- **Distributed ESP-NOW**, not a wired I2C mux — each finger XIAO transmits independently. Eliminates long wires across joints.
- **Channel 11**, not the default channel 6 — local 2.4 GHz neighbours saturate channel 6.
- **Duplicate-send on every sender** — each packet broadcast twice 800 µs apart. Receiver dedupes by `seq`. Roughly squares the loss rate.
- **External U.FL antennas** instead of the XIAO's PCB chip antenna — measured 10–15× link-loss improvement.
- **Palm MPU dropped** after A/B testing showed identical overall accuracy and *improved* NO class F1 without it. 45-channel input instead of 54.
- **1D CNN architecture**, not the default MLP — boosted test accuracy from 55 % to 88 % on fresh recordings.
- **Mobile-side ML** — ESP32 lacks compute for real-time CNN; phone runs the model.

---

## Open tasks (high-level)

Tracked in detail in the project's task list. Headline items:

1. Reassemble the glove with brick-pattern antenna layout + tight compression glove + clean power distribution
2. Multi-session data collection (4 sessions × 15 samples per sign)
3. Train production Letters and Words models
4. Add BLE GATT server to receiver firmware
5. Implement Trie engine for U/V, M/N, S/T disambiguation in letter mode

---

## License

Academic project, GTU CSE496. Use is permitted for educational reference;
contact authors for commercial use.
