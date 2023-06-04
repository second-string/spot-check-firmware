#pragma once

#include "esp_log.h"

#include "constants.h"
#include "uart.h"

typedef enum {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
} log_level_t;

void     log_init(uart_handle_t *console_handle);
void     log_log_line(sc_tag_t tag, log_level_t level, char *fmt, ...);
void     log_wait_until_all_tx();
void     log_set_max_log_level(log_level_t level);
void     log_hide_tag(sc_tag_t tag);
void     log_show_tag(sc_tag_t tag);
void     log_show_all_tags();
void     log_hide_all_tags();
uint32_t log_get_tag_blacklist();
char    *log_get_time_str();

// Uses esp idf ansi color code macros
#define LOG_LEVEL_DEBUG_COLOR LOG_COLOR(LOG_COLOR_BLACK)
#define LOG_LEVEL_INFO_COLOR LOG_COLOR(LOG_COLOR_BLACK)
#define LOG_LEVEL_WARN_COLOR LOG_COLOR(LOG_COLOR_BROWN)
#define LOG_LEVEL_ERROR_COLOR LOG_COLOR(LOG_COLOR_RED)

#define LOG_LEVEL_ERROR_PREFIX "[ERR]"
#define LOG_LEVEL_WARN_PREFIX "[WRN]"
#define LOG_LEVEL_INFO_PREFIX "[INF]"
#define LOG_LEVEL_DEBUG_PREFIX "[DBG]"

// Builds colored log line with preprocessor to avoid manual runtime str manipulation. Hardcoded %s here will be filled
// by the tag_strs[TAG] arg in the main log macro so the var args always have at least the tag passed as an arg.
// Should not be called externally.
// example literal output: \033[0;31m%s [ERR] example log from app %s %d %u\033[0;30m\n
#define BUILD_LOG_LINE(_level_, _caller_format_) \
    "[%s] " _level_##_COLOR "%s " _level_##_PREFIX " " _caller_format_ LOG_RESET_COLOR "\n"

// Main log macro - this is the only thing that should be called externally
// Don't need to error check TAG existence because it will error on compile if calling file hasn't defined it. Must pass
// the tag string as arg before optional var args as hardcoded %s in built log str will blow up in vsnprintf if not
// included
#define log_printf(_level_, _fmt_, ...) \
    log_log_line(TAG, _level_, BUILD_LOG_LINE(_level_, _fmt_), log_get_time_str(), tag_strs[TAG], ##__VA_ARGS__)
