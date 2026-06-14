#include "mpu6050.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mpu6050";

#define MPU_ADDR              0x68
#define REG_SMPLRT_DIV        0x19
#define REG_CONFIG            0x1A
#define REG_GYRO_CONFIG       0x1B
#define REG_ACCEL_CONFIG      0x1C
#define REG_PWR_MGMT_1        0x6B
#define REG_WHO_AM_I          0x75
#define REG_ACCEL_XOUT_H      0x3B

#define I2C_TIMEOUT_MS        50
#define I2C_SCL_HZ            400000

static bool wreg(mpu6050_t *m, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(m->dev, b, 2, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool rreg(mpu6050_t *m, uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_master_transmit_receive(m->dev, &reg, 1, buf, n, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool open_bus(mpu6050_t *m) {
    i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = m->sda_pin,
        .scl_io_num = m->scl_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &m->bus) != ESP_OK) return false;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MPU_ADDR,
        .scl_speed_hz    = I2C_SCL_HZ,
    };
    return i2c_master_bus_add_device(m->bus, &dc, &m->dev) == ESP_OK;
}

static bool configure_mpu(mpu6050_t *m) {
    uint8_t who;
    if (!rreg(m, REG_WHO_AM_I, &who, 1)) {
        ESP_LOGE(TAG, "WHO_AM_I read failed");
        return false;
    }
    ESP_LOGI(TAG, "WHO_AM_I=0x%02X (sda=%d scl=%d)", who, m->sda_pin, m->scl_pin);

    // wake, PLL X-gyro clock
    if (!wreg(m, REG_PWR_MGMT_1, 0x01)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));

    wreg(m, REG_SMPLRT_DIV,   0x00); // 1 kHz internal — DLPF does the filtering
    wreg(m, REG_CONFIG,       0x05); // DLPF ~10 Hz (was 0x04 = 21 Hz) — quiet for ASL
    wreg(m, REG_GYRO_CONFIG,  0x08); // ±500 dps
    wreg(m, REG_ACCEL_CONFIG, 0x08); // ±4 g
    return true;
}

bool mpu6050_init(mpu6050_t *m, int sda_pin, int scl_pin) {
    memset(m, 0, sizeof(*m));
    m->sda_pin = sda_pin;
    m->scl_pin = scl_pin;
    if (!open_bus(m)) { ESP_LOGE(TAG, "open_bus failed"); return false; }
    return configure_mpu(m);
}

bool mpu6050_read_imu(mpu6050_t *m, float a[3], float g[3]) {
    uint8_t b[14];
    if (!rreg(m, REG_ACCEL_XOUT_H, b, 14)) { m->consec_fail++; return false; }
    m->consec_fail = 0;
    int16_t rax = (int16_t)((b[0]  << 8) | b[1]);
    int16_t ray = (int16_t)((b[2]  << 8) | b[3]);
    int16_t raz = (int16_t)((b[4]  << 8) | b[5]);
    int16_t rgx = (int16_t)((b[8]  << 8) | b[9]);
    int16_t rgy = (int16_t)((b[10] << 8) | b[11]);
    int16_t rgz = (int16_t)((b[12] << 8) | b[13]);
    a[0] = (rax / MPU_ACCEL_LSB_PER_G) * MPU_G_TO_MS2;
    a[1] = (ray / MPU_ACCEL_LSB_PER_G) * MPU_G_TO_MS2;
    a[2] = (raz / MPU_ACCEL_LSB_PER_G) * MPU_G_TO_MS2;
    g[0] = rgx / MPU_GYRO_LSB_PER_DPS;
    g[1] = rgy / MPU_GYRO_LSB_PER_DPS;
    g[2] = rgz / MPU_GYRO_LSB_PER_DPS;
    return true;
}

void mpu6050_recover_bus(mpu6050_t *m) {
    ESP_LOGW(TAG, "bus recovery starting");
    if (m->dev) { i2c_master_bus_rm_device(m->dev); m->dev = NULL; }
    if (m->bus) { i2c_del_master_bus(m->bus); m->bus = NULL; }

    // Bit-bang 9 SCL pulses to clock out whatever slave is holding SDA low.
    gpio_reset_pin(m->scl_pin);
    gpio_reset_pin(m->sda_pin);
    gpio_set_direction(m->scl_pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode(m->scl_pin, GPIO_PULLUP_ONLY);
    gpio_set_direction(m->sda_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(m->sda_pin, GPIO_PULLUP_ONLY);
    gpio_set_level(m->scl_pin, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(m->scl_pin, 0); esp_rom_delay_us(5);
        gpio_set_level(m->scl_pin, 1); esp_rom_delay_us(5);
    }

    if (!open_bus(m))    { ESP_LOGE(TAG, "recovery: open_bus failed"); return; }
    if (!configure_mpu(m)) { ESP_LOGE(TAG, "recovery: configure failed"); return; }
    m->consec_fail = 0;
    ESP_LOGI(TAG, "bus recovery done");
}

bool mpu6050_calibrate_still(mpu6050_t *m, mpu_cal_t *c) {
    const int N = 300;
    double sax=0, say=0, saz=0;
    double sgx=0, sgy=0, sgz=0;
    double sgx2=0, sgy2=0, sgz2=0;
    int ok = 0;

    for (int i = 0; i < N; i++) {
        float a[3], g[3];
        if (!mpu6050_read_imu(m, a, g)) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        sax += a[0]; say += a[1]; saz += a[2];
        sgx += g[0]; sgy += g[1]; sgz += g[2];
        sgx2 += (double)g[0]*g[0];
        sgy2 += (double)g[1]*g[1];
        sgz2 += (double)g[2]*g[2];
        ok++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ok < N / 2) { ESP_LOGW(TAG, "calib: too many I2C fails (%d/%d)", ok, N); return false; }

    double mx = sax/ok, my = say/ok, mz = saz/ok;
    double mgx = sgx/ok, mgy = sgy/ok, mgz = sgz/ok;
    double vgx = sgx2/ok - mgx*mgx;
    double vgy = sgy2/ok - mgy*mgy;
    double vgz = sgz2/ok - mgz*mgz;
    double gvar = vgx + vgy + vgz;
    if (gvar > 4.0) {
        ESP_LOGW(TAG, "calib REJECTED gyro_var=%.2f (hand was moving)", gvar);
        return false;
    }

    // Compute angle offsets from the *averaged* accel vector — avoids the
    // bias you get from averaging atan2 of noisy samples.
    c->roll_off  = atan2f((float)my, (float)mz) * 180.0f / (float)M_PI;
    c->pitch_off = atan2f(-(float)mx, sqrtf((float)(my*my + mz*mz))) * 180.0f / (float)M_PI;
    c->gx_bias = (float)mgx;
    c->gy_bias = (float)mgy;
    c->gz_bias = (float)mgz;
    ESP_LOGI(TAG, "calib OK: pitch_off=%.2f roll_off=%.2f gbias=(%.3f,%.3f,%.3f) gvar=%.3f",
             c->pitch_off, c->roll_off, c->gx_bias, c->gy_bias, c->gz_bias, gvar);
    return true;
}
