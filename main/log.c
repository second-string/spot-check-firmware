#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/uart.h"

#include "log.h"

// Should match CLI_UART_TX_BYTES probably
#define LOG_OUT_BUFFER_BYTES (512)

static uart_handle_t    *cli_uart_handle;
static char             *log_out_buffer;
static SemaphoreHandle_t mutex_handle;
static StaticSemaphore_t mutex_buffer;
static log_level_t       max_log_level;

static const char *level_strs[] = {
    [LOG_LEVEL_ERROR] = "[ERR]",
    [LOG_LEVEL_WARN]  = "[WRN]",
    [LOG_LEVEL_INFO]  = "[INF]",
    [LOG_LEVEL_DEBUG] = "[DBG]",
};

void log_init(uart_handle_t *cli_handle) {
    assert(cli_handle);

    log_out_buffer = pvPortMalloc(LOG_OUT_BUFFER_BYTES * sizeof(uint8_t));
    assert(log_out_buffer);

    mutex_handle = xSemaphoreCreateMutexStatic(&mutex_buffer);
    assert(mutex_handle);

    cli_uart_handle = cli_handle;
    max_log_level   = LOG_LEVEL_DEBUG;
}

void log_log_line(char *tag, log_level_t level, char *fmt, ...) {
    // Drop log line entirely if max level set less verbose than line verbosity
    if (level > max_log_level) {
        return;
    }

    BaseType_t rval = xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(50));
    // TODO :: could handle this more elegantly
    assert(rval == pdTRUE);

    char *moving_log_out_buffer = log_out_buffer;
    char  temp_tag[20];
    sprintf(temp_tag, "[%s]", tag);
    size_t bytes_written = 0;
    bytes_written += sprintf(moving_log_out_buffer, "%s ", temp_tag);
    moving_log_out_buffer += bytes_written;

    const char *level_str      = level_strs[level];
    size_t      level_str_size = strlen(level_str);
    memcpy(moving_log_out_buffer, level_str, strlen(level_str));
    bytes_written += level_str_size;
    moving_log_out_buffer += level_str_size;

    *moving_log_out_buffer = ' ';
    bytes_written++;
    moving_log_out_buffer++;

    va_list args;
    va_start(args, fmt);
    // Leave 1 char for the \n on the end (don't care about null term since we're sending to uart with byte count)
    size_t formatted_size = vsnprintf(moving_log_out_buffer, LOG_OUT_BUFFER_BYTES - bytes_written - 1, fmt, args);
    va_end(args);

    log_out_buffer[bytes_written + formatted_size] = '\n';
    xSemaphoreGive(mutex_handle);

    // Let go of the mutex before sending to serial, it'll handle re-entrancy with it's own internal queueing
    // (maybe?)
    uart_write_bytes(cli_uart_handle->port, log_out_buffer, bytes_written + formatted_size + 1);
}

void log_set_max_log_level(log_level_t level) {
    // I don't think we really have to worry about this since it's mostly for re-entrant log calls overwriting the
    // buffer but why not
    BaseType_t rval = xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(50));
    assert(rval == pdTRUE);

    max_log_level = level;

    xSemaphoreGive(mutex_handle);
}
