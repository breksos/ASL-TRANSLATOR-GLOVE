// Minimal MPU6050 gyro sanity check.
//
// Purpose: verify a single XIAO + MPU6050 board is alive and reading sane
// gyro values before we commit to the new sensor placement.
//
// Flash to ONE XIAO, open Serial Monitor @ 115200, and rotate the board on
// each axis in turn. You should see the matching gx/gy/gz channel spike to
// hundreds of dps while the other two stay near zero. At rest all three
// should sit within ~1–2 dps of zero.
//
// Pins, I2C speed, and gyro full-scale match asl_finger_sender / pinky_test
// so this also doubles as a wiring check before flashing the real firmware.

#include <Arduino.h>
#include <Wire.h>

static const int      SDA_PIN = 3;  // Waveshare ESP32-S3-Zero: GPIO3 pad
static const int      SCL_PIN = 2;  // Waveshare ESP32-S3-Zero: GPIO2 pad
static const uint32_t I2C_HZ  = 400000;

static uint8_t MPU = 0x68;   // auto-detected (0x68 or 0x69) at boot
static const uint8_t REG_SMPLRT_DIV  = 0x19;
static const uint8_t REG_CONFIG      = 0x1A;
static const uint8_t REG_GYRO_CONFIG = 0x1B;
static const uint8_t REG_PWR_MGMT_1  = 0x6B;
static const uint8_t REG_WHO_AM_I    = 0x75;
static const uint8_t REG_GYRO_XOUT_H = 0x43;

static const float GYRO_LSB_PER_DPS = 65.5f;  // ±500 dps full-scale

static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool mpuRead(uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)MPU, (int)n, (int)true);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool readGyro(float& gx, float& gy, float& gz) {
  uint8_t b[6];
  if (!mpuRead(REG_GYRO_XOUT_H, b, 6)) return false;
  int16_t rgx = (int16_t)((b[0] << 8) | b[1]);
  int16_t rgy = (int16_t)((b[2] << 8) | b[3]);
  int16_t rgz = (int16_t)((b[4] << 8) | b[5]);
  gx = rgx / GYRO_LSB_PER_DPS;
  gy = rgy / GYRO_LSB_PER_DPS;
  gz = rgz / GYRO_LSB_PER_DPS;
  return true;
}

// Probe both possible MPU addresses (0x68 = AD0 low, 0x69 = AD0 high/floating)
// and use whichever ACKs — so the sketch works no matter how AD0 sits, with
// no AD0 soldering needed.
static bool detectMpu() {
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() == 0) { MPU = 0x68; return true; }
  Wire.beginTransmission(0x69);
  if (Wire.endTransmission() == 0) { MPU = 0x69; return true; }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[gyro_test] booting");

  Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
  Wire.setTimeOut(50);

  if (!detectMpu()) {
    Serial.println("FATAL: no MPU at 0x68 or 0x69 — check wiring/power");
    while (1) delay(1000);
  }
  Serial.printf("MPU at I2C 0x%02X\n", MPU);

  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) {
    Serial.println("FATAL: WHO_AM_I read failed");
    while (1) delay(1000);
  }
  Serial.printf("WHO_AM_I = 0x%02X (expect 0x68 or 0x70)\n", who);

  if (!mpuWrite(REG_PWR_MGMT_1, 0x01)) {  // wake, PLL clock
    Serial.println("FATAL: PWR_MGMT_1 write failed");
    while (1) delay(1000);
  }
  delay(10);
  mpuWrite(REG_SMPLRT_DIV,  0x00);
  mpuWrite(REG_CONFIG,      0x05);  // DLPF 10 Hz
  mpuWrite(REG_GYRO_CONFIG, 0x08);  // ±500 dps
  delay(50);

  Serial.println("ready — rotate the board on each axis in turn");
  Serial.println("gx (°/s) | gy (°/s) | gz (°/s)");
}

void loop() {
  float gx, gy, gz;
  if (readGyro(gx, gy, gz)) {
    Serial.printf("%+8.2f | %+8.2f | %+8.2f\n", gx, gy, gz);
  } else {
    Serial.println("read FAIL");
  }
  delay(100);  // 10 Hz print rate — easy to eyeball
}
