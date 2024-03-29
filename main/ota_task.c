#include "constants.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "memfault/panics/assert.h"
#include "sdkconfig.h"

#include "constants.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "ota_task.h"
#include "scheduler_task.h"
#include "screen_img_handler.h"
#include "sleep_handler.h"
#include "spot_check.h"
#include "wifi.h"

#define TAG SC_TAG_OTA

typedef enum {
    OTA_RESULT_NOT_STARTED,  // Any reason we bail before actual download of image (version compare the same, ota
                             // disabled, even any failures that occur before we actually start/draw start text)
    OTA_RESULT_FAIL,         // Download of image was started but failed somewhere before we try to reboot into it
    OTA_RESULT_SUCCESS,      // Download and validate of new image successful, full process succeeded
} ota_result_t;

// Global OTA and task handles
// TODO :: these should all be in our own OTA handle I'm just being lazy
static esp_https_ota_handle_t ota_handle;
static TaskHandle_t           ota_task_handle    = NULL;
static scheduler_mode_t       mode_at_task_start = SCHEDULER_MODE_INIT;

static esp_err_t http_client_init_callback(esp_http_client_handle_t http_client) {
    esp_err_t err = ESP_OK;
    /* Uncomment to add custom headers to HTTP request */
    // err = esp_http_client_set_header(http_client, "Custom-Header", "Value");
    return err;
}

static void ota_task_revert_scheduler_mode() {
    switch (mode_at_task_start) {
        case SCHEDULER_MODE_ONLINE:
            scheduler_set_online_mode();
            break;
        case SCHEDULER_MODE_OFFLINE:
            scheduler_set_offline_mode();
            break;
        default:
            log_printf(LOG_LEVEL_ERROR, "Reverting back to this mode in OTA task not supported!");
            MEMFAULT_ASSERT(0);
            break;
    }
}

// Sets up  OTA binary URL and queries to see if OTA image accessible (no version checking)
static bool ota_start_ota(char *binary_url) {
    // Have to manually build query params here since ota uses it's own internal http client
    char url_with_params[strlen(binary_url) + 128];
    strcpy(url_with_params, CONFIG_OTA_URL);
    strcat(url_with_params, "?");
    strcat(url_with_params, "device_id");
    strcat(url_with_params, "=");
    strcat(url_with_params, spot_check_get_serial());

    esp_http_client_config_t http_config = {
        .url               = url_with_params,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config         = &http_config,
        .http_client_init_cb = http_client_init_callback,
    };

    esp_err_t error = esp_https_ota_begin(&ota_config, &ota_handle);
    if (error != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR, "OTA failed at esp_https_ota_begin: %s", esp_err_to_name(error));
    }

    return error == ESP_OK;
}

/*
 * Return ESP_FAIL if no update needed, ESP_OK if update should proceed
 */
static esp_err_t ota_validate_image_header(esp_app_desc_t *new_image_info, esp_app_desc_t *current_image_info) {
    if (new_image_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    log_printf(LOG_LEVEL_INFO,
               "Running firmware version: %s - server get_binary endpoint returned version: %s",
               current_image_info->version,
               new_image_info->version);

    uint32_t current_major;
    uint32_t current_minor;
    uint32_t current_dot;
    sscanf(current_image_info->version, "%lu.%lu.%lu", &current_major, &current_minor, &current_dot);
    uint32_t new_major;
    uint32_t new_minor;
    uint32_t new_dot;
    sscanf(new_image_info->version, "%lu.%lu.%lu", &new_major, &new_minor, &new_dot);

    if (current_major == new_major && current_minor == new_minor && current_dot == new_dot) {
        log_printf(LOG_LEVEL_INFO, "OTA image version same as current version, no update needed");
        return ESP_FAIL;
    } else if (current_major < new_major) {
        log_printf(LOG_LEVEL_WARN, "OTA image version has lower major, starting OTA update...");
        return ESP_OK;
    } else if (current_major == new_major && current_minor < new_minor) {
        log_printf(LOG_LEVEL_WARN, "OTA image version has same major but lower minor, starting OTA update...");
        return ESP_OK;
    } else if (current_major == new_major && current_minor == new_minor && current_dot < new_dot) {
        log_printf(LOG_LEVEL_WARN,
                   "OTA image version has same major and minor but lower dot version, starting OTA update...");
        return ESP_OK;
    } else {
        // This means at least one of the current versions was greater than the new versions. That should never happen
        // unless server version is mistakenly saved OR the server is trying to force downgrade due to an issue. The
        // secondary manual version check should handle those
        log_printf(LOG_LEVEL_ERROR,
                   "Current version greater than OTA image version, something is wrong!!",
                   new_image_info->version);

        return ESP_FAIL;
    }

    // Satisfy the compiler, will never be executed with above if/else
    return ESP_FAIL;
}

static bool check_forced_update(esp_app_desc_t *current_image_info, char *version_to_download) {
    // Send a request to our custom FW endpoint to determine if we need to force a downgrade
    char post_data[100];
    int  err = sprintf(post_data,
                      "{\"current_version\": \"%s\", \"device_id\": \"%s\"}",
                      current_image_info->version,
                      spot_check_get_serial());
    if (err < 0) {
        log_printf(LOG_LEVEL_ERROR, "Error sprintfing version string into version_info endpoint post body");
        return ESP_FAIL;
    }

    char           version_info_path[] = "ota/version_info";
    char           url[strlen(URL_BASE) + strlen(version_info_path) + 1];
    http_request_t request_obj = http_client_build_post_request(version_info_path, url, post_data, strlen(post_data));

    char                    *response_data;
    size_t                   response_data_size;
    esp_http_client_handle_t client;
    int                      content_length = 0;
    bool                     http_success = http_client_perform_with_retries(&request_obj, 1, &client, &content_length);
    if (!http_success) {
        log_printf(LOG_LEVEL_ERROR,
                   "Error in http perform request checking to see if need forced update, defaulting to no update");
        return false;
    }

    esp_err_t http_err =
        http_client_read_response_to_buffer(&client, content_length, &response_data, &response_data_size);
    if (http_err != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR,
                   "Error in http request readout checking to see if need forced update, defaulting to no update");
        return false;
    }

    log_printf(LOG_LEVEL_INFO, "%s", response_data);
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

    // Clean up request and json mem
    if (response_data_size && response_data) {
        free(response_data);
    }
    cJSON_Delete(response_json);

    return force_update;
}

/*
 * Proper task teardown depending on how it went
 */
static void ota_task_stop(ota_result_t result) {
    switch (result) {
        case OTA_RESULT_NOT_STARTED:
            log_printf(LOG_LEVEL_INFO, "OTA task exiting before any download occurred, no screen changes needed");
            break;
        case OTA_RESULT_FAIL:
            // TODO :: write failure text? It would persist until next OTA check or reboot
            spot_check_clear_ota_start_text();
            spot_check_render();
            break;
        case OTA_RESULT_SUCCESS:
            // TODO :: success text briefly?
            log_printf(LOG_LEVEL_INFO, "OTA update successful, rebooting in 3 seconds...");
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            esp_restart();
            break;
    }

    // Common actions whether OTA was not needed or failed. Success case won't reach here with the restart)
    ota_task_revert_scheduler_mode();
    sleep_handler_set_idle(SYSTEM_IDLE_OTA_BIT);
    ota_task_handle = NULL;
    vTaskDelete(NULL);
}

static void check_ota_update_task(void *args) {
    sleep_handler_set_busy(SYSTEM_IDLE_OTA_BIT);
    log_printf(LOG_LEVEL_INFO, "Starting OTA task to check update status");

    // Store mode so we can properly revert once OTA is done no matter what state it finishes in
    mode_at_task_start = scheduler_get_mode();

#ifdef CONFIG_DISABLE_OTA
    log_printf(LOG_LEVEL_INFO, "FW compiled with ENABLE_OTA menuconfig option disabled, bailing out of OTA task");
    ota_task_stop(OTA_RESULT_NOT_STARTED);
    return;
#endif

    // This only checks if we're connected to a wifi network, but not if there's an active internet connection beyond
    // that. That's fine, as the status checks for http requests later in the ota process will fail out gracefully if
    // error codes rcvd
    if (!wifi_is_connected_to_network()) {
        log_printf(LOG_LEVEL_INFO, "Not connected to wifi, waiting for 30 seconds then bailing out of OTA task");
        if (!wifi_block_until_connected_timeout(30 * MS_PER_SEC)) {
            log_printf(LOG_LEVEL_INFO, "No connection received, bailing out of OTA task");
            ota_task_stop(OTA_RESULT_NOT_STARTED);
            return;
        }
        log_printf(LOG_LEVEL_INFO, "Got connection, continuing with OTA check");
    }

    // Start our OTA process with the default binary URL first
    bool success = ota_start_ota(CONFIG_OTA_URL);
    if (!success) {
        ota_task_stop(OTA_RESULT_NOT_STARTED);
    }

    // Get our current version
    const esp_partition_t *current_partition = esp_ota_get_running_partition();
    esp_app_desc_t         current_image_info;
    esp_ota_get_partition_description(current_partition, &current_image_info);

    // Download the servers inital binary header
    esp_app_desc_t ota_image_desc;
    esp_err_t      error = esp_https_ota_get_img_desc(ota_handle, &ota_image_desc);
    if (error != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR, "OTA failed at esp_https_ota_get_img_desc: %s", esp_err_to_name(error));
        ota_task_stop(OTA_RESULT_FAIL);
        return;
    }

    // Check to see if a basic version comparison results in an update from a newer version on the server
    error = ota_validate_image_header(&ota_image_desc, &current_image_info);
    if (error != ESP_OK) {
        log_printf(LOG_LEVEL_INFO,
                   "Image validation resulted in no go-ahead for update. Now checking custom endpoint for forced "
                   "upgrades/downgrades...");

        error = esp_https_ota_abort(ota_handle);
        if (error != ESP_OK) {
            log_printf(LOG_LEVEL_ERROR,
                       "Error cleaning up OTA handle to manually check our force endpoint. Giving up on OTA right now "
                       "and deleting task, but socket lock from ota internal http_client in unknown state, rest of app "
                       "might be broken.");
            ota_task_stop(OTA_RESULT_FAIL);
            return;
        }

        // If we get anything other than success, we don't do our basic upgrade. Check our force upgrade/downgrade
        // endpoint for us to manually apply a specific version
        char version_to_download[10];
        bool force_download = check_forced_update(&current_image_info, version_to_download);
        if (force_download) {
            log_printf(LOG_LEVEL_INFO,
                       "Received force_download command from server for version %s, getting now",
                       version_to_download);
            size_t ota_url_size = strlen(CONFIG_OTA_URL);
            char   forced_version_url[ota_url_size + 25];
            char   query_str[] = "?version=";
            memcpy(forced_version_url, CONFIG_OTA_URL, ota_url_size);
            memcpy(forced_version_url + ota_url_size, query_str, strlen(query_str));
            strcpy(forced_version_url + ota_url_size + strlen(query_str), version_to_download);

            // Restart OTA process with new url specific to forced version
            log_printf(LOG_LEVEL_INFO, "Attempting to restart OTA with specific version url: %s", forced_version_url);
            ota_start_ota(forced_version_url);
        } else {
            log_printf(LOG_LEVEL_INFO, "Still got no go-ahead from force OTA endpoint, deleting OTA task");
            ota_task_stop(OTA_RESULT_NOT_STARTED);
            return;
        }
    }

    // Notify user on screen and kick scheduler into OTA mode so time updates continue but no other network requests are
    // made (that would fail anyway since ota is monopolizing http client)
    scheduler_set_ota_mode();
    spot_check_draw_ota_start_text();
    spot_check_render();

    uint32_t iter_counter = 0;
    uint32_t bytes_received;
    while (1) {
        error = esp_https_ota_perform(ota_handle);
        if (error != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (error == ESP_OK) {
                log_printf(LOG_LEVEL_INFO, "Successfully received full OTA image");
            } else {
                log_printf(LOG_LEVEL_ERROR, "OTA failed during esp_https_ota_perform: %s", esp_err_to_name(error));
            }
            break;
        }

        bytes_received = esp_https_ota_get_image_len_read(ota_handle);

        if (iter_counter >= 100) {
            log_printf(LOG_LEVEL_INFO, "Received %d bytes of image so far", bytes_received);
            iter_counter = 0;
        }

        iter_counter++;
    }

    bool received_full_image = esp_https_ota_is_complete_data_received(ota_handle);
    if (!received_full_image) {
        log_printf(LOG_LEVEL_ERROR, "Did not receive full image package from server, aborting.");
        ota_task_stop(OTA_RESULT_FAIL);
        return;
    }

    error = esp_https_ota_finish(ota_handle);
    if (error != ESP_OK) {
        if (error == ESP_ERR_OTA_VALIDATE_FAILED) {
            log_printf(LOG_LEVEL_ERROR, "OTA failed in esp_https_ota_finish, image validation unsuccessful.");
        } else {
            log_printf(LOG_LEVEL_ERROR,
                       "Error in esp_https_ota_finish, OTA update unsuccessful: %s",
                       esp_err_to_name(error));
        }
    }

    // Catch-all to clear OTA text and clean up task
    ota_task_stop(OTA_RESULT_SUCCESS);
}

UBaseType_t ota_task_get_stack_high_water() {
    // OTA task is usually not running
    return ota_task_handle == NULL ? 0 : uxTaskGetStackHighWaterMark(ota_task_handle);
}

void ota_task_start() {
    if (ota_task_handle != NULL) {
        log_printf(LOG_LEVEL_WARN,
                   "%s called when ota task handle not null. This means the task wasn't torn down correctly after last "
                   "check, or it is somehow being called from somewhere it shouldn't (OTA should only run on set "
                   "schedule far apart). This is a bug, it should never happen.",
                   __func__);
        return;
    }

    // minimal * 3 is the smallest we can go w/o SO
    xTaskCreate(&check_ota_update_task,
                "check-ota-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 5,
                NULL,
                tskIDLE_PRIORITY,
                &ota_task_handle);
}
