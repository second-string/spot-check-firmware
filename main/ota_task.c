#include "constants.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "http_client.h"
#include "ota_task.h"

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static esp_err_t http_client_init_callback(esp_http_client_handle_t http_client) {
    esp_err_t err = ESP_OK;
    /* Uncomment to add custom headers to HTTP request */
    // err = esp_http_client_set_header(http_client, "Custom-Header", "Value");
    return err;
}

static esp_err_t validate_image_header(esp_app_desc_t *image_info) {
    if (image_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *current_partition = esp_ota_get_running_partition();
    esp_app_desc_t         current_image_info;
    if (esp_ota_get_partition_description(current_partition, &current_image_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", current_image_info.version);
    }

    int version_comparison = memcmp(image_info->version, current_image_info.version, sizeof(image_info->version));
    if (version_comparison == 0) {
        ESP_LOGI(TAG, "OTA image version same as current version, no update needed");
        return ESP_FAIL;
    } else if (version_comparison < 0) {
        ESP_LOGI(TAG, "Current version less than OTA image version (%s), starting OTA update", image_info->version);
        return ESP_OK;
    } else if (version_comparison > 0) {
        ESP_LOGI(TAG, "Current version greater than OTA image version (%s), something is wrong!!", image_info->version);
        // return ESP_FAIL;
        return ESP_OK;
    }

    // Satisfy the compiler, will never be executed with above if/else
    return ESP_FAIL;
}
void check_ota_update_task(void *args) {
    ESP_LOGI(TAG, "Starting OTA task to check update status");

    while (!connected_to_network) {
        ESP_LOGI(TAG, "Not connected to wifi yet, OTA task will sleep and periodically check connection");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    esp_http_client_config_t http_config = {
        .url        = CONFIG_OTA_URL,
        .cert_pem   = (char *)server_cert_pem_start,
        .timeout_ms = 10000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config         = &http_config,
        .http_client_init_cb = http_client_init_callback,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t              error      = esp_https_ota_begin(&ota_config, &ota_handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed at esp_https_ota_begin: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
    }

    esp_app_desc_t ota_image_desc;
    error = esp_https_ota_get_img_desc(ota_handle, &ota_image_desc);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed at esp_https_ota_get_img_desc: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
    }

    error = validate_image_header(&ota_image_desc);
    if (error != ESP_OK) {
        ESP_LOGI(TAG, "Image validation resulted in no OTA update, deleting task.");
        vTaskDelete(NULL);
    }

    uint32_t iter_counter = 0;
    uint32_t bytes_received;
    while (1) {
        error = esp_https_ota_perform(ota_handle);
        if (error != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (error == ESP_OK) {
                ESP_LOGI(TAG, "Successfully received full OTA image");
            } else {
                ESP_LOGE(TAG, "OTA failed during esp_https_ota_perform: %s", esp_err_to_name(error));
            }
            break;
        }

        bytes_received = esp_https_ota_get_image_len_read(ota_handle);

        if (iter_counter >= 100) {
            ESP_LOGI(TAG, "Received %d bytes of image so far", bytes_received);
            iter_counter = 0;
        }

        iter_counter++;
    }

    bool received_full_image = esp_https_ota_is_complete_data_received(ota_handle);
    if (!received_full_image) {
        ESP_LOGE(TAG, "Did not receive full image package from server, aborting.");
        vTaskDelete(NULL);
    }

    error = esp_https_ota_finish(ota_handle);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, rebooting in 3 seconds...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        if (error == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA failed in esp_https_ota_finish, image validation unsuccessful.");
        } else {
            ESP_LOGE(TAG, "Error in esp_https_ota_finish, OTA update unsuccessful: %s", esp_err_to_name(error));
        }
    }

    // Delete this task once we've checked
    vTaskDelete(NULL);
}
