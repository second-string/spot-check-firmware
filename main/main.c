#include <stdio.h>
#include <string.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mdns.h"
#include "cJSON.h"

#include "driver/gpio.h"

#include "constants.h"
#include "nvs.h"
#include "wifi.h"
#include "mdns_local.h"
#include "gpio.h"
#include "timer.h"
#include "http_client.h"
#include "json.h"

static int sta_connect_attempts = 0;

void timer_expired_callback(void *timer_args) {
    timer_expired = true;
}

void button_isr_handler(void *arg) {
    button_pressed = !(bool)gpio_get_level(GPIO_BUTTON_PIN);
}

void default_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
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
                    ESP_LOGI(TAG,"Got STA_DISCON and max retries, setting FAIL bit and kicking provision process");
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
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                ESP_LOGI(TAG, "Setting CONNECTED bit, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                sta_connect_attempts = 0;
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
                size_t ssid_len = strnlen((char *)wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
                ESP_LOGI(TAG, "Received provisioning creds event - SSID: %s (length %d), PW: %s", wifi_sta_cfg->ssid, ssid_len, wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed: %s", (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "AP PW incorrect" : "AP not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful event emitted");
                break;
            case WIFI_PROV_END: {
                ESP_LOGI(TAG, "Provisioning complete event emitted, de-initing prov mgr");
                wifi_deinit_provisioning();
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

void app_main(void) {
    // ESP_ERROR_CHECK(esp_task_wdt_init());

    // Init nvs to allow storage of wifi config directly to flash.
    // This enables us to pull config info directly out of flash after
    // first setup.
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-configuration-phase
    nvs_init();
    timer_init(timer_expired_callback);
    gpio_init_local(button_isr_handler);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init(default_event_handler);
    mdns_init_local();
    wifi_init_provisioning();
    http_client_init();
    wifi_start_provisioning(false);

    bool tides = false;
    while (1) {
        // ESP_ERROR_CHECK(esp_task_wdt_reset());

        if (button_was_released()) {
            ESP_ERROR_CHECK(gpio_set_level(LED_PIN, !gpio_get_level(LED_PIN)));

            // Space for base url, endpoint, and some extra
            char url_buf[strlen(URL_BASE) + 20];
            request request;
            query_param params[2];
            if (tides) {
                request = http_client_build_request("tides", "wedge", "2", url_buf, params);
                tides = false;
            } else {
                request = http_client_build_request("swell", "wedge", "2", url_buf, params);
                tides = true;
            }

            char *server_response;
            int data_length = http_client_perform_request(&request, &server_response);
            if (data_length != 0) {
                cJSON *json = parse_json(server_response);

                cJSON *data_value = cJSON_GetObjectItem(json, "data");
                char *pretty = cJSON_Print(data_value);
                ESP_LOGI(TAG, "%s", pretty);
                free(pretty);

                // int values_written = send_json_list(data_value);
                // assert(values_written > 0);

                cJSON_free(data_value);
                cJSON_free(json);
            }

            // Caller responsible for freeing buffer if non-null on return
            if (server_response != NULL) {
                free(server_response);
            }
        }
    }

    // wifi_init_softap_sta(default_event_handler);
    // httpd_handle_t server = http_server_start();
}
