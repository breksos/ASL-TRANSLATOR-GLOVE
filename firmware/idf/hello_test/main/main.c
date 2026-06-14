#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_idf_version.h"

void app_main(void)
{
    printf("\n==== XIAO ESP32-C3 Test ====\n");
    printf("ESP-IDF version: %s\n", esp_get_idf_version());

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("Chip: %s, cores: %d, revision: v%d.%d\n",
           CONFIG_IDF_TARGET, chip_info.cores,
           chip_info.revision / 100, chip_info.revision % 100);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    printf("Flash size: %" PRIu32 " MB\n", flash_size / (1024 * 1024));
    printf("Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());

    printf("\nStarting heartbeat loop...\n");
    int n = 0;
    while (1) {
        printf("[%d] alive, uptime=%lld ms, heap=%" PRIu32 "\n",
               n++,
               esp_timer_get_time() / 1000,
               esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
