#include "constants.h"

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "http_client.h"
#include "ota_task.h"

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

void check_ota_update_task(void *args) {
    ESP_LOGI(TAG, "Starting OTA task to check update status");

    while (!connected_to_network) {
        ESP_LOGI(TAG, "Not connected to wifi yet, OTA task will sleep and periodically check connection");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    esp_http_client_config_t ota_config = {
        .url           = CONFIG_OTA_URL,
        .cert_pem      = (char *)server_cert_pem_start,
        .event_handler = http_event_handler,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }

    // Delete this task once we've checked
    vTaskDelete(NULL);
}
