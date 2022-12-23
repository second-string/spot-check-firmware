#pragma once

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

// Don't need to error check TAG existence because it will error on compile if calling file hasn't defined it
#define log_printf(_level_, _fmt_, ...) log_log_line(TAG, _level_, _fmt_, ##__VA_ARGS__)
