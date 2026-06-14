# ASL Glove — ESP-IDF version

Ported from the Arduino `.ino` sketches in `ASL_Latest_Ver/` and rewritten with
every fix from the precision/noise review. One PlatformIO-style project per
firmware image; shared MPU6050 + Kalman code lives in `components/glove_common`.

## Requirements

- **ESP-IDF v5.4 or newer** (uses the new `i2c_master` driver and the
  `wifi_tx_info_t` ESP-NOW send-callback signature).
- Python 3.8+, the `esptool`/`idf.py` toolchain (installed by ESP-IDF).
- VS Code with the official **Espressif IDF** extension is the easiest way to
  build/flash from a GUI; everything also works from the IDF shell.

## Projects

| Folder | Target | Role |
|---|---|---|
| `asl_finger_sender` | esp32c3 | One image per finger XIAO. Edit `FINGER_ID` (1..5) in `main/main.c` before each flash. |
| `asl_hand_receiver` | esp32 | DevKit1 on back of hand. Reads palm IMU locally + receives fingers over ESP-NOW. Live PRY @ 5 Hz, stats @ 1 Hz. |
| `asl_csv_logger`    | esp32 | Same wiring as receiver, but emits one CSV row per packet on UART @ 921600 baud. Use for data-loss analysis. |
| `pinky_test`        | esp32c3 | Pinky XIAO with extra USB diagnostics every 5 s (I2C ok/fail, max consecutive failure burst, send ok/fail). Behaves as `FINGER_ID=5` for the rest of the glove. |
| `i2c_scan`          | esp32c3 | Probes every 7-bit I2C address — sanity check the MPU6050 wiring on a XIAO. |
| `print_mac`         | esp32c3 | Prints STA MAC, used to label modules. |

## Build / flash workflow

From the IDF shell (or VS Code → "ESP-IDF: Open ESP-IDF Terminal"):

```sh
cd IDF_Ver/asl_finger_sender
idf.py set-target esp32c3           # only first time, or after changing target
idf.py build
idf.py -p COM7 flash monitor        # adjust COM port
```

Stop the monitor with `Ctrl+]`.

The receiver and CSV logger use `esp32` target:
```sh
cd IDF_Ver/asl_hand_receiver
idf.py set-target esp32
idf.py build
idf.py -p COM15 flash monitor
```

### Per-finger flashing
`FINGER_ID` is a `#define` in `asl_finger_sender/main/main.c`. Change it,
rebuild, flash. Recommended workflow: tape a label with the finger name on
each XIAO before mounting, and keep a list of which MAC → finger so you can
re-flash later without confusion (`print_mac` helps here).

### ESP-NOW channel
All boards must agree on `ESPNOW_CHANNEL` — defined in `asl_finger_sender`,
`pinky_test`, `asl_hand_receiver`, `asl_csv_logger`. Default is 6. If your
home Wi-Fi router is on channel 6 you will see packet loss in the receiver
`[stats]` output; change to 1 or 11 on every board.

## What the optimised firmware does differently

The review went through each issue; this is a compressed map from "fix" to
"where it lives" so you can audit the code.

| Fix from review | Implementation |
|---|---|
| Accel-magnitude gating in Kalman | `if (dev_g < ACCEL_GATE_MS2) kalman_update(...) else kalman_predict(...)` in every sampling loop |
| DLPF 10 Hz (was 21 Hz) | `wreg(m, REG_CONFIG, 0x05)` in [mpu6050.c](components/glove_common/mpu6050.c) |
| I2C @ 400 kHz (was 100 kHz) | `scl_speed_hz = 400000` in `mpu6050.c::open_bus` |
| Deadline scheduler (was `delay(20)`) | `next_us += PERIOD_US; spin until esp_timer_get_time() >= next_us` |
| `dt` fallback fixed | `dt = SEND_PERIOD_US / 1e6f` (was a wrong 0.01s default) |
| Calibration stillness check | `mpu6050_calibrate_still()` computes gyro variance and rejects if it exceeds 4 dps² |
| Average raw accel, not angles | `mpu6050_calibrate_still()` sums `ax,ay,az` then calls `atan2` once |
| Online gyro-bias EMA when still | `cal.gx_bias += BIAS_EMA_ALPHA * (g[0] - cal.gx_bias)` in every loop |
| Yaw reset on sustained stillness | `if (still_since > 0 && (now - still_since) > STILL_RESET_US) yaw = 0` — replaces the old `yaw *= 0.99` |
| Serial @ 921 600 on receivers | `CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600` in `sdkconfig.defaults` |
| Receiver live print slowed to 5 Hz | `last_print > 200 ms` in `asl_hand_receiver` |
| Palm IMU in its own FreeRTOS task | `xTaskCreate(palm_task, ...)` in receiver / CSV logger |
| CSV emit out of ESP-NOW callback | `xQueueSend` from `recv_cb`, `emitter_task` drains and prints |
| Reset-tolerant loss counter | `else if (p.seq < s->seq) { s->lost_count = 0; }` in receiver `recv_cb` |
| TX failure counter on all senders | `s_tx_ok / s_tx_fail_cb / s_tx_fail_imm` in every sender |
| I2C bus recovery | `mpu6050_recover_bus()` — bit-bangs SCL 9 times and reinits the bus |

## Repository layout
```
IDF_Ver/
├── README.md                       ← this file
├── components/
│   └── glove_common/
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── kalman.h
│       │   ├── mpu6050.h
│       │   └── packet.h
│       ├── kalman.c
│       └── mpu6050.c
├── asl_finger_sender/  …
├── asl_hand_receiver/  …
├── asl_csv_logger/     …
├── pinky_test/         …
├── i2c_scan/           …
└── print_mac/          …
```
