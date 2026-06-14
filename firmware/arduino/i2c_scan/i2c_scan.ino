// XIAO ESP32-C3 I2C scanner. SDA=D4(GPIO6), SCL=D5(GPIO7).
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin(6, 7, 100000);   // start slow for diagnostics
  Serial.println("I2C scan starting...");
}

void loop() {
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("  (no devices)");
  Serial.println("---");
  delay(2000);
}
