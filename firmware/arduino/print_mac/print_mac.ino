#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
}

void loop() {
  Serial.print("STA MAC: ");
  Serial.println(WiFi.macAddress());
  delay(1000);
}
