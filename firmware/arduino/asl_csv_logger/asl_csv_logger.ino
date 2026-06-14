// ASL hand receiver — CSV-only mode for data-loss proof. Arduino fallback.
//
// Patched with the review fixes:
//   - CSV emit moved out of the ESP-NOW receive callback. Callback only
//     enqueues to a FreeRTOS queue; a dedicated emitter task drains it.
//     Eliminates the back-pressure that could drop packets in the old code.
//   - Palm IMU in its own task at 50 Hz (independent of emitter).
//   - Same MPU/Kalman improvements as the sender (DLPF 10 Hz, I2C 400 kHz,
//     accel-magnitude gating, online gyro-bias EMA, yaw reset on stillness).
//
// Format: rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az
//
// Pipe COM15 to a file via Python, then post-process to count seq gaps.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <math.h>
#include "freertos/queue.h"

static const uint8_t  ESPNOW_CHANNEL  = 11;  // was 6 — moved due to RF congestion
static const uint8_t  PALM_ID         = 6;
static const int      SDA_PIN         = 21;
static const int      SCL_PIN         = 22;
static const uint32_t I2C_HZ          = 400000;
static const uint32_t PALM_PERIOD_US  = 20000;
static const uint16_t CSV_Q_LEN       = 1024;    // ~3 s of headroom after dedupe

typedef struct __attribute__((packed)) {
  uint8_t  finger_id;
  uint32_t seq;
  uint32_t t_ms;
  float    pitch, roll, yaw;
  float    gx, gy, gz;
  float    ax, ay, az;
} FingerPacket;

typedef struct {
  uint32_t     rx_ms;
  FingerPacket pkt;
} CsvMsg;

static QueueHandle_t csvQ = NULL;
static volatile uint32_t dropped = 0;

// ---- Local palm MPU (same registers/scaling as senders) ----
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

bool palmOk = false;
Kalman pkPitch, pkRoll;
float palmPitchOff = 0, palmRollOff = 0;
float palmGxBias = 0, palmGyBias = 0, palmGzBias = 0;

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

static bool calibratePalmStill() {
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
  if (gvar > 4.0) return false;
  palmRollOff  = atan2f((float)my, (float)mz) * 180.0f / PI;
  palmPitchOff = atan2f(-(float)mx, sqrtf((float)(my*my + mz*mz))) * 180.0f / PI;
  palmGxBias = mgx; palmGyBias = mgy; palmGzBias = mgz;
  return true;
}

static void initPalmMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) return;
  mpuWrite(REG_PWR_MGMT_1,  0x01);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);   // DLPF 10 Hz
  mpuWrite(REG_GYRO_CONFIG, 0x08);
  mpuWrite(REG_ACCEL_CONFIG,0x08);
  delay(100);
  while (!calibratePalmStill()) delay(1000);
  float ax,ay,az,gx,gy,gz;
  if (readImu(ax,ay,az,gx,gy,gz)) {
    pkRoll.angle  = atan2f(ay, az) * 180.0f / PI - palmRollOff;
    pkPitch.angle = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - palmPitchOff;
  }
  palmOk = true;
}

// Drain queue at full speed — keeps recv callback non-blocking.
static void emitterTask(void *arg) {
  CsvMsg m;
  for (;;) {
    if (xQueueReceive(csvQ, &m, portMAX_DELAY) == pdTRUE) {
      Serial.printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                    (unsigned long)m.rx_ms, (unsigned)m.pkt.finger_id,
                    (unsigned long)m.pkt.seq, (unsigned long)m.pkt.t_ms,
                    m.pkt.pitch, m.pkt.roll, m.pkt.yaw,
                    m.pkt.gx, m.pkt.gy, m.pkt.gz,
                    m.pkt.ax, m.pkt.ay, m.pkt.az);
    }
  }
}

// Bit-bang SCL 9x and reinit Wire — recovers from a stuck I2C bus, the same
// recipe the senders use. Called from palm task on bursts of consec failures.
static void recoverPalmI2C() {
  Wire.end();
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(SDA_PIN, INPUT_PULLUP);
  digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
  }
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  mpuWrite(REG_PWR_MGMT_1,  0x01);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);
  mpuWrite(REG_GYRO_CONFIG, 0x08);
  mpuWrite(REG_ACCEL_CONFIG,0x08);
}

// Palm IMU at 50 Hz, enqueues just like remote senders.
static void palmTask(void *arg) {
  uint32_t pseq = 0;
  float yaw = 0.0f;
  unsigned long lastUs = micros();
  unsigned long nextUs = lastUs;
  unsigned long stillSince = 0;
  bool stillFlag = false;
  uint32_t consecFail = 0;

  for (;;) {
    unsigned long now = micros();
    if ((long)(now - nextUs) < 0) {
      long wait = (long)(nextUs - now);
      if (wait > 1500) vTaskDelay(pdMS_TO_TICKS((wait - 1000) / 1000));
      while ((long)(micros() - nextUs) < 0) { }
      now = micros();
    }
    nextUs += PALM_PERIOD_US;
    if ((long)(now - (nextUs + 2 * PALM_PERIOD_US)) > 0) nextUs = now + PALM_PERIOD_US;

    float ax,ay,az,gx,gy,gz;
    if (!readImu(ax,ay,az,gx,gy,gz)) {
      // I2C recovery if hung for 400 ms (20 consecutive failures @ 50 Hz).
      // Without this, a single bad read sequence can stall palm indefinitely
      // — which manifests on the laptop as growing per-sample staleness.
      if (++consecFail > 20) { recoverPalmI2C(); consecFail = 0; }
      continue;
    }
    consecFail = 0;
    float dt = (now - lastUs) / 1e6f;
    if (dt <= 0 || dt > 0.05f) dt = (float)PALM_PERIOD_US / 1e6f;
    lastUs = now;

    float gxC = gx - palmGxBias, gyC = gy - palmGyBias, gzC = gz - palmGzBias;
    float aMag = sqrtf(ax*ax + ay*ay + az*az);
    float devG = fabsf(aMag - G_TO_MS2);
    float gMag = sqrtf(gxC*gxC + gyC*gyC + gzC*gzC);
    bool isStill = (devG < STILL_ACC_MS2) && (gMag < STILL_GYRO_DPS);

    if (isStill) {
      palmGxBias += BIAS_EMA * (gx - palmGxBias);
      palmGyBias += BIAS_EMA * (gy - palmGyBias);
      palmGzBias += BIAS_EMA * (gz - palmGzBias);
      if (!stillFlag) { stillFlag = true; stillSince = millis(); }
    } else stillFlag = false;

    float gzd = (fabsf(gzC) < YAW_DEADBAND) ? 0.0f : gzC;
    yaw += gzd * dt;
    if (yaw >  YAW_MAX) yaw =  YAW_MAX;
    if (yaw < -YAW_MAX) yaw = -YAW_MAX;
    if (stillFlag && (millis() - stillSince) > STILL_RESET_MS) yaw = 0.0f;

    float accRoll  = atan2f(ay, az) * 180.0f / PI - palmRollOff;
    float accPitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - palmPitchOff;
    float fP, fR;
    if (devG < ACCEL_GATE_MS2) {
      fP = pkPitch.update(accPitch, gyC, dt);
      fR = pkRoll .update(accRoll,  gxC, dt);
    } else {
      fP = pkPitch.predict(gyC, dt);
      fR = pkRoll .predict(gxC, dt);
    }

    CsvMsg m;
    m.rx_ms = millis();
    m.pkt.finger_id = PALM_ID;
    m.pkt.seq   = ++pseq;
    m.pkt.t_ms  = m.rx_ms;
    m.pkt.pitch = fP; m.pkt.roll = fR; m.pkt.yaw = yaw;
    m.pkt.gx = gxC; m.pkt.gy = gyC; m.pkt.gz = gzC;
    m.pkt.ax = ax;  m.pkt.ay = ay;  m.pkt.az = az;
    if (xQueueSend(csvQ, &m, 0) != pdTRUE) dropped++;
  }
}

// Dedupe by (finger_id, seq) before queuing. With duplicate-send each packet
// arrives twice; emitting both copies wastes serial bandwidth and overflows the
// queue. Keeping only the FIRST copy of each seq halves the output rate.
static uint32_t last_seq[6] = {0};   // index 0..4 for finger_id 1..5

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(FingerPacket)) return;
  CsvMsg m;
  memcpy(&m.pkt, data, sizeof(m.pkt));
  if (m.pkt.finger_id < 1 || m.pkt.finger_id > 5) return;
  uint8_t idx = m.pkt.finger_id - 1;
  if (m.pkt.seq == last_seq[idx]) return;     // duplicate from dup-send, skip
  last_seq[idx] = m.pkt.seq;
  m.rx_ms = millis();
  if (xQueueSend(csvQ, &m, 0) != pdTRUE) dropped++;
}

void setup() {
  Serial.begin(921600);
  delay(500);
  Serial.println("# rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az");

  csvQ = xQueueCreate(CSV_Q_LEN, sizeof(CsvMsg));
  xTaskCreatePinnedToCore(emitterTask, "csv_emit", 4096, NULL, 5, NULL, 0);

  initPalmMpu();
  if (palmOk) xTaskCreatePinnedToCore(palmTask, "palm", 4096, NULL, 6, NULL, 1);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init failed"); while (1) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
}

void loop() {
  // Periodic drop-counter line, prefixed with '#' so parsers skip it.
  static uint32_t lastStats = 0;
  if (millis() - lastStats > 5000) {
    lastStats = millis();
    uint32_t d = dropped; dropped = 0;
    Serial.printf("# dropped_last_5s=%lu queue_waiting=%u\n",
                  (unsigned long)d, (unsigned)uxQueueMessagesWaiting(csvQ));
  }
  delay(100);
}
