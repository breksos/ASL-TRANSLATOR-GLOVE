// ASL glove — finger SLAVE (XIAO ESP32-C3)
//
// Variant of asl_finger_sender that BROADCASTS its packets; the middle-finger
// MASTER receives them. Used in the "no-DevKit" architecture where the middle
// XIAO bridges 5 fingers to the phone over BLE. Broadcast (vs unicast) avoids
// MAC-layer ACK retries — the cause of ESP_ERR_ESPNOW_NO_MEM and a finger
// dropping to rx=0 at the master — and needs no per-board master MAC.
//
// Flash to thumb (FINGER_ID=1), index (=2), ring (=4), pinky (=5).
// DO NOT flash with FINGER_ID=3 — that's the master.
//
// Setup steps:
//   1. Set FINGER_ID per board (1=thumb, 2=index, 4=ring, 5=pinky).
//   2. Flash, repeat for the four slave fingers. No master MAC needed —
//      slaves broadcast and the master listens.
//
// Period dropped from 20 ms (50 Hz) → 40 ms (25 Hz) to leave headroom on
// the master for BLE notifications without needing light-sleep tricks.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <math.h>

// ---- USER CONFIG ---------------------------------------------------------- 58:8C:81:AD:E1:20
#define FINGER_ID 5                            // 1=thumb 2=index 4=ring 5=pinky
// TX power for this board's ESP-NOW radio. 8.5 dBm has ample margin at the
// few-cm finger-to-master range and roughly halves the ~335 mA TX spike that
// sagged marginal boards into brown-out. Raise a SINGLE board here only if the
// master heartbeat shows that finger losing packets.
#define TX_POWER WIFI_POWER_19_5dBm
// --------------------------------------------------------------------------

// Slaves broadcast (no per-board master MAC needed). Broadcast is
// fire-and-forget: no MAC-layer ACK, hence no ACK-retry queue build-up and
// less radio-on time per packet. The master receives broadcast and unicast
// identically, so the recorded data is unchanged.
static uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static const uint8_t  ESPNOW_CHANNEL = 11;
static const uint32_t SEND_PERIOD_US = 40000;   // 25 Hz

static const int      SDA_PIN = 6;              // D4 on XIAO ESP32-C3
static const int      SCL_PIN = 7;              // D5 on XIAO ESP32-C3
static const uint32_t I2C_HZ  = 400000;

static uint8_t MPU = 0x68;   // auto-detected (0x68 or 0x69) at boot
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

Kalman kPitch, kRoll;
float pitchOff = 0, rollOff = 0;
float gxBias = 0, gyBias = 0, gzBias = 0;
float yaw = 0, fPitch = 0, fRoll = 0;
uint32_t txOk = 0, txFailCb = 0, txFailImm = 0;
uint32_t txConsecFail = 0;   // consecutive esp_now_send failures (radio stuck)
uint32_t i2cOk = 0, i2cFail = 0;
unsigned long lastUs = 0, nextUs = 0;
unsigned long stillSince = 0, windowStart = 0;
bool stillFlag = false;
uint32_t seq = 0;
FingerPacket pkt;

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

// Probe both possible MPU addresses (0x68 = AD0 low, 0x69 = AD0 high/floating)
// and use whichever ACKs — no AD0 soldering needed, and the same firmware runs
// on a board wired either way (e.g. the S3-Zero board reads 0x69).
static bool detectMpu() {
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() == 0) { MPU = 0x68; return true; }
  Wire.beginTransmission(0x69);
  if (Wire.endTransmission() == 0) { MPU = 0x69; return true; }
  return false;
}

static bool configMpu() {
  if (!detectMpu()) return false;   // sets MPU to 0x68 or 0x69
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) return false;
  Serial.printf("MPU @ 0x%02X  WHO_AM_I = 0x%02X\n", MPU, who);
  if (!mpuWrite(REG_PWR_MGMT_1, 0x01)) return false;
  delay(10);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);   // DLPF 10 Hz
  mpuWrite(REG_GYRO_CONFIG, 0x08);
  mpuWrite(REG_ACCEL_CONFIG,0x08);
  delay(50);
  return true;
}

static void recoverI2C() {
  Wire.end();
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(SDA_PIN, INPUT_PULLUP);
  digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
  }
  Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
  Wire.setTimeOut(50);
  configMpu();
}

static bool calibrateStill() {
  const int N = 300;
  double sax=0, say=0, saz=0;
  double sgx=0, sgy=0, sgz=0;
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
  if (gvar > 4.0) { Serial.printf("calib REJECTED gvar=%.2f\n", gvar); return false; }
  rollOff  = atan2f((float)my, (float)mz) * 180.0f / PI;
  pitchOff = atan2f(-(float)mx, sqrtf((float)(my*my + mz*mz))) * 180.0f / PI;
  gxBias = mgx; gyBias = mgy; gzBias = mgz;
  Serial.printf("calib OK gvar=%.3f\n", gvar);
  return true;
}

static void onSent(const wifi_tx_info_t*, esp_now_send_status_t st) {
  if (st == ESP_NOW_SEND_SUCCESS) txOk++; else txFailCb++;
}

static bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setTxPower(TX_POWER);   // see TX_POWER note in USER CONFIG
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST_MAC, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

// Tear down and re-initialise the radio. Called after a run of consecutive
// esp_now_send failures (queue stuck / radio wedged), to self-heal without a
// full reset — mirrors the I2C bus-recovery pattern.
static void recoverEspNow() {
  esp_now_deinit();
  delay(2);
  initEspNow();
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.printf("\n[slave FINGER %u] booting (broadcast mode)\n", FINGER_ID);
  if (FINGER_ID == 3) {
    Serial.println("WARNING: FINGER_ID=3 is reserved for the master. Set 1, 2, 4, or 5.");
  }

  Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
  Wire.setTimeOut(50);

  if (!configMpu()) { Serial.println("FATAL: I2C dead at boot"); while (1) delay(1000); }

  Serial.println("Calibrating, hold still...");
  while (!calibrateStill()) { delay(1000); }

  float ax,ay,az,gx,gy,gz;
  if (readImu(ax,ay,az,gx,gy,gz)) {
    kRoll.angle  = atan2f(ay, az) * 180.0f / PI - rollOff;
    kPitch.angle = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - pitchOff;
  }

  if (!initEspNow()) { Serial.println("FATAL: ESP-NOW init failed"); while (1) delay(1000); }
  Serial.print("Slave MAC: "); Serial.println(WiFi.macAddress());
  Serial.println("running — diagnostics every 5 s");
  delay((FINGER_ID - 1) * 6);   // phase stagger so 4 slaves don't collide
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
  nextUs += SEND_PERIOD_US;
  if ((long)(now - (nextUs + 2 * SEND_PERIOD_US)) > 0) nextUs = now + SEND_PERIOD_US;

  float ax,ay,az,gx,gy,gz;
  bool ok = readImu(ax,ay,az,gx,gy,gz);
  if (ok) { i2cOk++; }
  else    {
    i2cFail++;
    static uint32_t consecFail = 0;
    consecFail++;
    if (consecFail > 20) { recoverI2C(); consecFail = 0; }
    return;
  }

  float dt = (now - lastUs) / 1e6f;
  if (dt <= 0 || dt > 0.1f) dt = (float)SEND_PERIOD_US / 1e6f;
  lastUs = now;

  float gxC = gx - gxBias, gyC = gy - gyBias, gzC = gz - gzBias;
  float aMag = sqrtf(ax*ax + ay*ay + az*az);
  float devG = fabsf(aMag - G_TO_MS2);
  float gMag = sqrtf(gxC*gxC + gyC*gyC + gzC*gzC);
  bool isStill = (devG < STILL_ACC_MS2) && (gMag < STILL_GYRO_DPS);

  if (isStill) {
    gxBias += BIAS_EMA * (gx - gxBias);
    gyBias += BIAS_EMA * (gy - gyBias);
    gzBias += BIAS_EMA * (gz - gzBias);
    if (!stillFlag) { stillFlag = true; stillSince = millis(); }
  } else stillFlag = false;

  float gzd = (fabsf(gzC) < YAW_DEADBAND) ? 0.0f : gzC;
  yaw += gzd * dt;
  if (yaw >  YAW_MAX) yaw =  YAW_MAX;
  if (yaw < -YAW_MAX) yaw = -YAW_MAX;
  if (stillFlag && (millis() - stillSince) > STILL_RESET_MS) yaw = 0.0f;

  float accRoll  = atan2f(ay, az) * 180.0f / PI - rollOff;
  float accPitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - pitchOff;
  if (devG < ACCEL_GATE_MS2) {
    fPitch = kPitch.update(accPitch, gyC, dt);
    fRoll  = kRoll .update(accRoll,  gxC, dt);
  } else {
    fPitch = kPitch.predict(gyC, dt);
    fRoll  = kRoll .predict(gxC, dt);
  }

  pkt.finger_id = FINGER_ID;
  pkt.seq       = ++seq;
  pkt.t_ms      = millis();
  pkt.pitch = fPitch; pkt.roll = fRoll; pkt.yaw = yaw;
  pkt.gx = gxC; pkt.gy = gyC; pkt.gz = gzC;
  pkt.ax = ax;  pkt.ay = ay;  pkt.az = az;

  if (isfinite(pkt.pitch) && isfinite(pkt.roll) && isfinite(pkt.yaw)) {
    esp_err_t r = esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
    if (r != ESP_OK) {
      txFailImm++;
      // A run of failures means the radio TX path is wedged (queue full and
      // not draining). Re-init the radio to self-heal without a reset. No
      // per-event print — the count shows in the 5 s summary, so no log spam.
      if (++txConsecFail >= 25) {
        recoverEspNow();
        txConsecFail = 0;
      }
    } else {
      txConsecFail = 0;
    }
    // Single broadcast send (no dup, no ACK retries) — clean at 25 Hz.
  }

  // 5 s window summary
  if (millis() - windowStart >= 5000) {
    uint32_t reads = i2cOk + i2cFail;
    uint32_t sends = txOk + txFailCb + txFailImm;
    Serial.printf("[%lu] i2c=%lu/%lu(%.1f%% fail) tx ok=%lu cbFail=%lu immFail=%lu  P%+6.1f R%+6.1f Y%+6.1f\n",
                  (unsigned long)millis(),
                  (unsigned long)i2cFail, (unsigned long)reads,
                  reads ? (100.0f * i2cFail / reads) : 0.0f,
                  (unsigned long)txOk, (unsigned long)txFailCb, (unsigned long)txFailImm,
                  fPitch, fRoll, yaw);
    (void)sends;
    i2cOk = i2cFail = 0;
    txOk = txFailCb = txFailImm = 0;
    windowStart = millis();
  }
}
