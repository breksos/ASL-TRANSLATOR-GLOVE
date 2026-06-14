// I2C scanner for XIAO ESP32-C3 (SDA=GPIO6, SCL=GPIO7).
// Probes every 7-bit address using the new master driver, prints the ones
// that ACK. Useful for verifying the MPU6050 wiring before flashing the
// real sender firmware.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#define SDA_PIN 6
#define SCL_PIN 7

void app_main(void) {
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &bus));
    printf("I2C scan starting on sda=%d scl=%d (slow 100 kHz probe)...\n", SDA_PIN, SCL_PIN);

    while (1) {
        int found = 0;
        for (uint16_t addr = 1; addr < 127; addr++) {
            if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
                printf("  device at 0x%02X\n", addr);
                found++;
            }
        }
        if (!found) printf("  (no devices)\n");
        printf("---\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
