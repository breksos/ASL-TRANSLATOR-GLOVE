// ASL glove finger sender — XIAO ESP32-C3 per finger.
// Reads MPU6050 over I2C, runs Kalman with accel-magnitude gating, integrates
// yaw, broadcasts FingerPacket over ESP-NOW @ 50 Hz on channel ESPNOW_CHANNEL.
//
// Set FINGER_ID below before each upload: 1=thumb 2=index 3=middle 4=ring 5=pinky.
// Wiring: MPU VCC=3V3, GND=GND, SDA=GPIO6 (D4), SCL=GPIO7 (D5).

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "mpu6050.h"
#include "kalman.h"
#include "packet.h"

// ===================== USER CONFIG =====================
#define FINGER_ID         5            // 1..5
#define ESPNOW_CHANNEL    6
#define SEND_PERIOD_US    20000        // 50 Hz
#define SDA_PIN           6
#define SCL_PIN           7
// =======================================================

static const char *TAG = "finger";
static uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint32_t s_tx_ok = 0, s_tx_fail_cb = 0, s_tx_fail_imm = 0;

static void send_cb(const wifi_tx_info_t *info, esp_now_send_status_t st) {
    (void)info;
    if (st == ESP_NOW_SEND_SUCCESS) s_tx_ok++;
    else                            s_tx_fail_cb++;
}

static void init_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    esp_wifi_set_max_tx_power(80);   // 0.25 dBm steps → 80 = 20 dBm

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, BROADCAST_MAC, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt = false;
    p.ifidx   = WIFI_IF_STA;
    ESP_ERROR_CHECK(esp_now_add_peer(&p));
}

void app_main(void) {
    // NVS is required by esp_wifi.
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    mpu6050_t mpu;
    if (!mpu6050_init(&mpu, SDA_PIN, SCL_PIN)) {
        ESP_LOGE(TAG, "MPU init failed");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Stillness-checked calibration — retry until the hand is held still.
    mpu_cal_t cal = {0};
    while (!mpu6050_calibrate_still(&mpu, &cal)) {
        ESP_LOGW(TAG, "hold finger still, retrying in 1 s...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Seed the two Kalmans with the first accel reading minus offsets.
    kalman_t kP, kR;
    kalman_init(&kP); kalman_init(&kR);
    {
        float a[3], g[3];
        if (mpu6050_read_imu(&mpu, a, g)) {
            float roll  = atan2f(a[1], a[2]) * 180.0f / (float)M_PI - cal.roll_off;
            float pitch = atan2f(-a[0], sqrtf(a[1]*a[1] + a[2]*a[2])) * 180.0f / (float)M_PI - cal.pitch_off;
            kalman_set_angle(&kP, pitch);
            kalman_set_angle(&kR, roll);
        }
    }

    init_espnow();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "FINGER %d MAC=%02x:%02x:%02x:%02x:%02x:%02x ch=%d",
             FINGER_ID, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], ESPNOW_CHANNEL);

    // Phase-stagger so the 5 senders never transmit at the same instant.
    vTaskDelay(pdMS_TO_TICKS((FINGER_ID - 1) * 3));

    uint32_t seq = 0;
    float yaw = 0.0f;
    int64_t last_us  = esp_timer_get_time();
    int64_t next_us  = last_us;
    int64_t still_since = -1;
    int64_t last_stats  = last_us;

    // Tuning constants. Values in SI (m/s², dps).
    const float ACCEL_GATE_MS2     = 1.5f;     // |‖a‖-g| above this → gyro-only
    const float STILL_GYRO_DPS     = 1.0f;     // total ‖g‖ for "still"
    const float STILL_ACC_MS2      = 0.3f;     // |‖a‖-g| for "still"
    const float YAW_DEADBAND_DPS   = 5.0f;     // ignore tiny gyro_z drift
    const float YAW_MAX_DEG        = 180.0f;   // wider than the old ±60 clamp
    const int64_t STILL_RESET_US   = 3000000;  // reset yaw after 3 s still
    const float BIAS_EMA_ALPHA     = 0.005f;   // online gyro-bias learning rate

    uint32_t consec_i2c_fail = 0;

    while (1) {
        // --- 50 Hz deadline scheduler ---
        int64_t now = esp_timer_get_time();
        if (now < next_us) {
            int64_t wait_us = next_us - now;
            if (wait_us > 1500) {
                vTaskDelay(pdMS_TO_TICKS((wait_us - 1000) / 1000));
            }
            while (esp_timer_get_time() < next_us) { /* tight spin <1 ms */ }
            now = esp_timer_get_time();
        }
        next_us += SEND_PERIOD_US;
        if (now > next_us + 2 * SEND_PERIOD_US) next_us = now + SEND_PERIOD_US;

        // --- IMU read with I2C recovery ---
        float a[3], g[3];
        if (!mpu6050_read_imu(&mpu, a, g)) {
            consec_i2c_fail++;
            if (consec_i2c_fail > 20) {
                ESP_LOGE(TAG, "I2C recovery (consec=%lu)", (unsigned long)consec_i2c_fail);
                mpu6050_recover_bus(&mpu);
                consec_i2c_fail = 0;
            }
            continue;
        }
        consec_i2c_fail = 0;

        float dt = (now - last_us) / 1e6f;
        if (dt <= 0.0f || dt > 0.05f) dt = (float)SEND_PERIOD_US / 1e6f;
        last_us = now;

        float gx = g[0] - cal.gx_bias;
        float gy = g[1] - cal.gy_bias;
        float gz = g[2] - cal.gz_bias;
        float ax = a[0], ay = a[1], az = a[2];

        float a_mag = sqrtf(ax*ax + ay*ay + az*az);
        float dev_g = fabsf(a_mag - MPU_G_TO_MS2);
        float g_mag = sqrtf(gx*gx + gy*gy + gz*gz);
        bool is_still = (dev_g < STILL_ACC_MS2) && (g_mag < STILL_GYRO_DPS);

        // Online gyro-bias re-estimation when still — kills temperature drift.
        if (is_still) {
            cal.gx_bias += BIAS_EMA_ALPHA * (g[0] - cal.gx_bias);
            cal.gy_bias += BIAS_EMA_ALPHA * (g[1] - cal.gy_bias);
            cal.gz_bias += BIAS_EMA_ALPHA * (g[2] - cal.gz_bias);
            if (still_since < 0) still_since = now;
        } else {
            still_since = -1;
        }

        // Yaw: integrate gyro_z with deadband + clamp; reset after sustained stillness.
        float gzd = (fabsf(gz) < YAW_DEADBAND_DPS) ? 0.0f : gz;
        yaw += gzd * dt;
        if (yaw >  YAW_MAX_DEG) yaw =  YAW_MAX_DEG;
        if (yaw < -YAW_MAX_DEG) yaw = -YAW_MAX_DEG;
        if (is_still && still_since > 0 && (now - still_since) > STILL_RESET_US) {
            yaw = 0.0f;
        }

        // Kalman: gate accel measurement update by acceleration magnitude.
        // During fast motion the accel direction ≠ gravity, so we must NOT
        // fuse it into pitch/roll — only propagate with gyro.
        float acc_roll  = atan2f(ay, az) * 180.0f / (float)M_PI - cal.roll_off;
        float acc_pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / (float)M_PI - cal.pitch_off;
        float fP, fR;
        if (dev_g < ACCEL_GATE_MS2) {
            fP = kalman_update(&kP, acc_pitch, gy, dt);
            fR = kalman_update(&kR, acc_roll,  gx, dt);
        } else {
            fP = kalman_predict(&kP, gy, dt);
            fR = kalman_predict(&kR, gx, dt);
        }

        finger_packet_t pkt = {
            .finger_id = FINGER_ID,
            .seq       = ++seq,
            .t_ms      = (uint32_t)(now / 1000),
            .pitch = fP, .roll = fR, .yaw = yaw,
            .gx = gx, .gy = gy, .gz = gz,
            .ax = ax, .ay = ay, .az = az,
        };
        if (isfinite(pkt.pitch) && isfinite(pkt.roll) && isfinite(pkt.yaw)) {
            esp_err_t er = esp_now_send(BROADCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
            if (er != ESP_OK) s_tx_fail_imm++;
        }

        // 1 Hz health line. Cheap; printed even on senders so flashing each
        // XIAO over USB shows you what it sees in real time.
        if (now - last_stats > 1000000) {
            last_stats = now;
            ESP_LOGI(TAG,
                "seq=%lu still=%d dev=%.2f gmag=%.2f bias=(%.3f,%.3f,%.3f) tx ok=%lu cbF=%lu imF=%lu",
                (unsigned long)seq, is_still ? 1 : 0, dev_g, g_mag,
                cal.gx_bias, cal.gy_bias, cal.gz_bias,
                (unsigned long)s_tx_ok, (unsigned long)s_tx_fail_cb, (unsigned long)s_tx_fail_imm);
            s_tx_ok = s_tx_fail_cb = s_tx_fail_imm = 0;
        }
    }
}
