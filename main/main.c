#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "fonts.h"
#include "gpio.h"
#include "http_client.h"
#include "http_server.h"
#include "json.h"
#include "led_strip.h"
#include "led_text.h"
#include "mdns_local.h"
#include "nvs.h"
#include "timer.h"
#include "wifi.h"

#define LED_ROWS 6
#define LEDS_PER_ROW 50

typedef struct {
    int8_t  temperature;
    uint8_t wind_speed;
    char    wind_dir[4];     // 3 characters for dir (SSW, etc) plus null
    char    tide_height[7];  // minus sign, two digits, decimal point, two digits, null
} conditions_t;

static volatile int  sta_connect_attempts = 0;
static volatile bool connected_to_network = false;

/*
 * Initialize this to pass our check is (since we normally divide this num by 60)
 * to force a conditions request on startup
 */
static volatile unsigned int seconds_elapsed      = CONDITIONS_UPDATE_INTERVAL_MINUTES * 60;
static volatile bool         fetch_new_conditions = false;
static conditions_t          last_retrieved_conditions;

void button_timer_expired_callback(void *timer_args) {
    button_timer_expired = true;
}

void conditions_timer_expired_callback(void *timer_args) {
    seconds_elapsed++;

    if ((seconds_elapsed / 60) >= CONDITIONS_UPDATE_INTERVAL_MINUTES || new_location_set) {
        new_location_set     = false;
        seconds_elapsed      = 0;
        fetch_new_conditions = true;
    }
}

void button_isr_handler(void *arg) {
    button_pressed = !(bool)gpio_get_level(GPIO_BUTTON_PIN);
}

void default_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    // STA mode events (connecting to internet-based station)
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Got STA_CONN event");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                if (sta_connect_attempts < PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY) {
                    ESP_LOGI(TAG, "Got STA_DISCON, retrying to connect to the AP");
                    esp_wifi_connect();
                    sta_connect_attempts++;
                } else {
                    ESP_LOGI(TAG, "Got STA_DISCON and max retries, setting FAIL bit and kicking provision process");
                    wifi_init_provisioning();
                    wifi_start_provisioning(true);
                }
                break;
            }
            default:
                ESP_LOGI(TAG, "Got unknown WIFI event id: %d", event_id);
        }
    }

    // IP events, needed for actual connection to provisioned network once in STA mode
    if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG, "Setting CONNECTED bit, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                sta_connect_attempts = 0;
                mdns_advertise_tcp_service();
                connected_to_network = true;

                // We only start our http server upon IP assignment if this is a normal startup
                // in STA mode where we already have creds. If we're in this state after connecting
                // through a provisioning, we might not have enough sockets (I think) and the http server
                // start will fail sometimes. In the PROV_END case below, we force a reboot once we're done
                // provisioning, which always frees up the ability to successfully start the http_server
                if (!wifi_is_provisioning_inited) {
                    http_server_start();
                }
                break;
            }
            default:
                ESP_LOGI(TAG, "Got unknown IP event id: %d", event_id);
        }
    }

    // Provisioning manager events
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_INIT:
                ESP_LOGI(TAG, "Provisioning inited event emitted");
                break;
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started event emitted");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                size_t             ssid_len     = strnlen((char *)wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
                ESP_LOGI(TAG, "Received provisioning creds event - SSID: %s (length %d), PW: %s", wifi_sta_cfg->ssid,
                         ssid_len, wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed: %s",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "AP PW incorrect" : "AP not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful event emitted");
                break;
            case WIFI_PROV_END: {
                ESP_LOGI(TAG, "Provisioning complete event emitted, de-initing prov mgr");
                wifi_deinit_provisioning();
                esp_restart();
                break;
            }
            case WIFI_PROV_DEINIT:
                ESP_LOGI(TAG, "Provisioning deinited event emitted");
                break;
            default:
                ESP_LOGI(TAG, "Received unsupported provisioning event: %d", event_id);
                break;
        }
    }
}

void refresh_conditions(conditions_t *new_conditions) {
    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 20];
    query_param        params[3];
    request            request = http_client_build_request("conditions", config, url_buf, params, 3);

    char *server_response = NULL;
    int   data_length     = http_client_perform_request(&request, &server_response);
    bool  parse_success   = false;
    if (data_length != 0) {
        cJSON *json               = parse_json(server_response);
        cJSON *data_value         = cJSON_GetObjectItem(json, "data");
        cJSON *temperature_object = cJSON_GetObjectItem(data_value, "temp");
        cJSON *wind_speed_object  = cJSON_GetObjectItem(data_value, "wind_speed");
        cJSON *wind_dir_object    = cJSON_GetObjectItem(data_value, "wind_dir");
        cJSON *tide_height_object = cJSON_GetObjectItem(data_value, "tide_height");

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
        ESP_LOGI(TAG, "Failed to get new conditions, falling back to last saved values");
        new_conditions->temperature = last_retrieved_conditions.temperature;
        new_conditions->wind_speed  = last_retrieved_conditions.wind_speed;
        strncpy(new_conditions->wind_dir, last_retrieved_conditions.wind_dir,
                sizeof(last_retrieved_conditions.wind_dir) - 1);
        new_conditions->wind_dir[sizeof(new_conditions->wind_dir) - 1] = '\0';
        strncpy(new_conditions->tide_height, last_retrieved_conditions.tide_height,
                sizeof(last_retrieved_conditions.tide_height) - 1);
        new_conditions->tide_height[sizeof(new_conditions->tide_height) - 1] = '\0';

        parse_success = false;
    }

    // If all good, save values back to our persisted state
    // This is technically irrelevant - we don't need to pass in a struct to use those values, we should just save to
    // local variables to test validity then assign to global struct
    if (parse_success) {
        last_retrieved_conditions.temperature = new_conditions->temperature;
        last_retrieved_conditions.wind_speed  = new_conditions->wind_speed;
        strncpy(last_retrieved_conditions.wind_dir, new_conditions->wind_dir,
                sizeof(last_retrieved_conditions.wind_dir) - 1);
        new_conditions->wind_dir[sizeof(last_retrieved_conditions.wind_dir) - 1] = '\0';
        strncpy(last_retrieved_conditions.tide_height, new_conditions->tide_height,
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
    sprintf(conditions_str, "%d F - %s at %d mph - %s ft", conditions->temperature, conditions->wind_dir,
            conditions->wind_speed, conditions->tide_height);
    ESP_LOGI(TAG, "Showing: '%s'", conditions_str);
    led_text_show_text(conditions_str, strlen(conditions_str));
}

void update_conditions(void *args) {
    timer_info_handle conditions_handle =
        timer_init("conditions", conditions_timer_expired_callback, ONE_SECOND_TIMER_MS * 1000);
    timer_reset(conditions_handle, true);

    while (1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);

        if (!connected_to_network || !fetch_new_conditions) {
            continue;
        }
        fetch_new_conditions = false;

        conditions_t new_conditions;
        refresh_conditions(&new_conditions);
        display_conditions(&new_conditions);
    }
}

void app_main(void) {
    // ESP_ERROR_CHECK(esp_task_wdt_init());

    // Init nvs to allow storage of wifi config directly to flash.
    // This enables us to pull config info directly out of flash after
    // first setup.
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-configuration-phase
    nvs_init();
    timer_info_handle debounce_handle =
        timer_init("debounce", button_timer_expired_callback, BUTTON_TIMER_PERIOD_MS * 1000);
    gpio_init_local(button_isr_handler);
    led_strip_t *strip = led_strip_init_ws2812();
    strip->clear(strip);

    led_strip_funcs strip_funcs = {.set_pixel = strip->set_pixel, .show = strip->show};
    led_text_init(fonts_4x6, LED_ROWS, LEDS_PER_ROW, ZIGZAG, strip_funcs);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init(default_event_handler);
    mdns_init_local();
    wifi_init_provisioning();
    http_client_init();
    wifi_start_provisioning(false);

    TaskHandle_t update_conditions_task_handle;
    xTaskCreate(&update_conditions, "update-conditions", 8192 / 4, NULL, tskIDLE_PRIORITY,
                &update_conditions_task_handle);

    led_text_state previous_text_state = led_text_current_state;
    led_text_state current_state;
    char           text_to_scroll_buffer[5][50];
    unsigned int   next_index_to_scroll         = 0;
    unsigned int   num_available_text_to_scroll = 0;
    while (1) {
        // ESP_ERROR_CHECK(esp_task_wdt_reset());
        current_state = led_text_current_state;
        switch (current_state) {
            case IDLE:
            case STATIC:
                // If there's text available to scroll, doesn't matter if we got here from SCROLLING or IDLE, scroll it
                if (num_available_text_to_scroll > 0 && next_index_to_scroll < num_available_text_to_scroll) {
                    ESP_LOGI(TAG, "text in the buffer to scrolling, scrolling index %d", next_index_to_scroll);
                    char *text_to_scroll = text_to_scroll_buffer[next_index_to_scroll];
                    led_text_scroll_text_async(text_to_scroll, strlen(text_to_scroll), false);
                    next_index_to_scroll++;
                } else {
                    if (previous_text_state == SCROLLING) {
                        // We just became idle after scrolling and we have no more text to scroll,
                        // "clear" our buffer and index and re-show the conditions
                        num_available_text_to_scroll = 0;
                        next_index_to_scroll         = 0;

                        display_conditions(&last_retrieved_conditions);
                    }
                }
                break;
            case SCROLLING:
                break;
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);

        previous_text_state = current_state;

        if (button_was_released(debounce_handle)) {
            ESP_ERROR_CHECK(gpio_set_level(LED_PIN, !gpio_get_level(LED_PIN)));

            // Space for base url + endpoint. Query param space handled when building full url in perform_request func
            char               url_buf[strlen(URL_BASE) + 20];
            request            request;
            query_param        params[2];
            spot_check_config *config = nvs_get_config();

            char *next_forecast_type = get_next_forecast_type(config->forecast_types);
            request                  = http_client_build_request(next_forecast_type, config, url_buf, params, 2);

            char *server_response = NULL;
            int   data_length     = http_client_perform_request(&request, &server_response);
            if (data_length != 0) {
                cJSON *json       = parse_json(server_response);
                cJSON *data_value = cJSON_GetObjectItem(json, "data");
                if (cJSON_IsArray(data_value)) {
                    cJSON *data_list_value = NULL;
                    cJSON_ArrayForEach(data_list_value, data_value) {
                        char *text = cJSON_GetStringValue(data_list_value);
                        ESP_LOGI(TAG, "Adding new text to the buffer at index %d: '%s'", num_available_text_to_scroll,
                                 text);
                        strcpy(text_to_scroll_buffer[num_available_text_to_scroll], text);
                        num_available_text_to_scroll++;
                    }
                } else {
                    ESP_LOGI(TAG, "Didn't get json array of strings to print, bailing");
                }

                cJSON_free(data_value);
                cJSON_free(json);
            }

            // Caller responsible for freeing buffer if non-null on return
            if (server_response != NULL) {
                free(server_response);
            }
        }
    }
}
