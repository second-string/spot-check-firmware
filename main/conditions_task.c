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

#define CONDITIONS_UPDATE_INTERVAL_MINUTES (20)
#define CHARTS_UPDATE_INTERVAL_MINUTES (60)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)

static TaskHandle_t conditions_update_task_handle;

static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;

static void conditions_timer_expired_callback(void *timer_args) {
    seconds_elapsed++;
    uint32_t minutes_elapsed = seconds_elapsed / 60;

    if ((minutes_elapsed % CONDITIONS_UPDATE_INTERVAL_MINUTES == 0) || new_location_set) {
        if (new_location_set) {
            // If we have currently scrolling text, clear the LEDs for us to push a new conditions string
            // led_text_stop_scroll();
        }

        new_location_set = false;
        conditions_trigger_conditions_update();
        log_printf(TAG, LOG_LEVEL_INFO, "Reached %d minutes elapsed, updating conditions...", minutes_elapsed);
    }

    if (minutes_elapsed % CHARTS_UPDATE_INTERVAL_MINUTES == 0) {
        conditions_trigger_both_charts_update();
        log_printf(TAG, LOG_LEVEL_INFO, "Reached %d minutes elapsed, updating conditions...", minutes_elapsed);
    }
}

static void conditions_refresh(conditions_t *new_conditions) {
    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 20];
    query_param        params[3];
    request            request = http_client_build_request("conditions", config, url_buf, params, 3);

    char                    *server_response    = NULL;
    size_t                   response_data_size = 0;
    esp_http_client_handle_t client;
    http_client_perform_request(&request, &client);
    esp_err_t http_err = http_client_read_response_to_buffer(&client, &server_response, &response_data_size);

    bool parse_success = false;
    if (http_err == ESP_OK && response_data_size != 0) {
        cJSON *json               = parse_json(server_response);
        cJSON *data_value         = cJSON_GetObjectItem(json, "data");
        cJSON *temperature_object = cJSON_GetObjectItem(data_value, "temp");
        cJSON *wind_speed_object  = cJSON_GetObjectItem(data_value, "wind_speed");
        cJSON *wind_dir_object    = cJSON_GetObjectItem(data_value, "wind_dir");
        cJSON *tide_height_object = cJSON_GetObjectItem(data_value, "tide_height");

        log_printf(TAG, LOG_LEVEL_DEBUG, "Server response: %s", server_response);
        parse_success = false;
        if (server_response != NULL) {
            free(server_response);
        }
        return;

        char *wind_dir_str    = cJSON_GetStringValue(wind_dir_object);
        char *tide_height_str = cJSON_GetStringValue(tide_height_object);

        // Set ints before copying string into buffer
        new_conditions->temperature = temperature_object->valueint;
        new_conditions->wind_speed  = wind_speed_object->valueint;
        strncpy(new_conditions->wind_dir, wind_dir_str, sizeof(new_conditions->wind_dir) - 1);
        new_conditions->wind_dir[sizeof(new_conditions->wind_dir) - 1] = '\0';
        strncpy(new_conditions->tide_height, tide_height_str, sizeof(new_conditions->tide_height) - 1);
        new_conditions->tide_height[sizeof(new_conditions->tide_height) - 1] = '\0';

        cJSON_free(data_value);
        cJSON_free(json);

        parse_success = true;
    } else {
        log_printf(TAG, LOG_LEVEL_INFO, "Failed to get new conditions, falling back to last saved values");
        new_conditions->temperature = last_retrieved_conditions.temperature;
        new_conditions->wind_speed  = last_retrieved_conditions.wind_speed;
        strncpy(new_conditions->wind_dir,
                last_retrieved_conditions.wind_dir,
                sizeof(last_retrieved_conditions.wind_dir) - 1);
        new_conditions->wind_dir[sizeof(new_conditions->wind_dir) - 1] = '\0';
        strncpy(new_conditions->tide_height,
                last_retrieved_conditions.tide_height,
                sizeof(last_retrieved_conditions.tide_height) - 1);
        new_conditions->tide_height[sizeof(new_conditions->tide_height) - 1] = '\0';

        parse_success = false;
    }

    // If all good, save values back to our persisted state
    // This is technically irrelevant - we don't need to pass in a struct to use those values, we should just save
    // to local variables to test validity then assign to global struct
    if (parse_success) {
        last_retrieved_conditions.temperature = new_conditions->temperature;
        last_retrieved_conditions.wind_speed  = new_conditions->wind_speed;
        strncpy(last_retrieved_conditions.wind_dir,
                new_conditions->wind_dir,
                sizeof(last_retrieved_conditions.wind_dir) - 1);
        new_conditions->wind_dir[sizeof(last_retrieved_conditions.wind_dir) - 1] = '\0';
        strncpy(last_retrieved_conditions.tide_height,
                new_conditions->tide_height,
                sizeof(last_retrieved_conditions.tide_height) - 1);
        new_conditions->tide_height[sizeof(last_retrieved_conditions.tide_height) - 1] = '\0';
    }

    // Caller responsible for freeing buffer if non-null on return
    if (server_response != NULL) {
        free(server_response);
    }
}

static void conditions_update_task(void *args) {
    timer_info_handle conditions_handle =
        timer_init("conditions",
                   conditions_timer_expired_callback,
                   NULL,
                   CONDITIONS_UPDATE_INTERVAL_MINUTES * SECONDS_PER_MIN * MS_PER_SECOND);
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

        if (update_bits & UPDATE_CONDITIONS_BIT) {
            conditions_t new_conditions;
            conditions_refresh(&new_conditions);
            conditions_display(&new_conditions);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated conditions");
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            screen_img_handler_download_and_save(SCREEN_IMG_TIDE_CHART);
            screen_img_handler_render_screen_img(SCREEN_IMG_TIDE_CHART);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated tide chart");
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            screen_img_handler_download_and_save(SCREEN_IMG_SWELL_CHART);
            screen_img_handler_render_screen_img(SCREEN_IMG_SWELL_CHART);
            log_printf(TAG, LOG_LEVEL_INFO, "update-conditions task updated swell chart");
        }
    }
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

void conditions_display(conditions_t *conditions) {
    char conditions_str[40] = {0};
    sprintf(conditions_str,
            "%dF %d%s %sft",
            conditions->temperature,
            conditions->wind_speed,
            conditions->wind_dir,
            conditions->tide_height);
    log_printf(TAG, LOG_LEVEL_INFO, "Showing: '%s'", conditions_str);
}

void conditions_display_last_retrieved() {
    conditions_display(&last_retrieved_conditions);
}

void conditions_update_task_start() {
    // Start off with update to all, then resume regular timer triggering
    conditions_trigger_conditions_update();
    conditions_trigger_both_charts_update();

    xTaskCreate(&conditions_update_task,
                "conditions-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 3,
                NULL,
                tskIDLE_PRIORITY,
                &conditions_update_task_handle);
}
