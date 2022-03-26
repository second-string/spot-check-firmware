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
#include "json.h"
#include "ota_task.h"
#include "wifi.h"

#define TAG "sc-ota-task"

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// Gloval OTA handle
static esp_https_ota_handle_t ota_handle;

static esp_err_t http_client_init_callback(esp_http_client_handle_t http_client) {
    esp_err_t err = ESP_OK;
    /* Uncomment to add custom headers to HTTP request */
    // err = esp_http_client_set_header(http_client, "Custom-Header", "Value");
    return err;
}

// Sets up  OTA binary URL and queries to see if OTA image accessible (no version checking)
static void ota_start_ota(char *binary_url) {
    esp_http_client_config_t http_config = {
        .url        = binary_url,
        .cert_pem   = (char *)server_cert_pem_start,
        .timeout_ms = 10000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config         = &http_config,
        .http_client_init_cb = http_client_init_callback,
    };

    esp_err_t error = esp_https_ota_begin(&ota_config, &ota_handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed at esp_https_ota_begin: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
    }
}

static esp_err_t ota_validate_image_header(esp_app_desc_t *new_image_info, esp_app_desc_t *current_image_info) {
    if (new_image_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Running firmware version: %s", current_image_info->version);

    int version_comparison =
        memcmp(new_image_info->version, current_image_info->version, sizeof(new_image_info->version));
    if (version_comparison == 0) {
        ESP_LOGI(TAG, "OTA image version same as current version, no update needed");
        return ESP_FAIL;
    } else if (version_comparison < 0) {
        ESP_LOGI(TAG,
                 "Current version greater than OTA image version (%s), something is wrong!!",
                 new_image_info->version);
        // return ESP_FAIL;
        return ESP_OK;
    } else if (version_comparison > 0) {
        ESP_LOGI(TAG, "Current version less than OTA image version (%s), starting OTA update", new_image_info->version);
        return ESP_OK;
    }

    // Satisfy the compiler, will never be executed with above if/else
    return ESP_FAIL;
}

static bool check_forced_update(esp_app_desc_t *current_image_info, char *version_to_download) {
    // Send a request to our custom FW endpoint to determine if we need to force a downgrade
    char post_data[60];
    int  err = sprintf(post_data, "{\"current_version\": \"%s\"}", current_image_info->version);
    if (err < 0) {
        ESP_LOGE(TAG, "Error sprintfing version string into version_info endpoint post body");
        return ESP_FAIL;
    }

    char   version_info_path[] = "ota/version_info";
    size_t base_len            = strlen(URL_BASE);
    size_t path_len            = strlen(version_info_path);

    // Build our url with memcpy (no null term) then strcpy (null term)
    char url[base_len + path_len + 1];
    memcpy(url, URL_BASE, base_len);
    strcpy(url + base_len, version_info_path);
    request request_obj = {.num_params = 0, .params = NULL, .url = url};

    char   response_data[100];
    size_t response_data_size;
    ESP_ERROR_CHECK(
        http_client_perform_post(&request_obj, post_data, strlen(post_data), response_data, &response_data_size));

    ESP_LOGI(TAG, "%s", response_data);
    bool   force_update      = false;
    cJSON *response_json     = parse_json(response_data);
    cJSON *needs_update_json = cJSON_GetObjectItem(response_json, "needs_update");
    if (cJSON_IsTrue(needs_update_json)) {
        // This will break if server gives us a string larger than pre-alloced buf passed in. Right now it's 10 bytes
        cJSON *version_to_download_json = cJSON_GetObjectItem(response_json, "server_version");
        char  *temp_version             = cJSON_GetStringValue(version_to_download_json);
        strcpy(version_to_download, temp_version);
        force_update = true;
    }

    // Clean up json mem
    cJSON_free(response_json);

    return force_update;
}

void check_ota_update_task(void *args) {
    ESP_LOGI(TAG, "Starting OTA task to check update status");

#ifdef CONFIG_DISABLE_OTA
    ESP_LOGI(TAG, "FW compiled with ENABLE_OTA menuconfig option disabled, bailing out of OTA task");
    vTaskDelete(NULL);
#endif

    while (!connected_to_network) {
        ESP_LOGI(TAG, "Not connected to wifi yet, OTA task will sleep and periodically check connection");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    // Start our OTA process with the default binary URL first
    ota_start_ota(CONFIG_OTA_URL);

    // Get our current version
    const esp_partition_t *current_partition = esp_ota_get_running_partition();
    esp_app_desc_t         current_image_info;
    esp_ota_get_partition_description(current_partition, &current_image_info);

    // Download the servers inital binary header
    esp_app_desc_t ota_image_desc;
    esp_err_t      error = esp_https_ota_get_img_desc(ota_handle, &ota_image_desc);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed at esp_https_ota_get_img_desc: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
    }

    // Check to see if a basic version comparison results in an update from a newer version on the server
    error = ota_validate_image_header(&ota_image_desc, &current_image_info);
    if (error != ESP_OK) {
        ESP_LOGI(TAG,
                 "Image validation resulted in no go-ahead for update. Now checking custom endpoint for forced "
                 "upgrades/downgrades...");

        // If we get anything other than success, we don't do our basic upgrade. Check our force upgrade/downgrade
        // endpoint for us to manually apply a specific version
        char version_to_download[10];
        bool force_download = check_forced_update(&current_image_info, version_to_download);
        if (force_download) {
            ESP_LOGI(TAG,
                     "Received force_download command from server for version %s, getting now",
                     version_to_download);
            size_t ota_url_size = strlen(CONFIG_OTA_URL);
            char   forced_version_url[ota_url_size + 25];
            char   query_str[] = "?version=";
            memcpy(forced_version_url, CONFIG_OTA_URL, ota_url_size);
            memcpy(forced_version_url + ota_url_size, query_str, strlen(query_str));
            strcpy(forced_version_url + ota_url_size + strlen(query_str), version_to_download);

            ESP_LOGI(TAG, "Attempting to restart OTA with specific version url: %s", forced_version_url);
            // Restart OTA process with new url specific to forced version
            ota_start_ota(forced_version_url);
        } else {
            ESP_LOGI(TAG, "Still got no go-ahead from force OTA endpoint, deleting OTA task");
            vTaskDelete(NULL);
        }
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
