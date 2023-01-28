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
#include "display.h"
#include "gpio.h"
#include "http_client.h"
#include "http_server.h"
#include "i2c.h"
#include "json.h"
#include "mdns_local.h"
#include "nvs.h"
#include "ota_task.h"
#include "scheduler_task.h"
#include "screen_img_handler.h"
#include "sleep_handler.h"
#include "sntp_time.h"
#include "timer.h"
#include "uart.h"
#include "wifi.h"

#include "log.h"

#define TAG SC_TAG_MAIN

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
    screen_img_handler_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    mdns_local_init();
    wifi_init();
    wifi_init_provisioning();
    http_client_init();

    scheduler_task_init();
    cli_task_init(&cli_uart_handle);
    cli_command_register_all();
}

static void app_start() {
    i2c_start(&bq24196_i2c_handle);
    bq24196_start();
    display_start();
    sleep_handler_start();
    sntp_time_start();
    scheduler_task_start();

    cli_task_start();
}

// TODO :: all of these screens need to go
// https://www.notion.so/pull-application-logic-out-into-spot_check-c-file-for-rendering-info-screens-conditions-network-par-2c21e6156eac437893a11a3925d60add
void spot_check_show_unprovisioned_screen() {
    log_printf(LOG_LEVEL_ERROR, "No prov info saved, showing provisioning screen without network checks.");
    display_full_clear();
    display_draw_text(
        "Download the Spot Check app and follow\nthe configuration steps to connect\n your device to a wifi "
        "network",
        400,
        300,
        DISPLAY_FONT_SIZE_SHMEDIUM,
        DISPLAY_FONT_ALIGN_CENTER);

    screen_img_handler_render(__func__, __LINE__);
}

void spot_check_show_no_network_screen() {
    log_printf(LOG_LEVEL_ERROR, "Prov info is saved, but could not find or connect to saved network.");
    display_full_clear();
    display_draw_text("Network not found", 400, 250, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text(
        "Spot Check could not find or connect to the network used previously.\nVerify network is "
        "available or use the Spot Check app to connect to a new network",
        400,
        300,
        DISPLAY_FONT_SIZE_SMALL,
        DISPLAY_FONT_ALIGN_CENTER);

    screen_img_handler_render(__func__, __LINE__);
}

void spot_check_clear_checking_connection_screen() {
    display_invert_text("Connecting to network...", 400, 350, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
    screen_img_handler_render(__func__, __LINE__);
}

void spot_check_show_checking_connection_screen() {
    log_printf(
        LOG_LEVEL_INFO,
        "Connection to network successful, showing 'connecting to network' screen while performing api healthcheck");
    display_draw_text("Connecting to network...", 400, 350, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
    screen_img_handler_render(__func__, __LINE__);
}

void spot_check_show_fetching_conditions_screen() {
    log_printf(
        LOG_LEVEL_INFO,
        "Connection to network successful, showing 'fetching data' screen while waiting for scheduler to full update");
    display_draw_text("Fetching latest conditions...", 400, 350, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
    screen_img_handler_render(__func__, __LINE__);
}

void spot_check_show_no_internet_screen() {
    log_printf(LOG_LEVEL_ERROR, "Connection to network successful and assigned IP, but no internet connection");
    display_full_clear();
    display_draw_text("No internet connection", 400, 250, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text("Spot Check is connected to the the WiFi\nnetwork but cannot reach the internet.",
                      400,
                      325,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    display_draw_text(
        "Verify local network is "
        "connected to the internet or\nuse the Spot Check app to connect to a new network",
        400,
        400,
        DISPLAY_FONT_SIZE_SMALL,
        DISPLAY_FONT_ALIGN_CENTER);

    screen_img_handler_render(__func__, __LINE__);
}

void app_main(void) {
    app_init();

    size_t info_buffer_size = 200 * sizeof(char);
    char  *info_buffer      = (char *)malloc(info_buffer_size);
    log_printf(LOG_LEVEL_INFO, "");
    log_printf(LOG_LEVEL_INFO, "");
    while (cli_command_info(info_buffer, info_buffer_size, NULL) == pdTRUE) {
        log_printf(LOG_LEVEL_INFO, "%s", info_buffer);
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

    // Enable breakout at each connectivity check of boot
    do {
        // If we're not even provisioned, give a little time for splash screen then start prov mgr and go straight to
        // displaying provisioning text. No need to waste time trying to connect to network.
        if (!wifi_is_provisioned()) {
            spot_check_show_unprovisioned_screen();
            wifi_init_provisioning();
            wifi_start_provisioning(false);
            break;
        }

        // De-init prov and kick off wifi to attempt to connect to STA network. Event loop will handle kicking scheduler
        // to offline mode if it can't connect to network, and below check will re-init prov mgr and kick us out of
        // startup logic
        wifi_deinit_provisioning();
        wifi_start_sta();

        // Wait for all network and wifi  event loops to settle after startup sequence (including retries internal to
        // those modules). If scheduler transitions out of init mode it either successfully got network conn or executed
        // STA_DISCON event and kicked scheduler to offline mode to poll, no reason to keep spinning here.
        uint32_t start_ticks = xTaskGetTickCount();
        uint32_t now_ticks   = start_ticks;
        while (!wifi_is_connected_to_network() && scheduler_get_mode() == SCHEDULER_MODE_INIT &&
               (now_ticks - start_ticks < pdMS_TO_TICKS(10 * 1000))) {
            log_printf(LOG_LEVEL_INFO, "Waiting for connection to wifi network and IP assignment");
            vTaskDelay(pdMS_TO_TICKS(1000));
            now_ticks = xTaskGetTickCount();
        }

        // If this is still false, it means we either couldn't find the provisioned network or just couldn't connect to
        // it. Regardless, event loop will kick scheduler to offline mode to continue trying to connect, and restart
        // provisioning here. This way, if it's just a network issue, the scheduler should keep retrying until it's
        // connected. If the old provisioned network actually isn't available anymore, prov mgr is running for user to
        // reprovision.
        if (!wifi_is_connected_to_network()) {
            spot_check_show_no_network_screen();

            // Have to re-init and restart since it would have been stopped and de-inited on initial startup due to
            // finding already provisioned network
            wifi_init_provisioning();
            wifi_start_provisioning(true);
            break;
        }

        // Update splash screen with fetching data text, then check actual internet connection. Start scheduler in
        // offline mode if failed
        spot_check_show_checking_connection_screen();
        if (!http_client_check_internet()) {
            spot_check_show_no_internet_screen();
            scheduler_set_offline_mode();
            break;
        }

        // SNTP check doesn't change our boot process, we just block here a bit to make the experience better to make it
        // less likely we render the epoch before it syncs correctly. Sometimes SNTP gets a new value within a second,
        // sometimes it takes 45 seconds. If sntp doesn't report fully synced,  we also check the date and as long as
        // it's not 1970 we call it synced to help speed up the process.
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
            log_printf(LOG_LEVEL_ERROR,
                       "Did not receive SNTP update before timing out! Non-blocking to rest of startup since we've "
                       "validated internet connection with healthcheck");
        }

        // TODO ::show time date and spot name here while other network stuff is fetched
        // All checks passed for full boot. Show fetching conditions screen then switching scheduler to online mode.
        // This will force update everything and thendo one big render.
        // TODO :: erase checking connection text
        spot_check_clear_checking_connection_screen();
        spot_check_show_fetching_conditions_screen();
        scheduler_set_online_mode();

        log_printf(LOG_LEVEL_INFO,
                   "Boot successful, showing time + last saved conditions / charts and kicked off conditions task");
    } while (0);

    // Delay a few minutes before we run the on-boot OTA check. This is because the esp ota version header check for the
    // server image uses its own internal http_client, so we can't force it to obey our http_client module request lock.
    // For a normal boot, waiting a few minutes ensures no further network connections will be running. There's still
    // the risk of edge cases for a late internet connection or provisioning that would force http errors from reqs
    // stommping each other, so this is just a dirtyish fix for now.
    uint8_t       initial_ota_delay_min = 3;
    TimerHandle_t initial_ota_timer     = xTimerCreate("initial-ota-timer",
                                                   pdMS_TO_TICKS(initial_ota_delay_min * SECS_PER_MIN * MS_PER_SEC),
                                                   pdFALSE,
                                                   NULL,
                                                   ota_task_start);
    if (initial_ota_timer == NULL) {
        log_printf(LOG_LEVEL_ERROR,
                   "Initial OTA timer kickoff could not be created!! OTA will eventually start checking when scheduler "
                   "in online mode, but this is a very bad sign about the memory levels!");
    } else {
        BaseType_t timer_success = xTimerStart(initial_ota_timer, 0);
        if (timer_success) {
            log_printf(LOG_LEVEL_INFO,
                       "Started timer to run initial boot OTA after %u minutes (%ums)",
                       initial_ota_delay_min,
                       initial_ota_delay_min * SECS_PER_MIN * MS_PER_SEC);
        } else {
            log_printf(LOG_LEVEL_ERROR,
                       "Failed to start initial OTA timer! OTA will eventually start checking when scheduler "
                       "in online mode, but this is a very bad sign about the memory levels",
                       initial_ota_delay_min,
                       initial_ota_delay_min * SECS_PER_MIN * MS_PER_SEC);
        }
    }

    // Wait for all running 'processes' to finish (downloading and image, saving things to flash, running a display
    // update, etc) before entering deep sleep
    sleep_handler_block_until_system_idle();

    // yeet the default task, everything runs from conditions task, ota task, and timers
    vTaskDelete(NULL);
}
