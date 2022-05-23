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

#include "bq24196.h"
#include "cd54hc4094.h"
#include "cli_commands.h"
#include "cli_task.h"
#include "conditions_task.h"
#include "epd_driver.h"
#include "epd_highlevel.h"
#include "gpio_local.h"
#include "http_client.h"
#include "http_server.h"
#include "i2c.h"
#include "json.h"
#include "mdns_local.h"
#include "nvs.h"
#include "ota_task.h"
#include "timer.h"
#include "uart.h"
#include "wifi.h"
// #include "firasans_20.h"

#include "log.h"

#define TAG "sc-main"

#define CLI_UART UART_NUM_0
#define SHIFTREG_CLK_PIN GPIO_NUM_32
#define SHIFTREG_DATA_PIN GPIO_NUM_33
#define SHIFTREG_STROBE_PIN GPIO_NUM_12

#define FONT FiraSans_20

static uart_handle_t       cli_uart_handle;
static i2c_handle_t        bq24196_i2c_handle;
// static EpdiyHighlevelState hl;

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
                log_printf(TAG, LOG_LEVEL_INFO, "Got STA_CONN event");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                if (sta_connect_attempts < PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY) {
                    log_printf(TAG, LOG_LEVEL_INFO, "Got STA_DISCON, retrying to connect to the AP");
                    esp_wifi_connect();
                    sta_connect_attempts++;
                } else {
                    log_printf(TAG,
                               LOG_LEVEL_INFO,
                               "Got STA_DISCON and max retries, setting FAIL bit and kicking provision process");
                    wifi_init_provisioning();
                    wifi_start_provisioning(true);
                }
                break;
            }
            default:
                log_printf(TAG, LOG_LEVEL_INFO, "Got unknown WIFI event id: %d", event_id);
        }
    }

    // IP events, needed for actual connection to provisioned network once in STA mode
    if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                log_printf(TAG, LOG_LEVEL_INFO, "Setting CONNECTED bit, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
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
                log_printf(TAG, LOG_LEVEL_INFO, "Got unknown IP event id: %d", event_id);
        }
    }

    // Provisioning manager events
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_INIT:
                log_printf(TAG, LOG_LEVEL_INFO, "Provisioning inited event emitted");
                break;
            case WIFI_PROV_START:
                log_printf(TAG, LOG_LEVEL_INFO, "Provisioning started event emitted");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                size_t             ssid_len     = strnlen((char *)wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
                log_printf(TAG,
                           LOG_LEVEL_INFO,
                           "Received provisioning creds event - SSID: %s (length %d), PW: %s",
                           wifi_sta_cfg->ssid,
                           ssid_len,
                           wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                log_printf(TAG,
                           LOG_LEVEL_ERROR,
                           "Provisioning failed: %s",
                           (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "AP PW incorrect" : "AP not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                log_printf(TAG, LOG_LEVEL_INFO, "Provisioning successful event emitted");
                break;
            case WIFI_PROV_END: {
                log_printf(TAG, LOG_LEVEL_INFO, "Provisioning complete event emitted, de-initing prov mgr");
                wifi_deinit_provisioning();
                esp_restart();
                break;
            }
            case WIFI_PROV_DEINIT:
                log_printf(TAG, LOG_LEVEL_INFO, "Provisioning deinited event emitted");
                break;
            default:
                log_printf(TAG, LOG_LEVEL_INFO, "Received unsupported provisioning event: %d", event_id);
                break;
        }
    }
}

static void app_init() {
    // ESP_ERROR_CHECK(esp_task_wdt_init());

    // NULL passed for process_char callback, see cli_task_init for reasoning
    uart_init(CLI_UART,
              CLI_UART_RX_RING_BUFFER_BYTES,
              CLI_UART_TX_RING_BUFFER_BYTES,
              CLI_UART_QUEUE_SIZE,
              CLI_UART_RX_BUFFER_BYTES,
              NULL,
              &cli_uart_handle);
    log_init(&cli_uart_handle);

    // Init nvs to allow storage of wifi config directly to flash.
    // This enables us to pull config info directly out of flash after
    // first setup.
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-configuration-phase
    nvs_init();
    i2c_init(BQ24196_I2C_PORT, BQ24196_I2C_SDA_PIN, BQ24196_I2C_SCL_PIN, &bq24196_i2c_handle);
    debounce_handle = timer_init("debounce", button_timer_expired_callback, BUTTON_TIMER_PERIOD_MS * 1000);
    button_hold_handle =
        timer_init("button_hold", button_hold_timer_expired_callback, BUTTON_HOLD_TIMER_PERIOD_MS * 1000);
    gpio_init_local(button_isr_handler);
    bq24196_init(&bq24196_i2c_handle);
    cd54hc4094_init(SHIFTREG_CLK_PIN, SHIFTREG_DATA_PIN, SHIFTREG_STROBE_PIN);
    // epd_init(EPD_LUT_1K);
    // hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    mdns_local_init();
    wifi_init(default_event_handler);
    wifi_init_provisioning();
    http_client_init();

    cli_task_init(&cli_uart_handle);
    cli_command_register_all();
}

static void app_start() {
    i2c_start(&bq24196_i2c_handle);
    bq24196_start();

    wifi_start_provisioning(false);

    TaskHandle_t update_conditions_task_handle;
    xTaskCreate(&update_conditions_task,
                "update-conditions",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 3,
                NULL,
                tskIDLE_PRIORITY,
                &update_conditions_task_handle);

    // minimal * 3 is the smallest we can go w/o SO
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
    app_init();
    app_start();

    while (1) {
        // ESP_ERROR_CHECK(esp_task_wdt_reset());

        vTaskDelay(100 / portTICK_PERIOD_MS);

        // TODO :: move debounce logic to gpio file and register callback
        button_state_t current_button_state = gpio_debounce(debounce_handle, button_hold_handle);
        if (current_button_state == BUTTON_STATE_SINGLE_PRESS) {
            log_printf(TAG, LOG_LEVEL_DEBUG, "Button pressed");
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
                        log_printf(TAG, LOG_LEVEL_INFO, "Used to execute logic for adding text to scroll buffer here");
                    }
                } else {
                    log_printf(TAG, LOG_LEVEL_INFO, "Didn't get json array of strings to print, bailing");
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
            log_printf(TAG, LOG_LEVEL_INFO, "GPIO button held, erasing NVM then rebooting");
            esp_err_t err = nvs_full_erase();
            (void)err;
            esp_restart();
            while (1) {
                ;
            }
        }
    }
}
