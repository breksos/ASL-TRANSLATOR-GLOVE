# ASL Glove — Mobile App

Companion Flutter app for the ASL Sign Language Glove Translator.
**GTU CSE496 graduation project** by Berk Hakan ÖGE & Utku GÖKÇEK.

The glove streams hand-pose data over Bluetooth LE; this app receives the
stream, runs a TFLite model on a 2-second rolling window, and speaks the
recognized sign via TTS.

> **The only things you need to configure:** the receiver's BLE MAC address
> and which model is active (Letters or Words). Everything else has sane defaults.

---

## Quickstart

```sh
cd flutter_app
# First-time only: scaffold the Android/iOS native code (gradle, manifests, etc.)
# This populates everything the platform needs around our Dart code.
flutter create --org com.gtu.aslglove --project-name asl_glove .

# When prompted "AndroidManifest.xml already exists" — say YES to overwrite (we
# have a custom one with BLE permissions). For other "exists" prompts, NO is safe.

flutter pub get
flutter run
```

In the app:
1. **Settings tab** → enter the receiver MAC → pick Letters or Words → **Save**.
2. **Predict tab** → **Connect** → perform a sign → see prediction + hear TTS.

If you see "Model not loaded": you haven't put your `.tflite` files in
`assets/models/` yet. See [Adding the model](#adding-the-model) below.

---

## Prerequisites

| Tool | Version | Why |
|---|---|---|
| Flutter SDK | 3.16 or newer | App build |
| Android SDK | API 33 (Android 13) target | Build target |
| A phone | Android 8+ with BLE | Runtime |
| USB cable | For `flutter run` deployment | Or just `flutter build apk` and sideload |

iOS works in principle (deps support it) but we didn't test it; build at your
own risk.

---

## One-time setup

### 1. Get Flutter

```sh
# Windows
choco install flutter
# or download from https://flutter.dev and add to PATH

flutter doctor   # confirm Android toolchain is happy
```

### 2. Fetch dependencies

```sh
cd flutter_app
flutter pub get
```

### 3. Add your trained model

Place these files in `assets/models/`:

| Filename | What |
|---|---|
| `asl-letters.tflite` | Letters model exported from Edge Impulse |
| `asl-letters-labels.txt` | One class label per line, same order as trained |
| `asl-words.tflite` | Words model |
| `asl-words-labels.txt` | One word label per line |

**Export from Edge Impulse:**
1. EI project → Sidebar → **Deployment** → **TensorFlow Lite** → **Build**.
2. Download the zip, extract.
3. Rename `model.tflite` → `asl-letters.tflite` (or `asl-words.tflite`).
4. The labels are inside the export; put them in the labels file.

If your model expects a different number of channels than 45 (palm dropped),
also update `AppConfig.channelCount` in `lib/config/app_config.dart` AND the
receiver firmware.

### 4. Add normalization stats (recommended)

Edge Impulse normalizes inputs with `StandardScaler`. To match at inference:

1. From your EI project, get the per-channel mean and std of the training set.
   - Easiest: in EI's Raw Data block, after "Generate features" check the
     processing block's output stats, or extract from the project export.
   - Or compute yourself from `data_nopalm/` using a short Python script:
     ```python
     import numpy as np, glob, csv
     vals = []
     for f in glob.glob('data_nopalm/*/*.csv'):
         with open(f) as h:
             reader = csv.reader(h); next(reader)  # skip header
             for row in reader:
                 vals.append([float(x) for x in row[1:]])  # skip timestamp
     arr = np.array(vals)   # (N, 45)
     print('mean:', arr.mean(axis=0).tolist())
     print('std:',  arr.std(axis=0).tolist())
     ```
2. Edit `assets/normalization/stats_template.json`:
   - Replace the 45 zeros in `"mean"` with your actual means.
   - Replace the 45 ones in `"std"` with your actual stds.
3. Save as `assets/normalization/stats.json` (drop `_template` from the name).

If `stats.json` is missing the app still runs but accuracy degrades — a warning
appears in the debug log.

### 5. Find the receiver's MAC address

When the DevKit1 receiver boots, it advertises a BLE service. To find its MAC:

**Easiest:** flash any version of the receiver firmware that prints the MAC on
boot (current `asl_hand_receiver.ino` does this). Watch the serial monitor —
look for `Receiver MAC: XX:XX:XX:XX:XX:XX` near the top.

**Alternative:** use any BLE scanner app on your phone (e.g. **nRF Connect**)
to find a device named `ASL_Glove*`. Its address is the MAC.

---

## Inspecting in VS Code (no phone needed)

If you just want to look at the code, verify it compiles, and navigate
without running anything:

### Setup

1. Install the **Flutter** VS Code extension (by Dart Code) — adds Dart
   support, debugger integration, hot-reload, IntelliSense.
2. **File → Open Folder** → select `flutter_app/`.
3. In a terminal:
   ```sh
   flutter create --org com.gtu.aslglove --project-name asl_glove .
   flutter pub get
   ```
   (`flutter create` scaffolds the Android/iOS native code around our Dart
   code; only needed once. Answer **YES** to the AndroidManifest.xml
   overwrite prompt, **NO** to other "exists" prompts.)

### Static analysis (no device required)

```sh
flutter analyze
```

Catches missing imports, unused variables, deprecated APIs, type mismatches
across all `lib/*.dart`. If it passes clean, the code is at least
syntactically and type-correct.

### Navigation shortcuts in VS Code

| Shortcut | What |
|---|---|
| `Ctrl+P` then filename | Quick-open |
| Hover on a symbol | Inline docs + type info |
| `F12` | Jump to definition |
| `Shift+F12` | Find all usages |
| `Ctrl+Shift+O` | Outline of current file |
| `F5` | Run with debugger |

Start in `lib/main.dart` and Ctrl-click through the service constructors —
that's how the dataflow is wired.

## Running

### Via USB on a real Android phone

1. Phone → Settings → About → tap Build number 7× → developer mode on
2. Phone → Settings → Developer options → enable **USB debugging**
3. Connect to laptop via USB, accept the RSA fingerprint prompt
4. In VS Code, bottom-right status bar should show the phone's name
5. Press **F5** or run:
   ```sh
   flutter run
   ```

Hot-reload works during dev — save a Dart file and the app updates without
restart.

### Via Android emulator (slower, no real BLE)

In Android Studio → Tools → Device Manager → create a virtual device with
API 33+. Then `flutter run` picks it up. Useful for verifying UI layout —
not useful for testing BLE (emulator's BLE is fake).

### Via Chrome (UI only, no BLE, no TFLite)

```sh
flutter run -d chrome
```

App loads in your browser. BLE is unavailable (no Web Bluetooth API in this
codebase). Useful only as a quick smoke check that widgets render.

### Build a release APK

```sh
flutter build apk --release
# Output: build/app/outputs/flutter-apk/app-release.apk
adb install build/app/outputs/flutter-apk/app-release.apk
```

### What you'll see on first run (before any model file is present)

- **Predict tab**: "Model not loaded" message in place of the prediction.
  The big text and history are blank. The BLE status pill at the top right
  shows "Not connected".
- **Settings tab**: empty MAC field, model dropdown defaults to "Letters
  (A–Z)", confidence slider at 70 %, TTS on. Save still works — the next
  start picks up the saved values.

Both of those resolve once you drop your `.tflite` files in `assets/models/`
(see [Adding the model](#3-add-your-trained-model) below) and once the
receiver firmware's BLE bridge is in place.

---

## In-app usage

1. **Open the app.** First-run: Settings tab will be empty.
2. **Enter receiver MAC** in `AA:BB:CC:DD:EE:FF` format. The field validates;
   it errors on Save if malformed.
3. **Pick active model** — Letters (A–Z) or Words.
4. **Adjust confidence threshold** (default 0.7). Lower = more predictions,
   higher = more conservative. Predictions below threshold appear in the
   history but aren't spoken.
5. **Pick TTS language** (default English US). Turkish supported.
6. **Tap Save.** The selected model loads; expect a toast confirmation.
7. **Predict tab → Connect button.** The app scans for the MAC, connects,
   subscribes to the sensor characteristic, and starts emitting predictions.
8. **Perform signs.** In **Letters mode**, individual letters accumulate into
   a text buffer that clears after the configured idle timeout (or manual
   clear). In **Words mode**, each recognized word is spoken immediately.

### Switching modes

In Settings, change the dropdown and tap Save. The previous model is unloaded
and the new one loaded — takes ~1 second. No app restart needed.

---

## Project structure

```
flutter_app/
├── README.md                              ← you are here
├── pubspec.yaml                           Dart dependencies
├── android/
│   └── app/src/main/AndroidManifest.xml   BLE + internet permissions
├── lib/
│   ├── main.dart                          App entry, service wiring
│   ├── config/
│   │   ├── app_config.dart                window size, sample rate, etc.
│   │   └── ble_protocol.dart              BLE UUIDs + channel layout
│   ├── models/
│   │   ├── sensor_frame.dart              One 45-float timestep
│   │   ├── prediction.dart                Model output (label + confidence)
│   │   └── ble_connection_state.dart      Connection state enum
│   ├── services/
│   │   ├── ble_service.dart               BLE scan/connect/subscribe
│   │   ├── inference_service.dart         TFLite loading + inference loop
│   │   ├── tts_service.dart               flutter_tts wrapper, dedup
│   │   └── preferences_service.dart       SharedPreferences for settings
│   ├── utils/
│   │   ├── rolling_buffer.dart            Ring buffer of N timesteps
│   │   ├── normalizer.dart                StandardScaler equivalent
│   │   └── packet_parser.dart             BLE payload → SensorFrame
│   ├── screens/
│   │   ├── home_screen.dart               Bottom-nav shell
│   │   ├── predict_screen.dart            Live prediction display
│   │   └── settings_screen.dart           User-facing configuration
│   ├── widgets/
│   │   ├── prediction_display.dart        Big text + confidence bar
│   │   ├── ble_status_widget.dart         Connection state pill
│   │   └── confidence_bar.dart            Coloured progress bar
│   └── theme/
│       └── app_theme.dart                 Material 3 light/dark theme
└── assets/
    ├── models/
    │   ├── README.txt                     instructions
    │   ├── asl-letters.tflite             ← YOU PROVIDE
    │   ├── asl-letters-labels.txt         ← YOU PROVIDE
    │   ├── asl-words.tflite               ← YOU PROVIDE
    │   └── asl-words-labels.txt           ← YOU PROVIDE
    └── normalization/
        ├── stats_template.json            example
        └── stats.json                     ← YOU PROVIDE (rename from template)
```

---

## How the data flow works

```
DevKit1 receiver         Phone (this app)
─────────────────         ────────────────
BLE notify @ 50 Hz   →   BleService           (receives 180 bytes per packet)
                            ↓
                         PacketParser         (180 bytes → 45 floats)
                            ↓
                         SensorFrame          (45 floats + timestamp)
                            ↓
                         InferenceService.pushFrame(...)
                            ↓
                         RollingBuffer        (last 100 timesteps)
                            ↓ every 200 ms
                         Normalizer           (StandardScaler from stats.json)
                            ↓
                         TFLite Interpreter   ([1, 100, 45] → [1, num_classes])
                            ↓
                         argmax + softmax     → Prediction(label, confidence)
                            ↓
                         Predict screen        + TtsService.speakIfNew()
```

---

## BLE protocol expected from the receiver

The app subscribes to one notify characteristic. The receiver firmware MUST
implement:

| Property | Value |
|---|---|
| Service UUID | `12345678-1234-5678-1234-56789abcdef0` |
| Notify char UUID | `12345678-1234-5678-1234-56789abcdef1` |
| Notification size | 180 bytes (45 little-endian float32) |
| Notification rate | 50 Hz (one per 20 ms) |
| Channel order | thumb (P,R,Y,gx,gy,gz,ax,ay,az), index (same), middle, ring, pinky |
| Advertised name | starts with `ASL_Glove` (used as fallback when MAC scan fails) |

This is independent of the original ESP-NOW protocol used between glove
fingers and the DevKit1. The DevKit1 bridges ESP-NOW → BLE.

The receiver firmware for this BLE bridge is tracked separately (task #24 in
the project's TaskList). When that firmware is flashed, this app will work
end-to-end.

---

## Adding a new model later

Say you want a third model (e.g. for sentence-level classification):

1. Drop `asl-sentences.tflite` + `asl-sentences-labels.txt` into `assets/models/`.
2. In `lib/services/preferences_service.dart`:
   ```dart
   static const String modelSentences = 'sentences';
   ```
3. In `lib/screens/settings_screen.dart`, add a dropdown entry:
   ```dart
   DropdownMenuItem(
     value: PreferencesService.modelSentences,
     child: Text('Sentences'),
   ),
   ```
4. Save. The runtime will load `assets/models/asl-sentences.tflite` and
   `assets/models/asl-sentences-labels.txt` automatically (the path is
   computed from the model key).

---

## Troubleshooting

### "Bluetooth permission required"
- Android Settings → Apps → ASL Glove → Permissions → Bluetooth → Allow.
- Or grant Location (legacy) for Android < 12.

### "Turn on Bluetooth"
- Self-explanatory. Pull down the Quick Settings panel.

### "Receiver not found"
- Receiver isn't powered on, or
- MAC address typo (Settings → re-enter), or
- Receiver too far away (BLE range ~5-10 m indoors), or
- Receiver firmware doesn't have the BLE bridge yet (see task #24).

### "Model not loaded — place asl-letters.tflite in assets/models/"
- You skipped the [Adding the model](#3-add-your-trained-model) step.
- Filename mismatch: must be exactly `asl-letters.tflite` / `asl-words.tflite`.

### Predictions look random
- **Normalization mismatch.** Did you populate `stats.json` with values from
  your training? If not, the model sees raw values; output is garbage.
- **Wrong channel count.** Model expects 45 channels (no palm). Confirm both
  `AppConfig.channelCount` and your EI impulse use 45.
- **Confidence threshold too low.** Slide it up to 0.85 in Settings; only
  high-confidence predictions get spoken.

### TTS not speaking
- TTS toggle in Settings is off.
- Phone volume too low (system or media volume).
- Selected language not installed on the phone (Android Settings →
  Accessibility → Text-to-speech).

### Inference is slow / app freezes briefly
- Run a release build (`flutter run --release` or build APK).
- Make sure you're loading the quantized int8 model (smaller, faster).
- If still slow, lower `AppConfig.inferenceInterval` to 100 ms or higher.

### "Bluetooth not supported on this device"
- Phone is too old / lacks BLE. Test on a newer phone.

---

## Known limitations

- **Single-glove model** — the app assumes one-handed ASL. Two-handed signs
  not supported by the underlying glove hardware.
- **Quantization sensitivity** — if your model loses > 5 % accuracy in int8,
  switch to float32 export in EI's Deployment. Larger file, marginal phone perf hit.
- **No offline word completion / autocomplete** — Letters mode just accumulates
  raw letters. The trie engine planned for later (task #25) adds context-aware
  disambiguation for U/V, M/N, S/T.
- **No data logging** — predictions aren't persisted across app restarts.
- **BLE only (no Wi-Fi fallback)** — receiver must be in BLE range.

---

## Dev notes

### Hot reload tips
- Changing settings live: tap Save, hot-reload, settings persist (they're in
  SharedPreferences).
- Changing the BLE UUIDs: edit `lib/config/ble_protocol.dart`, hot-restart
  (not just reload) to re-register subscriptions.
- Adding a new model: drop in assets, edit `pubspec.yaml` if needed (it's
  already wildcard-included for `assets/models/`), hot-restart.

### Running without a real receiver (sim mode)
The app doesn't include a built-in simulator. To test the inference path
without hardware:
- Modify `lib/services/ble_service.dart` to add a Timer that emits synthetic
  `SensorFrame`s with random values. Wire it up in `main.dart`.
- Or write integration tests that feed `inference_service.pushFrame()` directly.

### Code style
- All Dart code follows the official Flutter style guide.
- `dart format` on every file.
- Public methods have dartdoc comments.
- No raw `print()` calls; use `debugPrint` (stripped in release).

---

## License & credits

GTU CSE496 graduation project, May–June 2026.

Built with: `flutter_blue_plus`, `tflite_flutter`, `flutter_tts`, `permission_handler`,
`shared_preferences`. See `pubspec.yaml` for versions.

Receiver firmware in the parent repo (`firmware/arduino/` and `firmware/idf/`).
