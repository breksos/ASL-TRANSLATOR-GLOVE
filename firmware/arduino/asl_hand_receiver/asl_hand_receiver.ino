// ASL glove hand receiver — ESP32 DevKit1 (back of hand). Arduino fallback.
//
// Patched with the review fixes (see IDF_Ver/README.md for full mapping):
//   - Serial @ 921600 (was 115200) so the live print never blocks
//   - Live PRY print throttled to 5 Hz (was 20 Hz)
//   - Palm IMU runs in its own FreeRTOS task — never jittered by Serial
//   - Source state guarded by portMUX spinlock; reset-tolerant loss counter
//   - Same MPU/Kalman improvements as the senders (DLPF 10 Hz, I2C 400 kHz,
//     accel-magnitude gating, online gyro-bias EMA, yaw reset on stillness)
//
// Wiring of the local MPU on the DevKit1: VCC=3V3, GND=GND, SDA=GPIO21, SCL=GPIO22

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <math.h>

static const uint8_t  ESPNOW_CHANNEL  = 11;  // was 6 — moved due to RF congestion
static const uint8_t  NUM_SOURCES     = 6;
static const uint8_t  PALM_ID         = 6;
static const int      SDA_PIN         = 21;
static const int      SCL_PIN         = 22;
static const uint32_t I2C_HZ          = 400000;
static const uint32_t PALM_PERIOD_US  = 20000;

typedef struct __attribute__((packed)) {
  uint8_t  finger_id;
  uint32_t seq;
  uint32_t t_ms;
  float    pitch, roll, yaw;
  float    gx, gy, gz;
  float    ax, ay, az;
} FingerPacket;

struct SourceState {
  uint32_t seq        = 0;
  uint32_t lastRxMs   = 0;
  uint32_t rxCount    = 0;
  uint32_t lostCount  = 0;
  uint32_t rxLastSec  = 0;
  FingerPacket pkt    = {};
};
SourceState sources[NUM_SOURCES + 1];
portMUX_TYPE srcLock = portMUX_INITIALIZER_UNLOCKED;

// CSV dedupe: senders do dup-send, only emit first copy of each seq to serial.
static uint32_t last_csv_seq[6] = {0};   // index = finger_id - 1 (1..5)

static const char* sourceName(uint8_t id) {
  switch (id) {
    case 1: return "thumb ";
    case 2: return "index ";
    case 3: return "middle";
    case 4: return "ring  ";
    case 5: return "pinky ";
    case 6: return "palm  ";
    default: return "??    ";
  }
}

// ---- Local MPU (palm only) ----
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
  if (gvar > 4.0) {
    Serial.printf("[palm] calib REJECTED gyro_var=%.2f\n", gvar);
    return false;
  }
  palmRollOff  = atan2f((float)my, (float)mz) * 180.0f / PI;
  palmPitchOff = atan2f(-(float)mx, sqrtf((float)(my*my + mz*mz))) * 180.0f / PI;
  palmGxBias = mgx; palmGyBias = mgy; palmGzBias = mgz;
  return true;
}

static void initPalmMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) {
    Serial.println("[palm] MPU not responding — palm disabled");
    return;
  }
  Serial.printf("[palm] WHO_AM_I = 0x%02X\n", who);
  mpuWrite(REG_PWR_MGMT_1,  0x01);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);   // DLPF 10 Hz
  mpuWrite(REG_GYRO_CONFIG, 0x08);
  mpuWrite(REG_ACCEL_CONFIG,0x08);
  delay(100);
  Serial.println("[palm] hold board still, calibrating...");
  while (!calibratePalmStill()) { Serial.println("[palm] retrying"); delay(1000); }
  float ax,ay,az,gx,gy,gz;
  if (readImu(ax,ay,az,gx,gy,gz)) {
    pkRoll.angle  = atan2f(ay, az) * 180.0f / PI - palmRollOff;
    pkPitch.angle = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI - palmPitchOff;
  }
  palmOk = true;
}

// Bit-bang SCL 9x and reinit Wire — recovers from a stuck palm I2C bus.
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

// Palm IMU task — independent 50 Hz deadline scheduler, runs on core 1.
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

    uint32_t now_ms = millis();
    portENTER_CRITICAL(&srcLock);
    SourceState& s = sources[PALM_ID];
    s.pkt.finger_id = PALM_ID;
    s.pkt.seq   = ++pseq;
    s.pkt.t_ms  = now_ms;
    s.pkt.pitch = fP; s.pkt.roll = fR; s.pkt.yaw = yaw;
    s.pkt.gx = gxC; s.pkt.gy = gyC; s.pkt.gz = gzC;
    s.pkt.ax = ax;  s.pkt.ay = ay;  s.pkt.az = az;
    s.seq = pseq;
    s.lastRxMs = now_ms;
    s.rxCount++;
    portEXIT_CRITICAL(&srcLock);

    // Direct CSV emit for palm
    Serial.printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  (unsigned long)now_ms, (unsigned)PALM_ID,
                  (unsigned long)pseq, (unsigned long)now_ms,
                  fP, fR, yaw, gxC, gyC, gzC, ax, ay, az);
  }
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(FingerPacket)) return;
  FingerPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.finger_id < 1 || p.finger_id > 5) return;

  uint8_t idx = p.finger_id - 1;
  bool new_seq = (p.seq != last_csv_seq[idx]);
  last_csv_seq[idx] = p.seq;

  portENTER_CRITICAL(&srcLock);
  SourceState& s = sources[p.finger_id];
  if (s.rxCount > 0) {
    if (p.seq > s.seq + 1) s.lostCount += (p.seq - s.seq - 1);
    else if (p.seq < s.seq) { s.lostCount = 0; s.rxLastSec = 0; }   // sender reset
  }
  s.pkt = p;
  s.seq = p.seq;
  s.lastRxMs = millis();
  s.rxCount++;
  portEXIT_CRITICAL(&srcLock);

  // Direct CSV emit (single printf — atomic on HardwareSerial). No queue
  // because the csv_logger queue/emitter pattern is somehow killing palm.
  if (new_seq) {
    Serial.printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  (unsigned long)millis(), (unsigned)p.finger_id,
                  (unsigned long)p.seq, (unsigned long)p.t_ms,
                  p.pitch, p.roll, p.yaw, p.gx, p.gy, p.gz, p.ax, p.ay, p.az);
  }
}

void setup() {
  Serial.begin(921600);
  delay(500);

  initPalmMpu();
  if (palmOk) {
    // 16 KB stack + pinned to core 0 (same as WiFi callback that does the
    // finger Serial.printf). Two-axis fix:
    //  - 16 KB: Serial.printf with 13 floats can spike high stack usage
    //  - core 0: avoid any cross-core Serial mutex contention between
    //    palm (was core 1) and WiFi-callback Serial output (core 0)
    xTaskCreatePinnedToCore(palmTask, "palm", 16384, NULL, 6, NULL, 0);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  Serial.println("# rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az");
}

void loop() {
  // Quiet main loop. All real output is direct-from-callback / direct-from-palm-task.
  // Heartbeat every 5 s so we can confirm the board's alive without interleaving
  // multi-call prints with the CSV stream.
  static uint32_t lastBeat = 0;
  uint32_t now = millis();
  if (now - lastBeat > 5000) {
    lastBeat = now;
    uint32_t loss[NUM_SOURCES + 1];
    portENTER_CRITICAL(&srcLock);
    for (uint8_t i = 1; i <= NUM_SOURCES; i++) loss[i] = sources[i].lostCount;
    portEXIT_CRITICAL(&srcLock);
    Serial.printf("# alive t=%lu loss=[t:%lu i:%lu m:%lu r:%lu p:%lu]\n",
                  (unsigned long)now,
                  (unsigned long)loss[1], (unsigned long)loss[2],
                  (unsigned long)loss[3], (unsigned long)loss[4],
                  (unsigned long)loss[5]);
  }
  delay(100);
}
