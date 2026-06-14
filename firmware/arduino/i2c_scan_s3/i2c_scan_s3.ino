// i2c_scan_s3 — find the MPU on the Waveshare ESP32-S3-Zero, regardless of
// which way SDA/SCL are wired or what address the chip uses.
//
// It scans the bus with BOTH pin orderings and every address, so it answers:
//   - is the device wired/powered at all?  (any ACK = yes)
//   - which GPIO is SDA and which is SCL?   (whichever ordering finds it)
//   - what is its address?                  (0x68 = AD0 low, 0x69 = AD0 high)
//
// Set the two candidate pins below to the two GPIOs you soldered to.

#include <Arduino.h>
#include <Wire.h>

static const int PIN_A = 2;   // one of your two I2C GPIOs
static const int PIN_B = 3;   // the other

static void scan(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, 100000);   // slow clock for robustness while debugging
  Wire.setTimeOut(50);
  delay(50);
  Serial.printf("\n--- scanning  SDA=GPIO%d  SCL=GPIO%d ---\n", sda, scl);
  int found = 0;
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("   DEVICE FOUND at 0x%02X", a);
      if (a == 0x68 || a == 0x69) Serial.print("  <-- MPU (AD0 ");
      if (a == 0x68) Serial.print("low)");
      else if (a == 0x69) Serial.print("high — tie AD0 to GND for 0x68)");
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println("   (nothing responded)");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==== I2C scanner (ESP32-S3-Zero) ====");
}

void loop() {
  scan(PIN_A, PIN_B);   // SDA=A, SCL=B
  scan(PIN_B, PIN_A);   // SDA=B, SCL=A  (swapped)
  Serial.println("\n==== repeating in 3 s (Ctrl-reset to stop) ====");
  delay(3000);
}
