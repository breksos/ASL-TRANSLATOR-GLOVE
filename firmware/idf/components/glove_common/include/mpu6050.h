#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// ±4 g and ±500 dps ranges; DLPF set to 10 Hz inside mpu6050_init().
#define MPU_ACCEL_LSB_PER_G    8192.0f
#define MPU_G_TO_MS2           9.80665f
#define MPU_GYRO_LSB_PER_DPS   65.5f

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int      sda_pin;
    int      scl_pin;
    uint32_t consec_fail;
} mpu6050_t;

typedef struct {
    float pitch_off, roll_off;     // accel-derived offsets at boot, degrees
    float gx_bias, gy_bias, gz_bias; // gyro bias, dps — updated online when still
} mpu_cal_t;

// Bring up the I2C bus and MPU6050 with our tight defaults:
//   ±4 g, ±500 dps, DLPF 10 Hz, 1 kHz internal rate, I2C @ 400 kHz.
bool mpu6050_init(mpu6050_t *m, int sda_pin, int scl_pin);

bool mpu6050_read_imu(mpu6050_t *m, float a_ms2[3], float g_dps[3]);

// Free a stuck SDA by bit-banging SCL 9 times, then reinit the I2C peripheral
// and the device handle. Call after a burst of consecutive read failures.
void mpu6050_recover_bus(mpu6050_t *m);

// Capture pitch/roll offsets + gyro bias while the sensor is still. Averages
// raw ax,ay,az (then computes angles once) instead of averaging noisy angle
// estimates. Rejects the capture if gyro variance during the window indicates
// the user moved — caller is expected to retry.
bool mpu6050_calibrate_still(mpu6050_t *m, mpu_cal_t *c);
