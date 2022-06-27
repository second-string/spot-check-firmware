#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

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
#include "display.h"
#include "gpio.h"
#include "http_client.h"
#include "http_server.h"
#include "i2c.h"
#include "json.h"
#include "mdns_local.h"
#include "nvs.h"
#include "ota_task.h"
#include "raw_image.h"
#include "screen_img_handler.h"
#include "sntp_time.h"
#include "timer.h"
#include "uart.h"
#include "wifi.h"

#include "log.h"

#define TAG "sc-main"

#define CLI_UART UART_NUM_0
#define SHIFTREG_CLK_PIN GPIO_NUM_32
#define SHIFTREG_DATA_PIN GPIO_NUM_33
#define SHIFTREG_STROBE_PIN GPIO_NUM_12

static uart_handle_t cli_uart_handle;
static i2c_handle_t  bq24196_i2c_handle;

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
    gpio_init();
    bq24196_init(&bq24196_i2c_handle);
    sntp_time_init();
    cd54hc4094_init(SHIFTREG_CLK_PIN, SHIFTREG_DATA_PIN, SHIFTREG_STROBE_PIN);
    display_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    mdns_local_init();
    wifi_init();
    wifi_init_provisioning();
    http_client_init();

    cli_task_init(&cli_uart_handle);
    cli_command_register_all();
}

static void app_start() {
    i2c_start(&bq24196_i2c_handle);
    bq24196_start();
    display_start();

    wifi_start_provisioning(false);

    conditions_update_task_start();

    // minimal * 3 is the smallest we can go w/o SO
    // TODO :: move this into an ota_task_start() func
    TaskHandle_t ota_task_handle;
    xTaskCreate(&check_ota_update_task,
                "check-ota-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 3,
                NULL,
                tskIDLE_PRIORITY,
                &ota_task_handle);

    cli_task_start();

    // TODO :: blocking!!!
    sntp_time_start();
}

void app_main(void) {
    app_init();

    size_t info_buffer_size = 200 * sizeof(char);
    char  *info_buffer      = (char *)malloc(info_buffer_size);
    log_printf(TAG, LOG_LEVEL_INFO, "");
    log_printf(TAG, LOG_LEVEL_INFO, "");
    while (cli_command_info(info_buffer, info_buffer_size, NULL) == pdTRUE) {
        log_printf(TAG, LOG_LEVEL_INFO, info_buffer);
    }
    log_printf(TAG, LOG_LEVEL_INFO, "");
    log_printf(TAG, LOG_LEVEL_INFO, "");
    free(info_buffer);
    info_buffer = NULL;

    app_start();

    display_render_splash_screen();

    // TODO :: this needs to check if it's provisioned && gets network connection, or at least hold off on displaying
    // data in else clause until we pass internet connection. If provisioned but now not around that network, it's
    // currently displaying saved data but shouldn't.
    // Maybe a timeout for network connection before this screen is shown to give network a chance to try to connect for
    // a hot sec?
    if (!wifi_is_provisioned()) {
        display_full_clear();
        display_draw_text(
            "Download the Spot Check app and follow\nthe configuration steps to connect\n your device to a wifi "
            "network",
            400,
            300,
            DISPLAY_FONT_SIZE_SMALL,
            DISPLAY_FONT_ALIGN_CENTER);
    } else {
        // Render whatever we have in flash to get up and showing asap, then kick off update to all
        display_full_clear();
        screen_img_handler_draw_conditions(NULL);
        screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
        screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
        conditions_trigger_conditions_update();
        conditions_trigger_both_charts_update();
    }

    screen_img_handler_render();

    while (1) {
        // TODO :: wdg with tasks
        // ESP_ERROR_CHECK(esp_task_wdt_reset());

        // yielding 100 seconds every loop for now. I think we'll be able to take this loop out and delete the default
        // task
        vTaskDelay(pdMS_TO_TICKS(100000));

        /*
        TODO :: this whole logic block should be either a simple timer with a callback to run the fetch of new forecast
        info or in a task. We don't need to wait for user trigger to scroll now, so we can handle the update in the
        background if (false) { log_printf(TAG, LOG_LEVEL_DEBUG, "Button pressed");
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
        }
        */
    }
}
