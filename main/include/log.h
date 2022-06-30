#pragma once

#include "uart.h"

typedef enum {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
} log_level_t;

void log_init(uart_handle_t *console_handle);
void log_log_line(char *tag, log_level_t level, char *fmt, ...);
void log_set_max_log_level(log_level_t level);

#define log_printf(_tag_, _level_, _fmt_, ...) log_log_line(_tag_, _level_, _fmt_, ##__VA_ARGS__)
