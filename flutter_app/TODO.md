# Flutter app — gated-capture rework (TODO)

Status: **planned, not yet implemented.** App currently does continuous
sliding-window inference. We are switching to button-gated, single-shot
capture that mirrors `scripts/record.py` exactly.

## Why this change

Train/inference parity. `record.py` captures one deterministic window per
sample (press → prep → GO → fixed window → STOP → one inference). If the app
captures the same way, the model sees the same data structure at inference
that it saw at training. This also eliminates the sliding-window alignment
problem (dynamic letters flipping to their static twin when motion ends
mid-window) — with gated capture the whole gesture lives inside one window,
exactly like training.

## Target UX

### Letters mode (fingerspelling)
```
[Listen] -> 3-2-1 countdown + GO cue -> capture window -> STOP cue
        -> single letter appended to on-screen buffer
[Listen] -> ... -> next letter appended
...
[Speak]  -> TTS reads the assembled word
[Clear]  -> empties the letter buffer to start a new word
```

### Words mode
```
[Listen] -> 3-2-1 countdown + GO cue -> capture window -> STOP cue
        -> recognized word shown
        -> TTS speaks it automatically (no separate button needed)
```

## Capture timing — MIRROR record.py

- prep: 1.0 s countdown (on-screen "3-2-1" + audible/haptic GO at window start)
- window: 3.0 s == AppConfig.windowTimesteps (75) @ 25 Hz. MUST equal the
  training window. If we ever shorten this, shorten record.py AND retrain.
- After STOP: snapshot the rolling buffer over the captured window, run the
  model ONCE, emit a single Prediction.
- BLE keeps streaming the whole time. The button gates *inference only*, not
  the radio — do not start/stop BLE on each press (avoids reconnect latency).

## Recording-pose protocol (so app capture matches dataset capture)

- Glove is always powered up / calibrated in the **signing-rest position:
  palm facing forward, fingers relaxed**. This is the pitch/roll zero
  reference — must be identical at dataset capture AND at demo inference.
- **Static letters** (A, B, C, ... except J, Z and the dynamic pair members):
  form the letter shape and hold it still for the whole window.
- **Dynamic letters** (J, Z, plus whichever confusable-pair members we mark
  as "moving"): start in the signing-rest position (palm forward) and perform
  the motion during the window.

## Code changes required (do NOT implement yet)

### lib/services/inference_service.dart
- Remove the periodic Timer (`start()` / `stop()` continuous loop).
- Add `Future<Prediction?> captureOnce()`:
  - (optional) prep delay handled by caller / UI
  - collect a full window of frames (windowTimesteps) from the rolling buffer
    starting at the GO instant — i.e. wait `windowDuration`, then snapshot.
  - run the model once, return one Prediction (or null if buffer not full).
- Keep `pushFrame()` filling the rolling buffer continuously from BLE.

### lib/screens/predict_screen.dart
- Replace the predictions-stream listener + threshold/_lastEmittedLabel/
  auto-clear logic with explicit button handlers.
- Add **Listen** button: runs countdown -> GO cue -> captureOnce() ->
  append result.
  - Letters mode: append single label char to `_letterBuffer`.
  - Words mode: show the word and auto-TTS it.
- Add **Speak** button (letters mode only): TTS the assembled `_letterBuffer`.
- Add **Clear** button (letters mode): empties `_letterBuffer` to start a new
  word. (Replaces the old auto-clear-timer behavior; manual control.)
- Show countdown / GO / STOP state visibly (big cue, not subtle) so the user
  knows exactly when to sign. This is the #1 reliability factor — copy the
  feel of record.py's prep beep.

### lib/config/app_config.dart
- Add capture timing constants if not already derivable
  (prepDuration = 1.0 s, windowDuration derived from windowTimesteps / 25 Hz).

### Cosmetic / cleanup
- The SegmentedButton (Letters/Words) stays as the mode switch.
- The continuous "model is predicting" history list can stay as a debug view
  or be removed — decide during implementation.

## Deferred / parked (not required now)
- Per-letter audio confirmation (soft tone or spoken letter on each capture).
  Keep word-only TTS for now.
- Light-sleep power management on the XIAOs (only if heat becomes a problem).
- Re-adding BLE GATT server to the middle-master firmware (task #24) — needed
  before any of this works live; right now master is in CSV-over-USB mode.

## Dependency note
This rework is BLOCKED on task #24 (BLE GATT server on the middle-finger
master). The app can be coded and tested against a mock/replayed stream, but
live demo needs the master broadcasting BLE notifications again. Sequence:
finish dataset + model -> re-add BLE to master -> implement this gated-capture
UI -> live demo.
