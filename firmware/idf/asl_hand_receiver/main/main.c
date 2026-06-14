// ASL hand receiver — ESP32 DevKit1 on back of hand.
//
// • Fingers 1..5 arrive over ESP-NOW broadcast.
// • Palm (id=6) is read locally on the DevKit's own MPU6050 (SDA=21, SCL=22).
//
// Palm sampling runs in its OWN FreeRTOS task with a 50 Hz deadline scheduler
// so the serial print loop in app_main can never jitter it. Source state is
// protected by a portMUX spinlock — short critical sections only.

#include <stdio.h>
#include <string.h>
#include <math.h>
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

#define ESPNOW_CHANNEL  6
#define NUM_SOURCES     6
#define PALM_ID         6
#define SDA_PIN         21
#define SCL_PIN         22

static const char *TAG = "recv";

typedef struct {
    uint32_t seq;
    int64_t  last_rx_us;
    uint32_t rx_count;
    uint32_t lost_count;
    uint32_t rx_last_sec;
    finger_packet_t pkt;
} src_state_t;

static src_state_t s_src[NUM_SOURCES + 1];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *source_name(uint8_t id) {
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

// ESP-NOW recv runs in the WiFi task. Keep it short — no printf, no I2C.
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    if (len != (int)sizeof(finger_packet_t)) return;
    finger_packet_t p;
    memcpy(&p, data, sizeof(p));
    if (p.finger_id < 1 || p.finger_id > 5) return;

    portENTER_CRITICAL(&s_lock);
    src_state_t *s = &s_src[p.finger_id];
    if (s->rx_count > 0) {
        if (p.seq > s->seq + 1) {
            s->lost_count += (p.seq - s->seq - 1);
        } else if (p.seq < s->seq) {
            // Sender rebooted — reset stats so loss accounting stays sane.
            s->lost_count  = 0;
            s->rx_last_sec = 0;
        }
    }
    s->pkt        = p;
    s->seq        = p.seq;
    s->last_rx_us = esp_timer_get_time();
    s->rx_count++;
    portEXIT_CRITICAL(&s_lock);
}

// ---------- Palm task (50 Hz, deadline-scheduled, independent of printer) ----------
static mpu6050_t s_palm;
static mpu_cal_t s_palm_cal;
static bool      s_palm_ok = false;

static void palm_task(void *arg) {
    (void)arg;
    kalman_t kP, kR;
    kalman_init(&kP); kalman_init(&kR);
    {
        float a[3], g[3];
        if (mpu6050_read_imu(&s_palm, a, g)) {
            float roll  = atan2f(a[1], a[2]) * 180.0f / (float)M_PI - s_palm_cal.roll_off;
            float pitch = atan2f(-a[0], sqrtf(a[1]*a[1] + a[2]*a[2])) * 180.0f / (float)M_PI - s_palm_cal.pitch_off;
            kalman_set_angle(&kP, pitch);
            kalman_set_angle(&kR, roll);
        }
    }

    float yaw = 0.0f;
    uint32_t seq = 0;
    int64_t last_us  = esp_timer_get_time();
    int64_t next_us  = last_us;
    int64_t still_since = -1;
    uint32_t consec_fail = 0;

    const float ACCEL_GATE_MS2   = 1.5f;
    const float STILL_GYRO_DPS   = 1.0f;
    const float STILL_ACC_MS2    = 0.3f;
    const float YAW_DEADBAND_DPS = 5.0f;
    const float YAW_MAX_DEG      = 180.0f;
    const int64_t STILL_RESET_US = 3000000;
    const float BIAS_EMA_ALPHA   = 0.005f;
    const int64_t PERIOD_US      = 20000;

    while (1) {
        int64_t now = esp_timer_get_time();
        if (now < next_us) {
            int64_t w = next_us - now;
            if (w > 1500) vTaskDelay(pdMS_TO_TICKS((w - 1000) / 1000));
            while (esp_timer_get_time() < next_us) { }
            now = esp_timer_get_time();
        }
        next_us += PERIOD_US;
        if (now > next_us + 2 * PERIOD_US) next_us = now + PERIOD_US;

        float a[3], g[3];
        if (!mpu6050_read_imu(&s_palm, a, g)) {
            if (++consec_fail > 20) {
                ESP_LOGE(TAG, "palm I2C recovery");
                mpu6050_recover_bus(&s_palm);
                consec_fail = 0;
            }
            continue;
        }
        consec_fail = 0;

        float dt = (now - last_us) / 1e6f;
        if (dt <= 0.0f || dt > 0.05f) dt = (float)PERIOD_US / 1e6f;
        last_us = now;

        float gx = g[0] - s_palm_cal.gx_bias;
        float gy = g[1] - s_palm_cal.gy_bias;
        float gz = g[2] - s_palm_cal.gz_bias;
        float ax = a[0], ay = a[1], az = a[2];

        float a_mag = sqrtf(ax*ax + ay*ay + az*az);
        float dev_g = fabsf(a_mag - MPU_G_TO_MS2);
        float g_mag = sqrtf(gx*gx + gy*gy + gz*gz);
        bool is_still = (dev_g < STILL_ACC_MS2) && (g_mag < STILL_GYRO_DPS);

        if (is_still) {
            s_palm_cal.gx_bias += BIAS_EMA_ALPHA * (g[0] - s_palm_cal.gx_bias);
            s_palm_cal.gy_bias += BIAS_EMA_ALPHA * (g[1] - s_palm_cal.gy_bias);
            s_palm_cal.gz_bias += BIAS_EMA_ALPHA * (g[2] - s_palm_cal.gz_bias);
            if (still_since < 0) still_since = now;
        } else {
            still_since = -1;
        }

        float gzd = (fabsf(gz) < YAW_DEADBAND_DPS) ? 0.0f : gz;
        yaw += gzd * dt;
        if (yaw >  YAW_MAX_DEG) yaw =  YAW_MAX_DEG;
        if (yaw < -YAW_MAX_DEG) yaw = -YAW_MAX_DEG;
        if (is_still && still_since > 0 && (now - still_since) > STILL_RESET_US) yaw = 0.0f;

        float acc_roll  = atan2f(ay, az) * 180.0f / (float)M_PI - s_palm_cal.roll_off;
        float acc_pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / (float)M_PI - s_palm_cal.pitch_off;
        float fP, fR;
        if (dev_g < ACCEL_GATE_MS2) {
            fP = kalman_update(&kP, acc_pitch, gy, dt);
            fR = kalman_update(&kR, acc_roll,  gx, dt);
        } else {
            fP = kalman_predict(&kP, gy, dt);
            fR = kalman_predict(&kR, gx, dt);
        }

        portENTER_CRITICAL(&s_lock);
        src_state_t *sp = &s_src[PALM_ID];
        sp->pkt.finger_id = PALM_ID;
        sp->pkt.seq       = ++seq;
        sp->pkt.t_ms      = (uint32_t)(now / 1000);
        sp->pkt.pitch = fP; sp->pkt.roll = fR; sp->pkt.yaw = yaw;
        sp->pkt.gx = gx; sp->pkt.gy = gy; sp->pkt.gz = gz;
        sp->pkt.ax = ax; sp->pkt.ay = ay; sp->pkt.az = az;
        sp->seq        = seq;
        sp->last_rx_us = now;
        sp->rx_count++;
        portEXIT_CRITICAL(&s_lock);
    }
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // ---- palm MPU (optional — receiver still works if it's not soldered) ----
    if (mpu6050_init(&s_palm, SDA_PIN, SCL_PIN)) {
        ESP_LOGI(TAG, "palm online, calibrating...");
        while (!mpu6050_calibrate_still(&s_palm, &s_palm_cal)) {
            ESP_LOGW(TAG, "hold board still, retrying calibration in 1 s");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        s_palm_ok = true;
        xTaskCreate(palm_task, "palm", 4096, NULL, 6, NULL);
    } else {
        ESP_LOGW(TAG, "palm MPU not responding — finger 1..5 still received");
    }

    // ---- WiFi + ESP-NOW ----
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "receiver MAC=%02x:%02x:%02x:%02x:%02x:%02x ch=%d palm=%s",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], ESPNOW_CHANNEL,
             s_palm_ok ? "yes" : "no");

    // ---- live PRY @ 5 Hz, stats @ 1 Hz ----
    int64_t last_print = 0;
    int64_t last_stats = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();

        if (now - last_print > 200000) {     // 5 Hz
            last_print = now;
            finger_packet_t snap[NUM_SOURCES + 1];
            bool stale[NUM_SOURCES + 1] = {0};
            portENTER_CRITICAL(&s_lock);
            for (int i = 1; i <= NUM_SOURCES; i++) {
                stale[i] = (s_src[i].rx_count == 0) ||
                           ((now - s_src[i].last_rx_us) > 500000);
                snap[i] = s_src[i].pkt;
            }
            portEXIT_CRITICAL(&s_lock);

            printf("|");
            for (int i = 1; i <= NUM_SOURCES; i++) {
                if (stale[i]) {
                    printf(" %s: ----- ----- ----- |", source_name(i));
                } else {
                    printf(" %s: P%+6.1f R%+6.1f Y%+6.1f |",
                           source_name(i), snap[i].pitch, snap[i].roll, snap[i].yaw);
                }
            }
            printf("\n");
        }

        if (now - last_stats > 1000000) {
            last_stats = now;
            uint32_t rate[NUM_SOURCES + 1];
            uint32_t loss[NUM_SOURCES + 1];
            portENTER_CRITICAL(&s_lock);
            for (int i = 1; i <= NUM_SOURCES; i++) {
                rate[i] = s_src[i].rx_count - s_src[i].rx_last_sec;
                s_src[i].rx_last_sec = s_src[i].rx_count;
                loss[i] = s_src[i].lost_count;
            }
            portEXIT_CRITICAL(&s_lock);

            printf("[stats]");
            for (int i = 1; i <= NUM_SOURCES; i++) {
                if (i == PALM_ID) {
                    printf("  %s rate=%lu", source_name(i), (unsigned long)rate[i]);
                } else {
                    printf("  %s rx/s=%lu loss=%lu",
                           source_name(i), (unsigned long)rate[i], (unsigned long)loss[i]);
                }
            }
            printf("\n");
        }
    }
}
