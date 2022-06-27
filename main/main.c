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
    sntp_time_start();
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

    // Wait for each system process that's necessary for operation to finish its startup sequence (including retries
    // internal to things like wifi and provisioning). If all are successful, proceed to normal operation and diplay of
    // info. If any unsuccessful, show provisioning screen.
    // Technically this could be error prone since it just makes sure all things have completed (like STA connection
    // retries -> forcing re-provisioning) by making sure the timeout is long enough that all things have occurred
    bool     wifi_connected_to_network = false;
    bool     wifi_provisioned          = false;
    bool     sntp_time_set             = false;
    bool     boot_successful           = false;
    uint32_t start_ticks               = xTaskGetTickCount();
    uint32_t now_ticks                 = start_ticks;
    while (!boot_successful && (now_ticks - start_ticks < pdMS_TO_TICKS(10 * 1000))) {
        log_printf(TAG, LOG_LEVEL_INFO, "Waiting for successful boot criteria");
        wifi_connected_to_network = wifi_is_network_connected();
        wifi_provisioned          = wifi_is_provisioned();
        sntp_time_set             = sntp_time_is_synced();
        boot_successful           = wifi_connected_to_network && wifi_provisioned && sntp_time_set;

        vTaskDelay(pdMS_TO_TICKS(1000));
        now_ticks = xTaskGetTickCount();
    }

    if (!boot_successful) {
        // TODO :: should do more detailed error handling here based on the individual bool success values
        log_printf(
            TAG,
            LOG_LEVEL_ERROR,
            "Boot unsuccessful, showing provisioning screen. wifi_connected_to_network: 0x%X - wifi_provisioned: "
            "0x%X - sntp_time_set: 0x%X",
            wifi_connected_to_network,
            wifi_provisioned,
            sntp_time_set);
        display_full_clear();
        display_draw_text(
            "Download the Spot Check app and follow\nthe configuration steps to connect\n your device to a wifi "
            "network",
            400,
            300,
            DISPLAY_FONT_SIZE_SMALL,
            DISPLAY_FONT_ALIGN_CENTER);
    } else {
        log_printf(TAG,
                   LOG_LEVEL_INFO,
                   "Boot successful, showing time + last saved conditions / charts and kicking off update");

        // Render whatever we have in flash to get up and showing asap, then kick off update to all
        display_full_clear();
        screen_img_handler_draw_time();
        screen_img_handler_draw_conditions(NULL);
        screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
        screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
        conditions_trigger_conditions_update();
        conditions_trigger_both_charts_update();
    }

    screen_img_handler_render();

    // yeet the default task, everything run from conditions task, ota task, and timers
    vTaskDelete(NULL);
}
