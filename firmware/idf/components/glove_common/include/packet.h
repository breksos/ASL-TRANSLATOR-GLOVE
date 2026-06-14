#pragma once
#include <stdint.h>

// Wire format broadcast over ESP-NOW from each finger sender, and produced
// locally on the receiver for the palm (finger_id == 6). 1-byte packed so
// every board agrees byte-for-byte.
typedef struct __attribute__((packed)) {
    uint8_t  finger_id;     // 1=thumb 2=index 3=middle 4=ring 5=pinky 6=palm(local)
    uint32_t seq;
    uint32_t t_ms;
    float    pitch, roll, yaw;
    float    gx, gy, gz;
    float    ax, ay, az;
} finger_packet_t;
