#include <sys/time.h>
#include <time.h>

#include "esp_sntp.h"

#include "log.h"
#include "sntp_time.h"

#define TAG "sc-sntp"

/*
 * Callback that fires every time the SNTP service syncs system time with received rmeote value. Superfluous with the
 * sntp_time_is_synced function below unless we wanted this to set a flag so we didn't have to poll that function
 */
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
}

void sntp_time_start() {
    // Restart returns true if sntp was running and it stopped +_re-inited, false if it wasn't running (and it does
    // nothing if so, so we manually init).
    if (!sntp_restart()) {
        sntp_init();
    }
}

/*
 * Returns success if at least one time value has been received from remote
 */
bool sntp_time_is_synced() {
    sntp_sync_status_t status = sntp_get_sync_status();
    char               status_str[12];
    sntp_time_status_str(status_str);

    log_printf(TAG, LOG_LEVEL_DEBUG, "Checking SNTP time status, currently: %s", status_str);
    return status == SNTP_SYNC_STATUS_COMPLETED;
}

/*
 * Max length needed by out_str buffer is 12 bytes
 */
void sntp_time_status_str(char *out_str) {
    sntp_sync_status_t status = sntp_get_sync_status();
    switch (status) {
        case SNTP_SYNC_STATUS_RESET:
            strcpy(out_str, "reset");
            break;
        case SNTP_SYNC_STATUS_IN_PROGRESS:
            strcpy(out_str, "in progress");
            break;
        case SNTP_SYNC_STATUS_COMPLETED:
            strcpy(out_str, "completed");
            break;
        default:
            configASSERT(0);
    }
}
