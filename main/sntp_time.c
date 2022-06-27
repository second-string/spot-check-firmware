#include <sys/time.h>
#include <time.h>

#include "esp_sntp.h"

#include "log.h"
#include "sntp_time.h"

#define TAG "sc-sntp"

static void sntp_time_sync_notification_cb(struct timeval *tv) {
    (void)tv;

    time_t    now      = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_string[64];
    strftime(time_string, 64, "%c", &timeinfo);
    log_printf(TAG, LOG_LEVEL_DEBUG, "SNTP updated current time to %s", time_string);
}

void sntp_time_init() {
    sntp_setoperatingmode(SNTP_SYNC_MODE_IMMED);
    // TODO :: look into dhcp as primary server if we can update time from router w/o internet connection
    // https://github.com/espressif/esp-idf/blob/c2ccc383dae2a47c2c2dc8c7ad78175a3fd11361/examples/protocols/sntp/main/sntp_example_main.c#L139
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
    sntp_init();
}

void sntp_time_start() {
    // TODO :: blocking wait for time to be set, obviously shouldn't be blocking but need to make sure we don't show the
    // wrong time. Might be handled with no effort since we won't show screen till internet connection and the time sync
    // should happen quick enough that it will be accurate (depends on time sync interval in menuconfig) ((maybe force
    // time sync in internet connection event handler))
    int       retry       = 0;
    const int retry_count = 5;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        log_printf(TAG, LOG_LEVEL_DEBUG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
