#include "kalman.h"

void kalman_init(kalman_t *k) {
    k->Q_angle   = 0.001f;
    k->Q_bias    = 0.003f;
    k->R_measure = 0.03f;
    k->angle = 0.0f; k->bias = 0.0f;
    k->P[0][0] = 1; k->P[0][1] = 0;
    k->P[1][0] = 0; k->P[1][1] = 1;
}

void kalman_set_angle(kalman_t *k, float a) { k->angle = a; }

float kalman_predict(kalman_t *k, float rate, float dt) {
    float r = rate - k->bias;
    k->angle += dt * r;
    k->P[0][0] += dt * (dt * k->P[1][1] - k->P[0][1] - k->P[1][0] + k->Q_angle);
    k->P[0][1] -= dt * k->P[1][1];
    k->P[1][0] -= dt * k->P[1][1];
    k->P[1][1] += k->Q_bias * dt;
    return k->angle;
}

float kalman_update(kalman_t *k, float meas, float rate, float dt) {
    kalman_predict(k, rate, dt);
    float S  = k->P[0][0] + k->R_measure;
    float K0 = k->P[0][0] / S;
    float K1 = k->P[1][0] / S;
    float y  = meas - k->angle;
    k->angle += K0 * y;
    k->bias  += K1 * y;
    float P00 = k->P[0][0], P01 = k->P[0][1];
    k->P[0][0] -= K0 * P00;
    k->P[0][1] -= K0 * P01;
    k->P[1][0] -= K1 * P00;
    k->P[1][1] -= K1 * P01;
    return k->angle;
}
