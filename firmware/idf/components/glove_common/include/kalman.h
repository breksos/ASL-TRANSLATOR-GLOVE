#pragma once

// 2-state Kalman: tracks angle and gyro-bias for one axis.
// Q_angle / Q_bias / R_measure tuned for MPU6050 from breksos/esp32-gesture-robotic-arm.
typedef struct {
    float Q_angle, Q_bias, R_measure;
    float angle, bias;
    float P[2][2];
} kalman_t;

void  kalman_init(kalman_t *k);
void  kalman_set_angle(kalman_t *k, float a);

// Propagate state with gyro only (use when accel is unreliable, e.g. during motion).
float kalman_predict(kalman_t *k, float rate, float dt);

// Full predict + accel measurement update.
float kalman_update(kalman_t *k, float meas, float rate, float dt);
