#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/uart.h"

#include "log.h"
#include "sntp_time.h"

// Should match CLI_UART_TX_BYTES probably
#define LOG_OUT_BUFFER_BYTES (512)

static uart_handle_t    *cli_uart_handle;
static char             *log_out_buffer;
static SemaphoreHandle_t mutex_handle;
static StaticSemaphore_t mutex_buffer;
static log_level_t       max_log_level;
static uint32_t          tag_blacklist;
static char              time_str_buffer[20];

void log_init(uart_handle_t *cli_handle) {
    assert(cli_handle);

    log_out_buffer = pvPortMalloc(LOG_OUT_BUFFER_BYTES * sizeof(uint8_t));
    assert(log_out_buffer);

    mutex_handle = xSemaphoreCreateMutexStatic(&mutex_buffer);
    assert(mutex_handle);

    cli_uart_handle = cli_handle;
    max_log_level   = LOG_LEVEL_DEBUG;
    tag_blacklist   = 0x00000000;
    memset(time_str_buffer, 0x00, sizeof(time_str_buffer));
}

void log_log_line(sc_tag_t tag, log_level_t level, char *fmt, ...) {
    // Drop log line entirely if max level set less verbose than line verbosity OR there is at least one tag blacklisted
    // (aka don't show) and the bitmask of the tag enum val matches what's in the blacklist
    if (level > max_log_level || (tag_blacklist > 0 && (tag_blacklist & (1 << tag)))) {
        return;
    }

    BaseType_t rval = xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(50));
    // TODO :: could handle this more elegantly
    assert(rval == pdTRUE);

    // Uncomment following block to printf the literal format string for format specifier debugging
    /*
    char     dbg_buffer[LOG_OUT_BUFFER_BYTES];
    char    *p   = fmt;
    uint16_t idx = 0;
    while (*p && idx < LOG_OUT_BUFFER_BYTES) {
        dbg_buffer[idx++] = *p;
        if (*p == '%') {
            dbg_buffer[idx++] = '%';
        }

        p++;
    }

    printf("%s\n", dbg_buffer);
    */

    va_list args;
    va_start(args, fmt);

    // vsnprintf subtracts 1 from the max length passed to leave room for the null term
    size_t formatted_size = vsnprintf(log_out_buffer, LOG_OUT_BUFFER_BYTES, fmt, args);
    va_end(args);

    xSemaphoreGive(mutex_handle);

    // Let go of the mutex before sending to serial, it'll handle re-entrancy with it's own internal queueing
    // (maybe?)
    uart_write_bytes(cli_uart_handle->port, log_out_buffer, formatted_size + 1);
}

/*
 * FULLY BLOCKING until all messages moved out of the uart TX buffer. Theoretically this is quick but who knows if
 * something goes wrong
 */
void log_wait_until_all_tx() {
    uart_wait_tx_done(cli_uart_handle->port, portMAX_DELAY);
}

void log_set_max_log_level(log_level_t level) {
    // I don't think we really have to worry about this since it's mostly for re-entrant log calls overwriting the
    // buffer but why not
    BaseType_t rval = xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(50));
    assert(rval == pdTRUE);

    max_log_level = level;

    xSemaphoreGive(mutex_handle);
}

/*
 * Hide single tag from appearing in log. Turns bit on because logic is inverted, 1s are blacklisted in bitmask
 */
void log_hide_tag(sc_tag_t tag) {
    tag_blacklist |= (1 << tag);
}

/*
 * Shows single tag in log if previously hidden. Turns bit off because logic is inverted, 0s are allowed in bitmask
 */
void log_show_tag(sc_tag_t tag) {
    tag_blacklist &= ~(1 << tag);
}

void log_show_all_tags() {
    tag_blacklist = 0x00000000;
}

void log_hide_all_tags() {
    tag_blacklist = UINT32_MAX;
}

/*
 * For CLI debugging only
 */
uint32_t log_get_tag_blacklist() {
    return tag_blacklist;
}

/*
 * Gets time string for log prefix. Doesn't matter if SNTP synced yet, RTC will return time since boot if not.
 */
char *log_get_time_str() {
    struct tm now_local = {0};
    sntp_time_get_local_time(&now_local);
    sntp_time_get_time_str(&now_local, time_str_buffer, NULL);

    return time_str_buffer;
}
