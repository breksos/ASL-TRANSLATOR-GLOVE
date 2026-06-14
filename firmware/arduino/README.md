# Arduino firmware

Primary firmware tree. Open each project in Arduino IDE 2.x; each subfolder
is one `.ino` sketch that compiles to one board.

## Projects

| Sketch | Target board | Role |
|---|---|---|
| `asl_finger_sender/` | XIAO ESP32-C3 | One per finger. Set `FINGER_ID` 1..5 before flashing each board. |
| `asl_hand_receiver/` | ESP32 DevKit v1 | Receives all 5 fingers via ESP-NOW + local palm IMU. Emits CSV over USB. |
| `asl_csv_logger/` | ESP32 DevKit v1 | Variant of the receiver — pure CSV output at 921 600 baud, no live PRY prints. Used with `scripts/record.py`. |
| `pinky_test/` | XIAO ESP32-C3 | Sender variant that prints diagnostics over USB while still broadcasting. Use to isolate which board is dropping packets. |
| `i2c_scan/` | XIAO ESP32-C3 | Confirms MPU6050 is wired correctly on each finger. |
| `print_mac/` | XIAO ESP32-C3 | Prints the board's STA MAC. Label your boards. |
| `hello_test/` | XIAO ESP32-C3 | Minimal blink/heartbeat sanity check. |

## Flash workflow

1. **Per finger** (5 times, FINGER_ID 1..5):
   - Open `asl_finger_sender/asl_finger_sender.ino`
   - Edit `#define FINGER_ID` near the top to the finger you're flashing
   - Tools → Board → "XIAO_ESP32C3"; Port: pick the right COM
   - Upload, watch the boot log: `[Finger N] starting…`
2. **Receiver**:
   - Open `asl_hand_receiver/asl_hand_receiver.ino` (for normal live use)
   - OR `asl_csv_logger/asl_csv_logger.ino` (for recording sessions)
   - Tools → Board → "ESP32 Dev Module"
   - Upload, open Serial Monitor at **921 600 baud**

## Build settings

| Setting | Value |
|---|---|
| Board family | Arduino-ESP32 core 3.x (Espressif via Boards Manager) |
| Sender CPU freq | default (160 MHz) |
| Receiver CPU freq | default (240 MHz) |
| Partition scheme | default |

## Hardware assumptions (latest)

- ESP-NOW channel: **11** (not the default 6 — channel 6 is RF-congested in the lab)
- Senders broadcast in dup-send mode (each packet twice with 800 µs gap)
- Phase stagger: 4 ms × (FINGER_ID − 1)
- I2C: SDA = GPIO 6, SCL = GPIO 7 on XIAO; SDA = GPIO 21, SCL = GPIO 22 on DevKit1
- Receiver palm: optional (removed in current production design, code still supports it)
- All boards run on a shared 5 V rail from a 5 A bench adapter
- DevKit1 receiver: mounted on back of wrist, antenna end (meander trace) pointing toward fingertips

## Related Python tooling

| Tool | What |
|---|---|
| `scripts/record.py` | Drives `asl_csv_logger` over USB, captures labelled samples |
| `scripts/diagnostics/*` | Link-quality + sensor-health monitors that consume the receiver's USB output |

## See also

- `firmware/idf/README.md` for the ESP-IDF equivalent
- Top-level `README.md` for the system architecture overview
