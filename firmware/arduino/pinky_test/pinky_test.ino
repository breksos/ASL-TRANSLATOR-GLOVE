// Pinky combined diagnostic + real sender. Arduino fallback for IDF_Ver.
//
// Behaves exactly like asl_finger_sender with FINGER_ID=5, plus prints a
// 5-second window summary over USB (I2C ok/fail %, max consecutive failure
// burst, send ok/cb-fail/imm-fail). Plug the pinky XIAO into the laptop
// while the rest of the glove runs to spot which board is dropping packets.
//
// Patched with the same review fixes as the main sender.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <math.h>

#define FINGER_ID  1
static const uint8_t  ESPNOW_CHANNEL = 11;  // was 6 — moved due to RF congestion
static const uint32_t SEND_PERIOD_US = 20000;

static const int      SDA_PIN = 6;
static const int      SCL_PIN = 7;
static const uint32_t I2C_HZ  = 400000;
static uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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
uint32_t i2cOk = 0, i2cFail = 0;
uint32_t maxBurst = 0, curBurst = 0;
unsigned long lastUs = 0, nextUs = 0;
unsigned long stillSince = 0, windowStart = 0;
bool stillFlag = false;
uint32_t windowIdx = 0;
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

static bool configMpu() {
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) return false;
  Serial.printf("WHO_AM_I = 0x%02X\n", who);
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
  Serial.println("[I2C recovery]");
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
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BROADCAST_MAC, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.printf("\n[Pinky diag, FINGER %u] booting\n", FINGER_ID);

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
  Serial.print("Sender MAC: "); Serial.println(WiFi.macAddress());
  Serial.println("running — diagnostics every 5 s");
  delay((FINGER_ID - 1) * 4);   // phase stagger — widened from 3 ms to reduce collisions
  lastUs = micros();
  nextUs = lastUs;
  windowStart = millis();
}

void loop() {
  // Deadline-scheduled 50 Hz
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
  if (ok) { i2cOk++; curBurst = 0; }
  else    {
    i2cFail++; curBurst++;
    if (curBurst > maxBurst) maxBurst = curBurst;
    Serial.printf("  [%lu ms] I2C FAIL #%lu\n", (unsigned long)millis(), (unsigned long)curBurst);
    if (curBurst > 20) { recoverI2C(); curBurst = 0; }
  }

  if (ok) {
    float dt = (now - lastUs) / 1e6f;
    if (dt <= 0 || dt > 0.05f) dt = (float)SEND_PERIOD_US / 1e6f;
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

    // Duplicate-send: same packet twice with an 800 us gap (see asl_finger_sender).
    if (isfinite(pkt.pitch) && isfinite(pkt.roll) && isfinite(pkt.yaw)) {
      esp_err_t r = esp_now_send(BROADCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
      if (r != ESP_OK) {
        txFailImm++;
        Serial.printf("  [%lu ms] esp_now_send FAIL code=%d\n",
                      (unsigned long)millis(), (int)r);
      }
      delayMicroseconds(800);
      esp_now_send(BROADCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
    }
  }

  // 5 s window summary
  if (millis() - windowStart >= 5000) {
    windowIdx++;
    uint32_t reads = i2cOk + i2cFail;
    uint32_t sends = txOk + txFailCb + txFailImm;
    Serial.printf("---- window %lu (last 5 s) ----\n", (unsigned long)windowIdx);
    Serial.printf("  i2c reads=%lu ok=%lu fail=%lu (%.2f%%) maxBurst=%lu\n",
                  (unsigned long)reads, (unsigned long)i2cOk, (unsigned long)i2cFail,
                  reads ? (100.0f * i2cFail / reads) : 0.0f, (unsigned long)maxBurst);
    Serial.printf("  send ok=%lu cbFail=%lu immFail=%lu (total=%lu)\n",
                  (unsigned long)txOk, (unsigned long)txFailCb,
                  (unsigned long)txFailImm, (unsigned long)sends);
    Serial.printf("  angles: P%+7.2f R%+7.2f Y%+7.2f bias=(%.3f,%.3f,%.3f)\n",
                  fPitch, fRoll, yaw, gxBias, gyBias, gzBias);
    Serial.println();
    i2cOk = i2cFail = 0;
    txOk = txFailCb = txFailImm = 0;
    curBurst = maxBurst = 0;
    windowStart = millis();
  }
}
