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
#include "memfault/core/data_packetizer.h"
#include "memfault/esp_port/core.h"
#include "memfault_interface.h"
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
#include "spot_check.h"
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

/*
 * Trigger mflt then wait 2 seconds to make sure scheduler begins executing mflt upload. Then trigger ota check so it
 * has to wait until scheduler loops back around to process the new event bit, otherwise the un-locked ota http reqs
 * will break the mflt one. This is a temporary hack until the http req system is refactored.
 */
static void special_case_boot_delayed_callback() {
    if (scheduler_get_mode() == SCHEDULER_MODE_INIT) {
        // We're in provisioning mode, don't bother with these calls
        log_printf(LOG_LEVEL_DEBUG, "Skipping special case boot delay callback since device not connected");
        return;
    }

    log_printf(LOG_LEVEL_DEBUG, "Starting special case boot delay callback");

    // Reset memfault back to full uploads for the rest of runtime
    memfault_packetizer_set_active_sources(kMfltDataSourceMask_All);

    scheduler_schedule_mflt_upload();
    scheduler_trigger();
    vTaskDelay(pdMS_TO_TICKS(2000));
    scheduler_schedule_ota_check();
    scheduler_trigger();
    log_printf(LOG_LEVEL_DEBUG, "Exiting special case boot delay callback");
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
    nvs_init();
    spot_check_init();

    // Strings used for serial/fw/hw versions must be set (aka spot_check_init called) before memfault_boot called!
#if !CONFIG_MEMFAULT_AUTOMATIC_INIT
    memfault_boot();
#endif

    // Note: intentionally don't init provisioning here as it doesn't need to be inited to check if device is
    // provisioned or not (it's just a NVS check). Only if unprovisioned, or network connection fails, do we init and
    // start provisioning in same step
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
    http_client_init();

    scheduler_task_init();
    cli_task_init(&cli_uart_handle);
    cli_command_register_all();
}

static void app_start() {
    nvs_start();
    i2c_start(&bq24196_i2c_handle);
    bq24196_start();
    display_start();
    sleep_handler_start();
    sntp_time_start();
    scheduler_task_start();

    cli_task_start();
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

    spot_check_config_t *config = nvs_get_config();
    log_printf(LOG_LEVEL_INFO, "Operating mode: '%s'", spot_check_mode_to_string(config->operating_mode));
    sntp_set_tz_str(config->tz_str);
    display_render_splash_screen(spot_check_get_fw_version(), spot_check_get_hw_version());

    // Enable breakout at each connectivity check of boot
    do {
        // If we're not even provisioned, give a little time for splash screen then start prov mgr and go straight to
        // displaying provisioning text. No need to waste time trying to connect to network.
        if (!wifi_is_provisioned()) {
            spot_check_show_unprovisioned_screen();
            spot_check_render();
            wifi_init_provisioning();
            wifi_start_provisioning();
            break;
        }

        // Kick off wifi to attempt to connect to STA network. Event loop will handle kicking scheduler to offline mode
        // if it can't connect to network, and below check will re-init prov mgr and kick us out of startup logic
        wifi_start_sta();

        // Wait for all network and wifi  event loops to settle after startup sequence (including retries internal to
        // those modules). If scheduler transitions out of init mode it either successfully got network conn or executed
        // STA_DISCON event and kicked scheduler to offline mode to poll, no reason to keep spinning here.
        const uint8_t max_wait_secs     = 60;
        uint8_t       current_wait_secs = 0;
        while (!wifi_is_connected_to_network() && scheduler_get_mode() == SCHEDULER_MODE_INIT &&
               (current_wait_secs < max_wait_secs)) {
            log_printf(LOG_LEVEL_INFO, "Waiting for connection to wifi network and IP assignment");
            vTaskDelay(pdMS_TO_TICKS(1000));

            // TODO :: I don't think this is actually doing anything, need a way to actually test it
            if (current_wait_secs == 30) {
                log_printf(LOG_LEVEL_INFO,
                           "30 seconds elapsed with no wifi connection still, kicking/restarting wifi connection");

                ESP_ERROR_CHECK(esp_wifi_stop());
                ESP_ERROR_CHECK(esp_wifi_start());
            }

            current_wait_secs++;
        }

        // If this is still false, it means we either couldn't find the provisioned network or just couldn't connect to
        // it. Regardless, we give up entirely for the remainder of this boot on forming a successful conneciton.
        // Scheduler stays in INIT mode and prov mgr is started. If network actually existss and we just couldn't
        // connect, user will have to reboot. If they successfully provision then event loop will reboot with new prov
        // creds. Too many problems with trying to support provisioning while also healthchecking to reconnect to maybe
        // still-existing network because the http client and the prov manager use the same radio, one will always
        // break.
        if (!wifi_is_connected_to_network()) {
            spot_check_show_no_network_screen();
            spot_check_render();
            wifi_init_provisioning();
            wifi_start_provisioning();
            break;
        }

        // Update splash screen with fetching data text, then check actual internet connection
        spot_check_show_checking_connection_screen();
        spot_check_render();
        if (!http_client_check_internet()) {
            log_printf(LOG_LEVEL_WARN,
                       "Failed healthcheck after being assigned IP. Waiting 5 seconds then trying again.");
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (!http_client_check_internet()) {
                log_printf(LOG_LEVEL_WARN, "Failed second healthcheck, fail out to prov");
                spot_check_show_no_internet_screen();
                spot_check_render();
                wifi_init_provisioning();
                wifi_start_provisioning();
                break;
            }
            log_printf(LOG_LEVEL_INFO, "Succeeded on second healthcheck request");
        }

        // Only enable heartbeat events on boot. This enables a quick heartbeat as soon as wifi conn. established
        // without blocking the remainder of boot for a big coredump upload (if it exists). Mask needs to be reset after
        // boot is complete so we properly upload a coredump if we have one.
        memfault_packetizer_set_active_sources(kMfltDataSourceMask_Event);
        memfault_interface_post_data();

        // SNTP check doesn't change our boot process, we just block here a bit to make the experience better to
        // make it less likely we render the epoch before it syncs correctly. Sometimes SNTP gets a new value within
        // a second, sometimes it takes 45 seconds. If sntp doesn't report fully synced,  we also check the date and
        // as long as it's not 1970 we call it synced to help speed up the process.
        // NOTE :: if boot proceeds without time sync and then it happens later, all the differential updates will fire
        // again because they see a huge time jump.
        bool     sntp_time_set = false;
        uint32_t start_ticks   = xTaskGetTickCount();
        uint32_t now_ticks     = start_ticks;
        log_printf(LOG_LEVEL_INFO, "Waiting for sntp time");
        while (!sntp_time_set && (now_ticks - start_ticks < pdMS_TO_TICKS(30 * 1000))) {
            sntp_time_set = sntp_time_is_synced();

            vTaskDelay(pdMS_TO_TICKS(1000));
            now_ticks = xTaskGetTickCount();
        }

        if (!sntp_time_set) {
            log_printf(LOG_LEVEL_WARN,
                       "Did not receive SNTP update before timing out! Non-blocking to rest of startup since we've "
                       "validated internet connection with healthcheck");
        } else {
            log_printf(LOG_LEVEL_INFO,
                       "Successfully synced SNTP time after %lu seconds",
                       (now_ticks - start_ticks) / configTICK_RATE_HZ);
        }

        // TODO ::show time date and spot name here while other network stuff is fetched
        // All checks passed for full boot. Show fetching conditions screen then switching scheduler to online mode.
        // This will force update everything and thendo one big render.
        spot_check_clear_checking_connection_screen();
        scheduler_set_online_mode();

        log_printf(LOG_LEVEL_INFO, "Boot successful, kicking scheduler taks into online mode");
    } while (0);

    // Delay a minute before we run the on-boot delayed actions. This is because both mflt's http client and the
    // esp ota version header check for the server image use their own internal http_client, so we can't force them to
    // obey our http_client module request lock. For a normal boot, waiting a minute or two ensures no further network
    // connections will be running. There's still the risk of edge cases for a late internet connection or provisioning
    // that would force http errors from reqs stommping each other, so this is just a dirtyish fix for now.
    uint8_t       initial_boot_delay_min = 1;
    TimerHandle_t initial_boot_delay_timer =
        xTimerCreate("initial-boot-delay-timer",
                     pdMS_TO_TICKS(initial_boot_delay_min * SECS_PER_MIN * MS_PER_SEC),
                     pdFALSE,
                     NULL,
                     special_case_boot_delayed_callback);
    if (initial_boot_delay_timer == NULL) {
        log_printf(LOG_LEVEL_ERROR,
                   "Initial boot delay timer kickoff could not be created!! MFLT and OTA will eventually upload / "
                   "check when scheduler "
                   "in online mode, but this is a very bad sign about the memory levels!");
    } else {
        BaseType_t timer_success = xTimerStart(initial_boot_delay_timer, 0);
        if (timer_success) {
            log_printf(LOG_LEVEL_INFO,
                       "Started timer to run initial boot delayed actions after %u minutes (%ums)",
                       initial_boot_delay_min,
                       initial_boot_delay_min * SECS_PER_MIN * MS_PER_SEC);
        } else {
            log_printf(
                LOG_LEVEL_ERROR,
                "Failed to start initial boot delay timer! MFLT & OTA will eventually start checking when scheduler "
                "in online mode, but this is a very bad sign about the memory levels",
                initial_boot_delay_min,
                initial_boot_delay_min * SECS_PER_MIN * MS_PER_SEC);
        }
    }

    // Wait for all running 'processes' to finish (downloading and image, saving things to flash, running a display
    // update, etc) before entering deep sleep
    sleep_handler_block_until_system_idle();

    // yeet the default task, everything runs from scheduler task, ota task, and timers
    vTaskDelete(NULL);
}
