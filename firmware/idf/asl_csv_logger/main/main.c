// ASL hand receiver — CSV-only data-loss-proof variant.
//
// Output: one CSV row per packet (or per local palm sample).
//   rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az
//
// Architecture vs the original Arduino sketch:
//   • ESP-NOW recv callback NEVER calls printf — it pushes into a FreeRTOS
//     queue and returns. This stops Serial back-pressure from blocking the
//     WiFi stack and dropping packets.
//   • A dedicated emitter task drains the queue at full speed.
//   • Palm IMU runs in its own task at 50 Hz and pushes packets into the
//     same queue, so they get the same timestamp/format treatment.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#define PALM_ID         6
#define SDA_PIN         21
#define SCL_PIN         22
#define CSV_QUEUE_LEN   256        // ~1 second of buffering at 300 pkt/s

typedef struct {
    uint32_t        rx_ms;
    finger_packet_t pkt;
} csv_msg_t;

static QueueHandle_t s_q = NULL;
static const char *TAG = "csvlog";

static volatile uint32_t s_dropped = 0;  // queue overflow counter

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    if (len != (int)sizeof(finger_packet_t)) return;
    csv_msg_t m;
    memcpy(&m.pkt, data, sizeof(m.pkt));
    if (m.pkt.finger_id < 1 || m.pkt.finger_id > 5) return;
    m.rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (xQueueSend(s_q, &m, 0) != pdTRUE) s_dropped++;
}

static void emitter_task(void *arg) {
    (void)arg;
    csv_msg_t m;
    while (1) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) == pdTRUE) {
            const finger_packet_t *p = &m.pkt;
            printf("%lu,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                   (unsigned long)m.rx_ms, (unsigned)p->finger_id,
                   (unsigned long)p->seq, (unsigned long)p->t_ms,
                   p->pitch, p->roll, p->yaw,
                   p->gx, p->gy, p->gz,
                   p->ax, p->ay, p->az);
        }
    }
}

// ---------- Palm task (50 Hz, deadline scheduled) ----------
static mpu6050_t s_palm;
static mpu_cal_t s_palm_cal;

static void palm_task(void *arg) {
    (void)arg;
    kalman_t kP, kR; kalman_init(&kP); kalman_init(&kR);
    {
        float a[3], g[3];
        if (mpu6050_read_imu(&s_palm, a, g)) {
            float roll  = atan2f(a[1], a[2]) * 180.0f / (float)M_PI - s_palm_cal.roll_off;
            float pitch = atan2f(-a[0], sqrtf(a[1]*a[1] + a[2]*a[2])) * 180.0f / (float)M_PI - s_palm_cal.pitch_off;
            kalman_set_angle(&kP, pitch); kalman_set_angle(&kR, roll);
        }
    }

    float yaw = 0.0f;
    uint32_t seq = 0;
    int64_t last_us = esp_timer_get_time();
    int64_t next_us = last_us;
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
            if (++consec_fail > 20) { mpu6050_recover_bus(&s_palm); consec_fail = 0; }
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

        csv_msg_t m;
        m.rx_ms = (uint32_t)(now / 1000);
        m.pkt.finger_id = PALM_ID;
        m.pkt.seq       = ++seq;
        m.pkt.t_ms      = m.rx_ms;
        m.pkt.pitch = fP; m.pkt.roll = fR; m.pkt.yaw = yaw;
        m.pkt.gx = gx; m.pkt.gy = gy; m.pkt.gz = gz;
        m.pkt.ax = ax; m.pkt.ay = ay; m.pkt.az = az;
        if (xQueueSend(s_q, &m, 0) != pdTRUE) s_dropped++;
    }
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_q = xQueueCreate(CSV_QUEUE_LEN, sizeof(csv_msg_t));
    xTaskCreate(emitter_task, "csv_emit", 4096, NULL, 5, NULL);

    // CSV header — printf direct, not via queue (one-shot).
    printf("# rx_ms,finger_id,seq,t_send_ms,P,R,Y,gx,gy,gz,ax,ay,az\n");

    if (mpu6050_init(&s_palm, SDA_PIN, SCL_PIN)) {
        while (!mpu6050_calibrate_still(&s_palm, &s_palm_cal)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        xTaskCreate(palm_task, "palm", 4096, NULL, 6, NULL);
    } else {
        ESP_LOGW(TAG, "palm MPU not present — logging fingers 1..5 only");
    }

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

    // Periodic drop-counter line, prefixed with '#' so post-processors skip it.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t d = s_dropped;
        s_dropped = 0;
        printf("# dropped_last_5s=%lu queue_waiting=%u\n",
               (unsigned long)d, (unsigned)uxQueueMessagesWaiting(s_q));
    }
}
