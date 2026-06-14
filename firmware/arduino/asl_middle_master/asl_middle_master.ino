// ASL glove — middle-finger MASTER (XIAO ESP32-C3)
//
// DUAL-MODE. One compile flag selects the output path:
//
//   #define USE_BLE 1  -> BLE GATT notify to the phone (live inference)
//   #define USE_BLE 0  -> CSV over USB @ 921600 (feed scripts/record.py)
//
// Architecture (same in both modes):
//   - 4 slave XIAOs (thumb=1, index=2, ring=4, pinky=5) ESP-NOW unicast
//     FingerPackets to this master.
//   - Master reads its own MPU at 25 Hz (finger 3 = middle).
//   - Latest packet from each of the 5 fingers is latched.
//
// CSV mode: each packet is printed as a CSV line in record.py's column order.
// BLE mode: every 25 Hz cycle, the 5 latched fingers are packed into a
//           180-byte frame (45 little-endian float32) and pushed over a BLE
//           notify characteristic the Flutter app subscribes to.
//
// BLE payload channel order (must match record.py CSV / stats.json order):
//   finger 1 thumb : P R Y gx gy gz ax ay az   (bytes  0..35)
//   finger 2 index : P R Y gx gy gz ax ay az   (bytes 36..71)
//   finger 3 middle: P R Y gx gy gz ax ay az   (bytes 72..107)  <- own MPU
//   finger 4 ring  : P R Y gx gy gz ax ay az   (bytes 108..143)
//   finger 5 pinky : P R Y gx gy gz ax ay az   (bytes 144..179)
//
// BLE service: 12345678-1234-5678-1234-56789abcdef0
// BLE notify : 12345678-1234-5678-1234-56789abcdef1
// Device name: ASL-Glove
//
// Setup (BLE mode):
//   1. Set USE_BLE 1, flash to the middle XIAO.
//   2. Open Serial Monitor @ 115200, read the "BLE MAC: XX:.." line, paste it
//      into the app's lib/config/hardware_config.dart receiverMac, rebuild app.
//   3. Power the 4 slaves. Connect from the app.
//
// Setup (CSV mode):
//   1. Set USE_BLE 0, flash. Serial Monitor @ 921600.
//   2. Read "# Master MAC:" -> paste into asl_finger_slave MASTER_MAC.
//   3. Run scripts/record.py against the COM port.

#define USE_BLE 0   // <-- 1 = BLE to phone, 0 = CSV to record.py

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <math.h>

#if USE_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#define BLE_DEVICE_NAME  "ASL-Glove"
#define BLE_SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define BLE_NOTIFY_UUID  "12345678-1234-5678-1234-56789abcdef1"
#endif

// ---- USER CONFIG ----------------------------------------------------------
static const uint8_t  MASTER_FINGER_ID = 3;
static const uint8_t  ESPNOW_CHANNEL   = 11;
static const uint32_t CYCLE_US         = 40000;   // 25 Hz
static const uint32_t STALE_MS         = 500;     // finger considered dropped
// --------------------------------------------------------------------------

static const int      SDA_PIN = 6;
static const int      SCL_PIN = 7;
static const uint32_t I2C_HZ  = 400000;

static const uint8_t MPU = 0x68;
static const uint8_t REG_SMPLRT_DIV  = 0x19;
static const uint8_t REG_CONFIG      = 0x1A;
static const uint8_t REG_GYRO_CONFIG = 0x1B;
static const uint8_t REG_ACCEL_CONFIG= 0x1C;
static const uint8_t REG_PWR_MGMT_1  = 0x6B;
static const uint8_t REG_WHO_AM_I    = 0x75;
static const uint8_t REG_ACCEL_XOUT  = 0x3B;
static const float ACCEL_LSB_PER_G  = 8192.0f;
static const float G_TO_MS2         = 9.80665f;
static const float GYRO_LSB_PER_DPS = 65.5f;

static const float    ACCEL_GATE_MS2 = 1.5f;
static const float    STILL_GYRO_DPS = 1.0f;
static const float    STILL_ACC_MS2  = 0.3f;
static const float    YAW_DEADBAND   = 5.0f;
static const float    YAW_MAX        = 180.0f;
static const uint32_t STILL_RESET_MS = 3000;
static const float    BIAS_EMA       = 0.005f;

typedef struct __attribute__((packed)) {
  uint8_t  finger_id;
  uint32_t seq;
  uint32_t t_ms;
  float    pitch, roll, yaw;
  float    gx, gy, gz;
  float    ax, ay, az;
} FingerPacket;

struct SourceState {
  uint32_t lastRxMs = 0;
  uint32_t rxCount  = 0;
  uint32_t lostCount= 0;
  uint32_t seq      = 0;
  FingerPacket pkt  = {};   // latest full packet, for BLE payload build
};
SourceState sources[6];                 // index 1..5
portMUX_TYPE srcLock = portMUX_INITIALIZER_UNLOCKED;

#if !USE_BLE
// CSV dedupe — only emit first copy of each (finger_id, seq) pair.
static uint32_t last_csv_seq[6] = {0};
#endif

// ---- Kalman (own MPU) ----
struct Kalman {
  float Q_angle = 0.001f, Q_bias = 0.003f, R_measure = 0.03f;
  float angle = 0.0f, bias = 0.0f;
  float P[2][2] = {{1,0},{0,1}};
  float predict(float rate, float dt) {
    float r = rate - bias;
    angle += dt * r;
    P[0][0] += dt * (dt*P[1][1] - P[0][1] - P[1][0] + Q_angle);
    P[0][1] -= dt * P[1][1];
    P[1][0] -= dt * P[1][1];
    P[1][1] += Q_bias * dt;
    return angle;
  }
  float update(float meas, float rate, float dt) {
    predict(rate, dt);
    float S = P[0][0] + R_measure;
    float K0 = P[0][0] / S, K1 = P[1][0] / S;
    float y = meas - angle;
    angle += K0 * y; bias += K1 * y;
    float P00 = P[0][0], P01 = P[0][1];
    P[0][0] -= K0 * P00; P[0][1] -= K0 * P01;
    P[1][0] -= K1 * P00; P[1][1] -= K1 * P01;
    return angle;
  }
};
Kalman mkPitch, mkRoll;
float mPitchOff = 0, mRollOff = 0;
float mGxBias = 0, mGyBias = 0, mGzBias = 0;
float mYaw = 0;
uint32_t mSeq = 0;
bool stillFlag = false;
uint32_t stillSince = 0;

#if USE_BLE
BLECharacteristic* gNotifyChar = nullptr;
volatile bool gBleConnected = false;
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    gBleConnected = true;
    Serial.println("# [ble] client connected");
  }
  void onDisconnect(BLEServer* s) override {
    gBleConnected = false;
    Serial.println("# [ble] client disconnected — readvertising");
    s->startAdvertising();
  }
};
#endif

// ---- MPU helpers ----
static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU); Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}
static bool mpuRead(uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(MPU); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)MPU, (int)n, (int)true);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static bool readImu(float& ax, float& ay, float& az,
                    float& gx, float& gy, float& gz) {
  uint8_t b[14];
  if (!mpuRead(REG_ACCEL_XOUT, b, 14)) return false;
  int16_t rax = (int16_t)((b[0]  << 8) | b[1]);
  int16_t ray = (int16_t)((b[2]  << 8) | b[3]);
  int16_t raz = (int16_t)((b[4]  << 8) | b[5]);
  int16_t rgx = (int16_t)((b[8]  << 8) | b[9]);
  int16_t rgy = (int16_t)((b[10] << 8) | b[11]);
  int16_t rgz = (int16_t)((b[12] << 8) | b[13]);
  ax = (rax / ACCEL_LSB_PER_G) * G_TO_MS2;
  ay = (ray / ACCEL_LSB_PER_G) * G_TO_MS2;
  az = (raz / ACCEL_LSB_PER_G) * G_TO_MS2;
  gx = rgx / GYRO_LSB_PER_DPS;
  gy = rgy / GYRO_LSB_PER_DPS;
  gz = rgz / GYRO_LSB_PER_DPS;
  return true;
}
static bool configMpu() {
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) return false;
  Serial.printf("# WHO_AM_I = 0x%02X\n", who);
  if (!mpuWrite(REG_PWR_MGMT_1, 0x01)) return false;
  delay(10);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);
  mpuWrite(REG_GYRO_CONFIG, 0x08);
  mpuWrite(REG_ACCEL_CONFIG,0x08);
  delay(50);
  return true;
}
static bool calibrateStill() {
  const int N = 300;
  double sax=0, say=0, saz=0, sgx=0, sgy=0, sgz=0;
  double sgx2=0, sgy2=0, sgz2=0;
  int ok = 0;
  for (int i = 0; i < N; i++) {
    float ax,ay,az,gx,gy,gz;
    if (!readImu(ax,ay,az,gx,gy,gz)) { delay(5); continue; }
    sax += ax; say += ay; saz += az;
    sgx += gx; sgy += gy; sgz += gz;
    sgx2 += (double)gx*gx; sgy2 += (double)gy*gy; sgz2 += (double)gz*gz;
    ok++; delay(5);
  }
  if (ok < N/2) return false;
  double mx = sax/ok, my = say/ok, mz = saz/ok;
  double mgx = sgx/ok, mgy = sgy/ok, mgz = sgz/ok;
  double gvar = (sgx2/ok - mgx*mgx) + (sgy2/ok - mgy*mgy) + (sgz2/ok - mgz*mgz);
  if (gvar > 4.0) { Serial.printf("# calib REJECTED gvar=%.2f\n", gvar); return false; }
  mRollOff  = atan2f((float)my, (float)mz) * 180.0f / PI;
  mPitchOff = atan2f(-(float)mx, sqrtf((float)(my*my + mz*mz))) * 180.0f / PI;
  mGxBias = mgx; mGyBias = mgy; mGzBias = mgz;
  Serial.printf("# calib OK gvar=%.3f\n", gvar);
  return true;
}

// ---- ESP-NOW receive: latch packet (+ CSV emit in CSV mode) ----
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(FingerPacket)) return;
  FingerPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.finger_id < 1 || p.finger_id > 5) return;
  if (p.finger_id == MASTER_FINGER_ID) return;   // ignore self-reflections

#if !USE_BLE
  uint8_t idx = p.finger_id - 1;
  bool new_seq = (p.seq != last_csv_seq[idx]);
  last_csv_seq[idx] = p.seq;
#endif

  portENTER_CRITICAL(&srcLock);
  SourceState& s = sources[p.finger_id];
  if (s.rxCount > 0) {
    if (p.seq > s.seq + 1) s.lostCount += (p.seq - s.seq - 1);
    else if (p.seq < s.seq) { s.lostCount = 0; }
  }
  s.pkt = p;
  s.seq = p.seq;
  s.lastRxMs = millis();
  s.rxCount++;
  portEXIT_CRITICAL(&srcLock);

#if !USE_BLE
  if (new_seq) {
    Serial.printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  (unsigned long)millis(), (unsigned)p.finger_id,
                  (unsigned long)p.seq, (unsigned long)p.t_ms,
                  p.pitch, p.roll, p.yaw, p.gx, p.gy, p.gz, p.ax, p.ay, p.az);
  }
#endif
}

static bool initWifiEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // TX power lowered from 19.5 dBm to match the slaves and cut the TX current
  // spike that triggers brown-out; the few-cm link has ample margin at 8.5 dBm.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onRecv);
  return true;
}

#if USE_BLE
static void initBle() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);
  gNotifyChar = pService->createCharacteristic(
      BLE_NOTIFY_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  gNotifyChar->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.print("# BLE MAC: ");
  Serial.println(BLEDevice::getAddress().toString().c_str());
  Serial.println("# ^^^ paste into app lib/config/hardware_config.dart receiverMac ^^^");
  Serial.println("# advertising as ASL-Glove");
}

// Build the 180-byte payload (45 LE float32) from the 5 latched fingers.
static void buildPayload(uint8_t out[180]) {
  float* f = reinterpret_cast<float*>(out);   // ESP32 is little-endian
  uint32_t now = millis();
  int o = 0;
  for (uint8_t fid = 1; fid <= 5; fid++) {
    FingerPacket p;
    bool stale;
    portENTER_CRITICAL(&srcLock);
    p = sources[fid].pkt;
    stale = (sources[fid].lastRxMs == 0) || ((now - sources[fid].lastRxMs) > STALE_MS);
    portEXIT_CRITICAL(&srcLock);
    if (stale) {
      for (int k = 0; k < 9; k++) f[o++] = 0.0f;
    } else {
      f[o++] = p.pitch; f[o++] = p.roll; f[o++] = p.yaw;
      f[o++] = p.gx;    f[o++] = p.gy;   f[o++] = p.gz;
      f[o++] = p.ax;    f[o++] = p.ay;   f[o++] = p.az;
    }
  }
}
#endif

// ---- Own-MPU step at 25 Hz: read, Kalman, latch (+ CSV emit in CSV mode) ----
static void ownMpuStep(float dt) {
  float ax,ay,az,gx,gy,gz;
  if (!readImu(ax,ay,az,gx,gy,gz)) return;

  float gxC = gx - mGxBias, gyC = gy - mGyBias, gzC = gz - mGzBias;
  float aMag = sqrtf(ax*ax + ay*ay + az*az);
  float devG = fabsf(aMag - G_TO_MS2);
  float gMag = sqrtf(gxC*gxC + gyC*gyC + gzC*gzC);
  bool isStill = (devG < STILL_ACC_MS2) && (gMag < STILL_GYRO_DPS);

  if (isStill) {
    mGxBias += BIAS_EMA * (gx - mGxBias);
    mGyBias += BIAS_EMA * (gy - mGyBias);
    mGzBias += BIAS_EMA * (gz - mGzBias);
    if (!stillFlag) { stillFlag = true; stillSince = millis(); }
  } else stillFlag = false;

  float gzd = (fabsf(gzC) < YAW_DEADBAND) ? 0.0f : gzC;
  mYaw += gzd * dt;
  if (mYaw >  YAW_MAX) mYaw =  YAW_MAX;
  if (mYaw < -YAW_MAX) mYaw = -YAW_MAX;
  if (stillFlag && (millis() - stillSince) > STILL_RESET_MS) mYaw = 0.0f;

  float accRoll  = atan2f(ay, az) * 180.0f / PI - mRollOff;
  float accPitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - mPitchOff;
  float fP, fR;
  if (devG < ACCEL_GATE_MS2) {
    fP = mkPitch.update(accPitch, gyC, dt);
    fR = mkRoll .update(accRoll,  gxC, dt);
  } else {
    fP = mkPitch.predict(gyC, dt);
    fR = mkRoll .predict(gxC, dt);
  }

  uint32_t now_ms = millis();
  ++mSeq;
  portENTER_CRITICAL(&srcLock);
  SourceState& s = sources[MASTER_FINGER_ID];
  s.pkt.finger_id = MASTER_FINGER_ID;
  s.pkt.seq   = mSeq;
  s.pkt.t_ms  = now_ms;
  s.pkt.pitch = fP; s.pkt.roll = fR; s.pkt.yaw = mYaw;
  s.pkt.gx = gxC; s.pkt.gy = gyC; s.pkt.gz = gzC;
  s.pkt.ax = ax;  s.pkt.ay = ay;  s.pkt.az = az;
  s.seq = mSeq;
  s.lastRxMs = now_ms;
  s.rxCount++;
  portEXIT_CRITICAL(&srcLock);

#if !USE_BLE
  Serial.printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                (unsigned long)now_ms, (unsigned)MASTER_FINGER_ID,
                (unsigned long)mSeq, (unsigned long)now_ms,
                fP, fR, mYaw, gxC, gyC, gzC, ax, ay, az);
#endif
}

unsigned long lastUs = 0, nextUs = 0;
unsigned long windowStart = 0;
uint32_t notifyCount = 0;

void setup() {
#if USE_BLE
  Serial.begin(115200);   // BLE mode: only debug/heartbeat over serial
#else
  Serial.begin(921600);   // CSV mode: high baud to carry 5x25Hz data stream
#endif
  delay(800);
  Serial.printf("\n# asl_middle_master booting (mode=%s)\n",
                USE_BLE ? "BLE" : "CSV");

  Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
  Wire.setTimeOut(50);
  if (!configMpu()) { Serial.println("# FATAL: I2C dead at boot"); while (1) delay(1000); }
  Serial.println("# calibrating own MPU, hold still...");
  while (!calibrateStill()) { delay(1000); }

  float ax,ay,az,gx,gy,gz;
  if (readImu(ax,ay,az,gx,gy,gz)) {
    mkRoll.angle  = atan2f(ay, az) * 180.0f / PI - mRollOff;
    mkPitch.angle = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - mPitchOff;
  }

  if (!initWifiEspNow()) { Serial.println("# FATAL: ESP-NOW init failed"); while (1) delay(1000); }
  Serial.print("# ESP-NOW (WiFi) MAC: "); Serial.println(WiFi.macAddress());
  Serial.println("# ^^^ this is the slaves' MASTER_MAC target ^^^");

#if USE_BLE
  initBle();
#else
  Serial.println("# rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az");
#endif

  lastUs = micros();
  nextUs = lastUs;
  windowStart = millis();
}

void loop() {
  // Deadline-scheduled 25 Hz
  unsigned long now = micros();
  if ((long)(now - nextUs) < 0) {
    long wait = (long)(nextUs - now);
    if (wait > 1500) delay((wait - 1000) / 1000);
    while ((long)(micros() - nextUs) < 0) { }
    now = micros();
  }
  nextUs += CYCLE_US;
  if ((long)(now - (nextUs + 2 * CYCLE_US)) > 0) nextUs = now + CYCLE_US;

  float dt = (now - lastUs) / 1e6f;
  if (dt <= 0 || dt > 0.1f) dt = (float)CYCLE_US / 1e6f;
  lastUs = now;

  ownMpuStep(dt);

#if USE_BLE
  if (gBleConnected && gNotifyChar != nullptr) {
    uint8_t payload[180];
    buildPayload(payload);
    gNotifyChar->setValue(payload, sizeof(payload));
    gNotifyChar->notify();
    notifyCount++;
  }
#endif

  // 5 s heartbeat (comment-prefixed; record.py ignores '#' lines)
  if (millis() - windowStart >= 5000) {
    uint32_t rxC[6], lostC[6];
    portENTER_CRITICAL(&srcLock);
    for (uint8_t i = 1; i <= 5; i++) { rxC[i] = sources[i].rxCount; lostC[i] = sources[i].lostCount; }
    portEXIT_CRITICAL(&srcLock);
#if USE_BLE
    Serial.printf("# alive t=%lu ble=%s notifies=%lu rx=[t:%lu i:%lu m:%lu r:%lu p:%lu]\n",
                  (unsigned long)millis(), gBleConnected ? "conn" : "adv",
                  (unsigned long)notifyCount,
                  (unsigned long)rxC[1], (unsigned long)rxC[2], (unsigned long)rxC[3],
                  (unsigned long)rxC[4], (unsigned long)rxC[5]);
    notifyCount = 0;
#else
    Serial.printf("# alive t=%lu loss=[t:%lu i:%lu m:%lu r:%lu p:%lu] rx=[t:%lu i:%lu m:%lu r:%lu p:%lu]\n",
                  (unsigned long)millis(),
                  (unsigned long)lostC[1], (unsigned long)lostC[2], (unsigned long)lostC[3],
                  (unsigned long)lostC[4], (unsigned long)lostC[5],
                  (unsigned long)rxC[1], (unsigned long)rxC[2], (unsigned long)rxC[3],
                  (unsigned long)rxC[4], (unsigned long)rxC[5]);
#endif
    portENTER_CRITICAL(&srcLock);
    for (uint8_t i = 1; i <= 5; i++) { sources[i].rxCount = 0; sources[i].lostCount = 0; }
    portEXIT_CRITICAL(&srcLock);
    windowStart = millis();
  }
}
