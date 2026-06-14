# ASL Sign Language Glove Translator — Project Brief
**GTU CSE496 Graduation Project | Berk Hakan ÖGE & Utku GÖKÇEK**

---

## What This Project Does
A wearable smart glove that recognizes American Sign Language (ASL) gestures and translates them into spoken text in real time. The user wears the glove, performs a sign, and a companion Flutter mobile app speaks the translated word aloud via TTS.

**Two-layer recognition:**
1. **Word-level gestures** — ~30–50 common ASL signs (hello, yes, no, help, water, etc.) using dynamic IMU motion over time
2. **Fingerspelling fallback** — A–Z static letters for names and unknown words, with context-aware disambiguation (trie-based word completion) for ambiguous letter pairs (U/V, M/N, S/T)

---

## Hardware (All Parts In Hand)

### On the Glove
| Component | Qty | Role |
|---|---|---|
| ESP32 DevKit V1 | 1 | Main MCU — reads all sensors, streams raw data over BLE |
| MPU6050 GY-521 | 6 | 6-axis IMU — 5 on fingers (one per finger) + 1 on back of hand for wrist/global orientation |
| TCA9548A I2C Multiplexer | 1 | Lets all 6 MPU6050s (same I2C address 0x68) coexist on one ESP32 I2C bus |
| Flex Sensor 2.2" | 5 | Analog bend detection per finger — supplements IMU data for stronger dataset |
| CD4051 Analog Multiplexer | 1 | Routes all 5 flex sensor ADC signals into single ESP32 ADC pin |
| Li-Po 3.7V 1000mAh | 1 | Power source |
| TP4056 USB-C charger | 1 | Safe Li-Po charging |

### Mobile
| Component | Role |
|---|---|
| Flutter App (Android/iOS) | BLE receive → ML classification → text buffer → TTS output |

---

## Architecture

```
[Glove Hardware]
  5x Flex Sensors
       │
    CD4051 (analog mux)
       │ (1 ADC pin)
       ▼
  ESP32 DevKit V1  ◄──── 6x MPU6050
       │                     │
       │              TCA9548A (I2C mux)
       │              GPIO21=SDA, GPIO22=SCL
       │
    [BLE]
       │
       ▼
  Flutter Mobile App
       │
   ┌───┴────────────┐
   │  ML Model      │  ← 1D CNN (TFLite) trained via Edge Impulse
   │  Text Buffer   │  ← accumulates letters/words
   │  Trie Engine   │  ← context disambiguation for ambiguous letters
   │  TTS Output    │  ← flutter_tts
   └────────────────┘
```

**Key architectural decisions:**
- ML runs on **mobile, not ESP32** — ESP32 is too weak for real-time model inference
- ESP32 only streams **raw sensor data** over BLE — no classification on device
- TCA9548A eliminates need for a second ESP32 — single board handles all 6 IMUs
- CD4051 is on a **separate analog bus** from TCA9548A — no conflict

---

## ESP32 Pin Mapping

| Pin | Function |
|---|---|
| GPIO21 | I2C SDA → TCA9548A |
| GPIO22 | I2C SCL → TCA9548A |
| GPIO34 (ADC) | CD4051 output (flex sensors) |
| GPIO25 | CD4051 select pin A |
| GPIO26 | CD4051 select pin B |
| GPIO27 | CD4051 select pin C |
| Built-in radio | BLE (no extra pins needed) |

**TCA9548A channels:**
- Channel 0 → MPU6050 (thumb)
- Channel 1 → MPU6050 (index finger)
- Channel 2 → MPU6050 (middle finger)
- Channel 3 → MPU6050 (ring finger)
- Channel 4 → MPU6050 (pinky finger)
- Channel 5 → MPU6050 (back of hand / wrist)
- Channels 6–7 → spare

**CD4051 channels:**
- Channel 0 → Flex sensor (thumb)
- Channel 1 → Flex sensor (index)
- Channel 2 → Flex sensor (middle)
- Channel 3 → Flex sensor (ring)
- Channel 4 → Flex sensor (pinky)

---

## Sensor Data Format

Each sample is a time window of raw readings streamed from ESP32 to mobile over BLE.

**Per timestamp (at 50Hz):**
```
imu[0..5]: { accelX, accelY, accelZ, gyroX, gyroY, gyroZ }  // 6 IMUs × 6 values = 36
flex[0..4]: { raw_adc }                                        // 5 flex values
─────────────────────────────────────────────────────────────
Total per sample: 41 values
At 50Hz over 1s window: 50 × 41 = 2050 values per gesture
```

BLE packet: send as packed float array or JSON — keep under BLE MTU (512 bytes typical). Consider sending at 50Hz as continuous stream and letting mobile handle windowing.

---

## ML Pipeline

### Phase 1 — DTW Baseline (build first, fast to implement)
- Record 1 template per gesture
- At runtime compare live window to all templates using DTW distance
- Classify as closest match
- No training needed — add new gesture by recording one sample
- Use this to validate the full hardware → mobile pipeline works

### Phase 2 — 1D CNN via Edge Impulse (final model)
- Collect 50–100 samples per gesture in Edge Impulse data collector
- Input: time-series window (50 samples × 41 channels)
- Architecture: 1D Conv → Pool → 1D Conv → Pool → Dense → Softmax
- Export as TFLite → integrate into Flutter via `tflite_flutter` package
- Target accuracy: ≥85% letters, ≥90% words

### Gesture vocabulary plan
- **Static letters:** A–Z (fingerspelling)
- **Word gestures (dynamic):** ~30–50 common signs — hello, yes, no, help, water, food, please, sorry, good, bad, I, you, want, need, more, stop, go, thank you, where, what, name, again, understand, know, not, like, want, hurt, sick, call
- **Control gestures:** space (closed fist hold), backspace, switch mode (fingerspell ↔ word)

---

## Flutter App Structure

```
lib/
├── main.dart
├── ble/
│   ├── ble_manager.dart          # flutter_blue_plus — scan, connect, subscribe
│   └── sensor_packet.dart        # parse raw BLE bytes into SensorFrame struct
├── ml/
│   ├── classifier.dart           # load TFLite model, run inference on window
│   ├── dtw_classifier.dart       # DTW baseline (Phase 1)
│   └── gesture_window.dart       # sliding window buffer (1s at 50Hz)
├── text/
│   ├── text_buffer.dart          # accumulate letters → words → sentences
│   ├── trie_engine.dart          # context disambiguation for ambiguous letters
│   └── word_completer.dart       # suggests completions from trie
├── tts/
│   └── tts_manager.dart          # flutter_tts wrapper
└── ui/
    ├── home_screen.dart           # main display — current word, sentence, mode
    └── settings_screen.dart       # BLE device picker, model settings
```

**Flutter dependencies to add:**
```yaml
dependencies:
  flutter_blue_plus: ^1.31.0
  tflite_flutter: ^0.10.4
  flutter_tts: ^3.8.5
  permission_handler: ^11.0.0
```

---

## ESP32 Firmware Structure

```
firmware/
├── src/
│   └── main.cpp
├── lib/
│   ├── TCA9548A/
│   │   └── mux.h              # selectChannel(uint8_t ch), readMPU(uint8_t ch)
│   ├── MPU6050/
│   │   └── imu.h              # initIMU(), readIMU() → struct IMUData
│   ├── FlexSensors/
│   │   └── flex.h             # selectFlex(uint8_t ch), readFlex() → int
│   └── BLE/
│       └── ble_stream.h       # init BLE, sendPacket(float* data, int len)
└── platformio.ini
```

**Key firmware logic:**
```cpp
// Main loop at 50Hz
void loop() {
  for (int ch = 0; ch < 6; ch++) {
    selectMPU(ch);           // TCA9548A switch
    readMPU6050(ch);         // fill imu_data[ch]
  }
  for (int ch = 0; ch < 5; ch++) {
    selectFlex(ch);          // CD4051 switch
    flex_data[ch] = analogRead(ADC_PIN);
  }
  packAndSend();             // BLE notify
  delay(20);                 // 50Hz
}
```

---

## Build Order (Phase by Phase)

### Phase 1 — Hardware Validation (do first)
1. Wire TCA9548A to ESP32 (SDA/SCL)
2. Connect one MPU6050 to TCA9548A channel 0, verify I2C reads
3. Add remaining 5 MPU6050s one at a time, verify each channel
4. Wire CD4051 to ESP32 ADC pin
5. Connect flex sensors through CD4051, verify analog reads
6. Confirm all 11 sensors reading simultaneously at 50Hz via Serial Monitor

### Phase 2 — BLE Streaming
1. Add BLE GATT server to firmware
2. Pack sensor frame as byte array, send via BLE notify characteristic
3. Build minimal Flutter app that connects and prints received packets to console
4. Verify data integrity and latency end-to-end

### Phase 3 — DTW Baseline Classifier
1. Add data logging mode to firmware (dump to Serial as CSV)
2. Record 5–10 gesture templates per sign
3. Implement DTW in Flutter/Dart
4. Test full pipeline: gesture → BLE → classify → display text

### Phase 4 — Dataset Collection + Edge Impulse
1. Set up Edge Impulse project with 41-channel time-series input
2. Collect 50–100 samples per gesture class
3. Train 1D CNN, validate accuracy
4. Export TFLite model
5. Integrate into Flutter via tflite_flutter

### Phase 5 — App Polish
1. Text buffer + trie disambiguation engine
2. TTS output via flutter_tts
3. Word/letter mode switching
4. UI for sentence display

---

## Success Criteria (for TA)
1. Static letter recognition accuracy ≥ 85% across A–Z
2. Word-level gesture accuracy ≥ 90% for defined 30-word vocabulary
3. End-to-end latency (gesture → audio) ≤ 1 second
4. BLE connection stable within 5 seconds of app launch
5. Battery life ≥ 3 hours continuous use

---

## Key Libraries & Tools
| Tool | Purpose |
|---|---|
| PlatformIO | ESP32 firmware development |
| Wire.h | I2C communication (Arduino) |
| MPU6050.h | IMU driver |
| BLEDevice.h | ESP32 BLE server |
| Edge Impulse | Dataset collection + 1D CNN training |
| flutter_blue_plus | BLE client in Flutter |
| tflite_flutter | Run TFLite model in Flutter |
| flutter_tts | Text-to-speech output |

---

## Notes for Claude Code
- Start with Phase 1 hardware validation firmware — get all sensors reading before writing any ML code
- Use PlatformIO, not Arduino IDE — better dependency management
- ESP32 I2C default pins: SDA=GPIO21, SCL=GPIO22
- TCA9548A I2C address: 0x70
- MPU6050 I2C address: 0x68 (all 6 — TCA9548A isolates them)
- CD4051 select pins are digital output (A, B, C = 3-bit binary channel select)
- ADC pin for flex sensors: use GPIO34 (input only, good for ADC)
- BLE MTU on ESP32: negotiate to 512 bytes, send packet every 20ms
- Keep firmware loop simple — no classification logic on ESP32
- All ML, windowing, and text logic lives in Flutter
