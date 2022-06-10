#include <string.h>

#include "freertos/FreeRTOS.h"
#include "log.h"

#include "conditions_task.h"
#include "gpio.h"
#include "http_client.h"
#include "json.h"
#include "nvs.h"
#include "timer.h"
#include "wifi.h"

#define TAG "sc-conditions-task"

#define CONDITIONS_UPDATE_INTERVAL_MINUTES (20)

/*
 * Initialize this to pass our check is (since we normally divide this num by 60)
 * to force a conditions request on startup
 */
static volatile unsigned int seconds_elapsed      = CONDITIONS_UPDATE_INTERVAL_MINUTES * 60;
static volatile bool         fetch_new_conditions = false;
static conditions_t          last_retrieved_conditions;

static void conditions_timer_expired_callback(void *timer_args) {
    seconds_elapsed++;

    if ((seconds_elapsed / 60) >= CONDITIONS_UPDATE_INTERVAL_MINUTES || new_location_set) {
        if (new_location_set) {
            // If we have currently scrolling text, clear the LEDs for us to push a new conditions string
            // led_text_stop_scroll();
        }

        new_location_set     = false;
        seconds_elapsed      = 0;
        fetch_new_conditions = true;
        log_printf(TAG, LOG_LEVEL_INFO, "Reached %d minutes elapsed, updating conditions...", seconds_elapsed / 60);
    }
}

static void refresh_conditions(conditions_t *new_conditions) {
    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 20];
    query_param        params[3];
    request            request = http_client_build_request("conditions", config, url_buf, params, 3);

    char                    *server_response    = NULL;
    size_t                   alloced_space_used = 0;
    esp_http_client_handle_t client;
    http_client_perform_request(&request, &client);
    ESP_ERROR_CHECK(http_client_read_response(&client, &server_response, &alloced_space_used));
    bool parse_success = false;
    if (alloced_space_used != 0) {
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

void display_conditions(conditions_t *conditions) {
    char conditions_str[40] = {0};
    sprintf(conditions_str,
            "%dF %d%s %sft",
            conditions->temperature,
            conditions->wind_speed,
            conditions->wind_dir,
            conditions->tide_height);
    log_printf(TAG, LOG_LEVEL_INFO, "Showing: '%s'", conditions_str);
    // led_text_show_text(conditions_str, strlen(conditions_str));
}

void display_last_retrieved_conditions() {
    display_conditions(&last_retrieved_conditions);
}

void update_conditions_task(void *args) {
    timer_info_handle conditions_handle =
        timer_init("conditions",
                   conditions_timer_expired_callback,
                   NULL,
                   CONDITIONS_UPDATE_INTERVAL_MINUTES * SECONDS_PER_MIN * MS_PER_SECOND);
    timer_reset(conditions_handle, true);

    // Wait forever until connected
    wifi_block_until_connected();

    while (1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);

        if (!fetch_new_conditions) {
            continue;
        }

        fetch_new_conditions = false;

        log_printf(
            TAG,
            LOG_LEVEL_INFO,
            "Picked up fetch_new_conditions flag set in conditions task main loop, running refresh then display");
        conditions_t new_conditions;
        refresh_conditions(&new_conditions);
        display_conditions(&new_conditions);
    }
}
