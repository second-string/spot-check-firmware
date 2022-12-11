#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_ota_ops.h"
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
#include "sleep_handler.h"
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
    nvs_init();
    i2c_init(BQ24196_I2C_PORT, BQ24196_I2C_SDA_PIN, BQ24196_I2C_SCL_PIN, &bq24196_i2c_handle);
    gpio_init();
    bq24196_init(&bq24196_i2c_handle);
    sntp_time_init();
    cd54hc4094_init(SHIFTREG_CLK_PIN, SHIFTREG_DATA_PIN, SHIFTREG_STROBE_PIN);
    display_init();
    sleep_handler_init();

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
    sleep_handler_start();
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
    log_printf(LOG_LEVEL_INFO, "");
    log_printf(LOG_LEVEL_INFO, "");
    while (cli_command_info(info_buffer, info_buffer_size, NULL) == pdTRUE) {
        log_printf(LOG_LEVEL_INFO, info_buffer);
    }
    log_printf(LOG_LEVEL_INFO, "");
    log_printf(LOG_LEVEL_INFO, "");
    free(info_buffer);
    info_buffer = NULL;

    app_start();

    const esp_partition_t *current_partition = esp_ota_get_running_partition();
    esp_app_desc_t         current_image_info;
    esp_ota_get_partition_description(current_partition, &current_image_info);
    display_render_splash_screen(current_image_info.version);

    // Wait for network settings that are necessary for operation to finish its startup sequence (including retries
    // internal to things like wifi and provisioning). If all are successful, proceed to wait for sntp time sync. If any
    // unsuccessful, show provisioning screen. Technically this could be error prone since it just makes sure all things
    // have completed (like STA connection retries -> forcing re-provisioning) by making sure the timeout is long enough
    // that all things have occurred
    bool     wifi_connected_to_network = false;
    bool     wifi_provisioned          = false;
    bool     connection_successful     = false;
    uint32_t start_ticks               = xTaskGetTickCount();
    uint32_t now_ticks                 = start_ticks;
    while (!connection_successful && (now_ticks - start_ticks < pdMS_TO_TICKS(10 * 1000))) {
        log_printf(LOG_LEVEL_INFO, "Waiting for successful boot criteria");
        wifi_connected_to_network = wifi_is_network_connected();
        wifi_provisioned          = wifi_is_provisioned();
        connection_successful     = wifi_connected_to_network && wifi_provisioned;

        vTaskDelay(pdMS_TO_TICKS(1000));
        now_ticks = xTaskGetTickCount();
    }

    if (!connection_successful) {
        log_printf(
            LOG_LEVEL_ERROR,
            "Connection unsuccessful, showing provisioning screen. wifi_connected_to_network: 0x%X - wifi_provisioned: "
            "0x%X",
            wifi_connected_to_network,
            wifi_provisioned);
        display_full_clear();
        display_draw_text(
            "Download the Spot Check app and follow\nthe configuration steps to connect\n your device to a wifi "
            "network",
            400,
            300,
            DISPLAY_FONT_SIZE_SHMEDIUM,
            DISPLAY_FONT_ALIGN_CENTER);

        screen_img_handler_render(__func__, __LINE__);
    } else {
        log_printf(LOG_LEVEL_INFO,
                   "Connection successful, showing 'fetching data' screen while waiting for time to sync");

        display_draw_text("Please wait, fetching latest conditions...",
                          400,
                          350,
                          DISPLAY_FONT_SIZE_SMALL,
                          DISPLAY_FONT_ALIGN_CENTER);
        screen_img_handler_render(__func__, __LINE__);

        // TODO :: This is still really hacky. Sometimes SNTP gets a new value within a second, sometimes it takes 45
        // seconds. I don't know enough about ntp to know if the SNTP init code actively sends a sync packet or just
        // passively waits for the next UDP broadcast. If the latter, need to figure out how to force it. If the former,
        // we're kind of at the whim of the slow update time.
        bool sntp_time_set = false;
        start_ticks        = xTaskGetTickCount();
        now_ticks          = start_ticks;
        while (!sntp_time_set && (now_ticks - start_ticks < pdMS_TO_TICKS(10 * 1000))) {
            log_printf(LOG_LEVEL_INFO, "Waiting for sntp time");
            sntp_time_set = sntp_time_is_synced();

            vTaskDelay(pdMS_TO_TICKS(1000));
            now_ticks = xTaskGetTickCount();
        }

        if (!sntp_time_set) {
            log_printf(LOG_LEVEL_ERROR, "Did not receive SNTP update before timing out! Showing all conditions anyway");
        }

        // Render whatever we have in flash to get up and showing asap, then kick off update to all
        spot_check_config *config = nvs_get_config();
        display_full_clear();
        screen_img_handler_draw_time();
        screen_img_handler_draw_spot_name(config->spot_name);
        screen_img_handler_draw_conditions(NULL);
        screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
        screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
        screen_img_handler_render(__func__, __LINE__);
        conditions_trigger_spot_name_update();
        conditions_trigger_conditions_update();
        conditions_trigger_both_charts_update();

        log_printf(LOG_LEVEL_INFO,
                   "Boot successful, showing time + last saved conditions / charts and kicking off update");
    }

    // Wait for all running 'processes' to finish (downloading and image, saving things to flash, running a display
    // update, etc) before entering deep sleep
    sleep_handler_block_until_system_idle();

    // yeet the default task, everything runs from conditions task, ota task, and timers
    vTaskDelete(NULL);
}
