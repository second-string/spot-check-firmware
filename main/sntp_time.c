#include "esp_sntp.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "log.h"
#include "sntp_time.h"

#define TAG SC_TAG_SNTP

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
    log_printf(LOG_LEVEL_DEBUG, "SNTP updated current time to %s", time_string);
}

void sntp_time_init() {
    sntp_setoperatingmode(SNTP_SYNC_MODE_IMMED);
    // TODO :: look into dhcp as primary server if we can update time from router w/o internet connection
    // https://github.com/espressif/esp-idf/blob/c2ccc383dae2a47c2c2dc8c7ad78175a3fd11361/examples/protocols/sntp/main/sntp_example_main.c#L139
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
}

void sntp_time_start() {
    // Restart returns true if sntp was running and it stopped +_re-inited, false if it wasn't running (and it does
    // nothing if so, so we manually init).
    if (!sntp_restart()) {
        sntp_init();
    }
}

void sntp_time_stop() {
    sntp_stop();
}

/*
 * Returns success if at least one time value has been received from remote
 */
bool sntp_time_is_synced() {
    sntp_sync_status_t status = sntp_get_sync_status();

    if (status == SNTP_SYNC_STATUS_COMPLETED) {
        log_printf(LOG_LEVEL_DEBUG, "SNTP fully synced");
        return true;
    }

    time_t    now      = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    char status_str[12];
    sntp_time_status_str(status_str);

    log_printf(LOG_LEVEL_DEBUG,
               "SNTP reported %s status, returning bool of current year > 1970 for sync check",
               status_str);
    return timeinfo.tm_year > 1970;
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
            MEMFAULT_ASSERT(0);
    }
}

/*
 * Pull the local time from the RTC and return in the tm struct. Assumes SNTP has already synced to an accurate value.
 */
void sntp_time_get_local_time(struct tm *now_local_out) {
    time_t    now            = 0;
    struct tm now_local_temp = {0};
    time(&now);
    localtime_r(&now, &now_local_temp);
    memcpy(now_local_out, &now_local_temp, sizeof(struct tm));
}

/*
 * time_string only neecds to be 6 chars. date_string is a toss up and we're blindly copying 64 bytes right now
 */
void sntp_time_get_time_str(struct tm *now_local, char *time_string, char *date_string) {
    if (time_string) {
        strftime(time_string, 6, "%H:%M", now_local);
    }

    if (date_string) {
        strftime(date_string, 64, "%A %B %d, %Y", now_local);
    }
}

void sntp_set_time(uint32_t epoch_secs) {
    // Must also set timzeone to UTC otherwise it will apply the current tz offset to the time set by the user
    sntp_set_tz_str("UTC0");

    const struct timeval time = {.tv_sec = epoch_secs, .tv_usec = 0};
    MEMFAULT_ASSERT(settimeofday(&time, NULL) == 0);
}

void sntp_set_tz_str(char *new_tz_str) {
    // EST +5 from GMT - DST starts 3rd month, second (2) sunday (0) at 2am (/2) - DST ends 11th month first (1) sunday
    // (0) at 2am (/2)
    //  "CET-1CEST,M3.5.0/2,M10.5.0/2" : CEST +2 from GMT - DST starts 3rd month, last (5) sunday (0) at 2am (/2) - DST
    //  ends 10th month last (5) sunday (0) at 2am (/2)
    // https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
    //
    // TODO :: 1. I don't know if 4 works for 'last' day of the month if there can ever be 5 of a day in a month (don't
    // think so) and 2. I don't know if sunday is still 0 for places like DE where the week starts on Monday (so it
    // would maybe be 7 instead of 0)
    setenv("TZ", new_tz_str, 1);
    tzset();
}
