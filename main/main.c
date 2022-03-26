#include <stdio.h>
#include <string.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "cli_task.h"
#include "conditions_task.h"
#include "fonts.h"
#include "gpio_local.h"
#include "http_client.h"
#include "http_server.h"
#include "json.h"
#include "mdns_local.h"
#include "nvs.h"
#include "ota_task.h"
#include "timer.h"
#include "uart.h"
#include "wifi.h"

#include "esp_log.h"

#define TAG "sc-main"

#define CLI_UART UART_NUM_0

uart_handle_t cli_uart_handle;

static volatile int sta_connect_attempts = 0;

static timer_info_handle button_hold_handle;
static timer_info_handle debounce_handle;

void button_timer_expired_callback(void *timer_args) {
    button_timer_expired = true;
}

void button_hold_timer_expired_callback(void *timer_args) {
    button_hold_timer_expired = true;
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
                // mdns_advertise_tcp_service();

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
                ESP_LOGI(TAG,
                         "Received provisioning creds event - SSID: %s (length %d), PW: %s",
                         wifi_sta_cfg->ssid,
                         ssid_len,
                         wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG,
                         "Provisioning failed: %s",
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

static void app_init() {
    // ESP_ERROR_CHECK(esp_task_wdt_init());

    // Init nvs to allow storage of wifi config directly to flash.
    // This enables us to pull config info directly out of flash after
    // first setup.
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-configuration-phase
    nvs_init();
    uart_init(CLI_UART, CLI_UART_RX_BUFFER_BYTES, CLI_UART_TX_BUFFER_BYTES, CLI_UART_QUEUE_SIZE, &cli_uart_handle);

    debounce_handle = timer_init("debounce", button_timer_expired_callback, BUTTON_TIMER_PERIOD_MS * 1000);
    button_hold_handle =
        timer_init("button_hold", button_hold_timer_expired_callback, BUTTON_HOLD_TIMER_PERIOD_MS * 1000);
    gpio_init_local(button_isr_handler);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    mdns_local_init();
    wifi_init(default_event_handler);
    wifi_init_provisioning();
    http_client_init();

    cli_task_init(CLI_UART);
}

static void app_start() {
    wifi_start_provisioning(false);

    TaskHandle_t update_conditions_task_handle;
    xTaskCreate(&update_conditions_task,
                "update-conditions",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES,
                NULL,
                tskIDLE_PRIORITY,
                &update_conditions_task_handle);

    // minimal * 3 is the smallest we can go w/o SO - are we (or the ota sdk) allocing space for the binary chunks on
    // the stack????
    // Actually empty CLI task SOs at single minimal so maybe not. Sheesh big tasks. conditions task seems fine on one
    // minimal?
    TaskHandle_t ota_task_handle;
    xTaskCreate(&check_ota_update_task,
                "check-ota-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 3,
                NULL,
                tskIDLE_PRIORITY,
                &ota_task_handle);

    cli_task_start();
}

void app_main(void) {
    ESP_LOGE(TAG, "minimal: %u", configMINIMAL_STACK_SIZE);
    app_init();
    app_start();

    while (1) {
        // ESP_ERROR_CHECK(esp_task_wdt_reset());

        vTaskDelay(100 / portTICK_PERIOD_MS);

        // TODO :: move debounce logic to gpio file and register callback
        button_state_t current_button_state = gpio_debounce(debounce_handle, button_hold_handle);
        if (current_button_state == BUTTON_STATE_SINGLE_PRESS) {
            ESP_ERROR_CHECK(gpio_set_level(LED_PIN, !gpio_get_level(LED_PIN)));

            // Space for base url + endpoint. Query param space handled when building full url in perform_request
            // func
            char               url_buf[strlen(URL_BASE) + 20];
            request            request;
            query_param        params[2];
            spot_check_config *config = nvs_get_config();

            char *next_forecast_type = get_next_forecast_type(config->forecast_types);
            request                  = http_client_build_request(next_forecast_type, config, url_buf, params, 2);

            // TODO :: both the request performing and the executed logic for doing something with the spot check
            // response strings should be in another task / file
            char *server_response = NULL;
            int   data_length     = http_client_perform_request(&request, &server_response);
            if (data_length != 0) {
                cJSON *json       = parse_json(server_response);
                cJSON *data_value = cJSON_GetObjectItem(json, "data");
                if (cJSON_IsArray(data_value)) {
                    cJSON *data_list_value = NULL;
                    cJSON_ArrayForEach(data_list_value, data_value) {
                        ESP_LOGI(TAG, "Used to execute logic for adding text to scroll buffer here");
                    }
                } else {
                    ESP_LOGI(TAG, "Didn't get json array of strings to print, bailing");
                }

                if (data_value != NULL) {
                    cJSON_free(data_value);
                }
                if (json != NULL) {
                    cJSON_free(json);
                }
            }

            // Caller responsible for freeing buffer if non-null on return
            if (server_response != NULL) {
                free(server_response);
            }
        } else if (current_button_state == BUTTON_STATE_HOLD) {
            // Currently erase full NVM when gpio button is held - can be scoped in the future just to stored wifi
            // provisioning data. Throw away return value, internal function will print out error if occurs. Reboot
            // after to get back into provisoning state
            ESP_LOGI(TAG, "GPIO button held, erasing NVM then rebooting");
            esp_err_t err = nvs_full_erase();
            (void)err;
            esp_restart();
            while (1) {
                ;
            }
        }
    }
}
