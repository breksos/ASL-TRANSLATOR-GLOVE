// board_test — GLOVE-LOAD power simulation for a single board (XIAO or S3-Zero).
//
// Reproduces what one slave finger actually does: a single ESP-NOW broadcast
// of a ~45-byte packet every 40 ms (25 Hz). Realistic duty — NOT a max-rate
// hammer. It runs two phases automatically:
//
//   AVERAGE phase : 8.5 dBm (the flight transmit power) — normal operating load.
//   PEAK phase    : 19.5 dBm (max) — the highest per-burst TX current a single
//                   board can draw, i.e. this board's worst-case current spike.
//
// On every boot it prints the LAST reset reason; a power sag prints
// ">>> BROWNOUT <<<", and a boot counter (RTC memory) makes repeated resets
// obvious. If it survives both phases with no brownout and no resets, the
// board has margin for glove use.
//
// Scope note: a single board reproduces its OWN average and per-TX peak
// current, but NOT the system peak where several fingers' bursts align on a
// shared supply rail — that only appears on the fully assembled glove.
//
// No MPU is used (pure radio/power test), so pins/sensor are irrelevant; works
// on any ESP32-C3 or ESP32-S3 board.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_system.h"

// ---- config ----
#define TX_AVG   WIFI_POWER_8_5dBm     // flight transmit power
#define TX_PEAK  WIFI_POWER_19_5dBm    // max per-burst current
static const uint32_t SEND_PERIOD_MS = 40;     // 25 Hz, one broadcast per cycle
static const uint32_t IDLE_MS = 8000;          // baseline before radio comes up
static const uint32_t AVG_MS  = 30000;         // AVERAGE phase length, then PEAK
static const uint8_t  ESPNOW_CHANNEL = 11;
// ----------------

RTC_DATA_ATTR int bootCount = 0;

static uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
typedef struct __attribute__((packed)) { uint8_t b[45]; } SlavePkt;  // ~slave size
static SlavePkt pkt;

enum Phase { IDLE, AVG, PEAK };
Phase phase = IDLE;
unsigned long lastBeat = 0, lastSend = 0, phaseStart = 0;
uint32_t sendOk = 0, sendFail = 0;

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on (normal)";
    case ESP_RST_SW:        return "software reset";
    case ESP_RST_PANIC:     return "panic / crash";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power sag)";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    default:                return "other/unknown";
  }
}

static void startRadio(wifi_power_t power) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setTxPower(power);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init FAILED"); return; }
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST_MAC, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  bootCount++;
  esp_reset_reason_t rr = esp_reset_reason();

  Serial.println("\n\n==== BOARD GLOVE-LOAD TEST ====");
  Serial.printf("Boot #%d\n", bootCount);
  Serial.printf("Last reset reason: %d (%s)\n", (int)rr, resetReasonStr(rr));
  if (rr == ESP_RST_BROWNOUT) {
    Serial.println(">>> PREVIOUS BOOT ENDED IN BROWNOUT — power path is sagging <<<");
  }
  Serial.printf("Chip rev: %d   Free heap: %u bytes\n",
                ESP.getChipRevision(), ESP.getFreeHeap());
  Serial.println("---- PHASE 1: IDLE (no radio). A reset here = severe fault/cable. ----");
  phaseStart = millis();
}

void loop() {
  unsigned long now = millis();

  // Phase transitions
  if (phase == IDLE && now > IDLE_MS) {
    startRadio(TX_AVG);
    phase = AVG; phaseStart = now;
    Serial.println("---- PHASE 2: AVERAGE  8.5 dBm  25 Hz  (flight load) ----");
  } else if (phase == AVG && (now - phaseStart) > AVG_MS) {
    WiFi.setTxPower(TX_PEAK);   // live power bump, no re-init needed
    phase = PEAK; phaseStart = now;
    Serial.println("---- PHASE 3: PEAK  19.5 dBm  25 Hz  (max per-burst current) ----");
  }

  // Realistic slave load: one broadcast per 40 ms (25 Hz)
  if (phase != IDLE && (now - lastSend) >= SEND_PERIOD_MS) {
    lastSend = now;
    esp_err_t r = esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
    if (r == ESP_OK) sendOk++; else sendFail++;
  }

  if (now - lastBeat >= 1000) {
    lastBeat = now;
    const char* pn = (phase == IDLE) ? "idle" : (phase == AVG ? "AVG-8.5" : "PEAK-19.5");
    Serial.printf("[%lus] alive  heap=%u  phase=%s  txOk=%lu txFail=%lu\n",
                  now / 1000, ESP.getFreeHeap(), pn,
                  (unsigned long)sendOk, (unsigned long)sendFail);
  }
}
