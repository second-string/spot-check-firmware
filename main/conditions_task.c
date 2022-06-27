#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "conditions_task.h"

#include "gpio.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "nvs.h"
#include "screen_img_handler.h"
#include "timer.h"
#include "wifi.h"

#define TAG "sc-conditions-task"

#define TIME_UPDATE_INTERVAL_SECONDS (60)
#define CONDITIONS_UPDATE_INTERVAL_MINUTES (20)
#define CHARTS_UPDATE_INTERVAL_MINUTES (60)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)
#define UPDATE_TIME_BIT (1 << 2)

static TaskHandle_t conditions_update_task_handle;

static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;

static void conditions_timer_expired_callback(void *timer_args) {
    seconds_elapsed++;
    uint32_t minutes_elapsed = seconds_elapsed / 60;

    if (seconds_elapsed % TIME_UPDATE_INTERVAL_SECONDS == 0) {
        conditions_trigger_time_update();
        log_printf(TAG, LOG_LEVEL_INFO, "Reached %d seconds elapsed, triggering screen time update..", seconds_elapsed);
    }

    // TODO :: switch this new_location_set flag to just calling conditions trigger function
    if ((minutes_elapsed && (minutes_elapsed % CONDITIONS_UPDATE_INTERVAL_MINUTES == 0)) || new_location_set) {
        if (new_location_set) {
            // If we have currently scrolling text, clear the LEDs for us to push a new conditions string
            // led_text_stop_scroll();
        }

        new_location_set = false;
        conditions_trigger_conditions_update();
        log_printf(TAG,
                   LOG_LEVEL_INFO,
                   "Reached %d minutes elapsed, triggering conditions update and display...",
                   minutes_elapsed);
    }

    if (minutes_elapsed && (minutes_elapsed % CHARTS_UPDATE_INTERVAL_MINUTES == 0)) {
        conditions_trigger_both_charts_update();
        log_printf(TAG,
                   LOG_LEVEL_INFO,
                   "Reached %d minutes elapsed, triggering tide and swell charts update and display...",
                   minutes_elapsed);
    }
}

static void conditions_refresh() {
    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 80];
    query_param        params[3];
    request            request = http_client_build_request("conditions", config, url_buf, params, 3);

    char                    *server_response    = NULL;
    size_t                   response_data_size = 0;
    esp_http_client_handle_t client;
    http_client_perform_request(&request, &client);
    esp_err_t http_err = http_client_read_response_to_buffer(&client, &server_response, &response_data_size);

    if (http_err == ESP_OK && response_data_size != 0) {
        cJSON *json               = parse_json(server_response);
        cJSON *data_value         = cJSON_GetObjectItem(json, "data");
        cJSON *temperature_object = cJSON_GetObjectItem(data_value, "temp");
        cJSON *wind_speed_object  = cJSON_GetObjectItem(data_value, "wind_speed");
        cJSON *wind_dir_object    = cJSON_GetObjectItem(data_value, "wind_dir");
        cJSON *tide_height_object = cJSON_GetObjectItem(data_value, "tide_height");

        log_printf(TAG, LOG_LEVEL_DEBUG, "Server response: %s", server_response);
        if (server_response != NULL) {
            free(server_response);
            server_response = NULL;
        }

        char *wind_dir_str    = cJSON_GetStringValue(wind_dir_object);
        char *tide_height_str = cJSON_GetStringValue(tide_height_object);

        // Build out local struct to make sure everything valid before we overwrite last saved values
        conditions_t temp_conditions;

        // Set ints directly, then don't know if cjson null terminates strings so manually strncpy then slap the null
        // term on
        temp_conditions.temperature = temperature_object->valueint;
        temp_conditions.wind_speed  = wind_speed_object->valueint;
        strncpy(temp_conditions.wind_dir, wind_dir_str, sizeof(temp_conditions.wind_dir) - 1);
        temp_conditions.wind_dir[sizeof(temp_conditions.wind_dir) - 1] = '\0';
        strncpy(temp_conditions.tide_height, tide_height_str, sizeof(temp_conditions.tide_height) - 1);
        temp_conditions.tide_height[sizeof(temp_conditions.tide_height) - 1] = '\0';

        cJSON_free(data_value);
        cJSON_free(json);

        // Copy into global last_retrieved_conditions struct now that everything parsed correctly
        memcpy(&last_retrieved_conditions, &temp_conditions, sizeof(conditions_t));
    } else {
        log_printf(TAG, LOG_LEVEL_INFO, "Failed to get new conditions, leaving last saved values displayed");
    }

    // Caller responsible for freeing buffer if non-null on return
    if (server_response != NULL) {
        free(server_response);
        server_response = NULL;
    }
}

static void conditions_update_task(void *args) {
    timer_info_handle conditions_handle =
        timer_init("conditions", conditions_timer_expired_callback, NULL, MS_PER_SECOND);
    timer_reset(conditions_handle, true);

    // Wait forever until connected
    wifi_block_until_connected();

    uint32_t update_bits = 0;
    while (1) {
        // Wait forever until a notification received. Clears all bits on exit since we'll handle every set bit in one
        // go
        xTaskNotifyWait(0x0, UINT32_MAX, &update_bits, portMAX_DELAY);

        log_printf(TAG,
                   LOG_LEVEL_INFO,
                   "update-conditions task received task notification of value 0x%02X, updating accordingly",
                   update_bits);

        if (update_bits & UPDATE_TIME_BIT) {
            screen_img_handler_draw_time();
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated time");
        }

        if (update_bits & UPDATE_CONDITIONS_BIT) {
            conditions_refresh();
            screen_img_handler_draw_conditions(&last_retrieved_conditions);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated conditions");
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            screen_img_handler_download_and_save(SCREEN_IMG_TIDE_CHART);
            screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated tide chart");
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            screen_img_handler_download_and_save(SCREEN_IMG_SWELL_CHART);
            screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated swell chart");
        }

        // Render after we've made all changes
        if (update_bits) {
            screen_img_handler_render();
        }
    }
}

void conditions_trigger_time_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_TIME_BIT, eSetBits);
}

void conditions_trigger_conditions_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_CONDITIONS_BIT, eSetBits);
}

void conditions_trigger_tide_chart_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_TIDE_CHART_BIT, eSetBits);
}

void conditions_trigger_swell_chart_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_SWELL_CHART_BIT, eSetBits);
}

void conditions_trigger_both_charts_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_SWELL_CHART_BIT | UPDATE_TIDE_CHART_BIT, eSetBits);
}

void conditions_update_task_start() {
    xTaskCreate(&conditions_update_task,
                "conditions-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 3,
                NULL,
                tskIDLE_PRIORITY,
                &conditions_update_task_handle);
}
