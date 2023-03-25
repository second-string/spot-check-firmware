#pragma once

#include "uart.h"

UBaseType_t cli_task_get_stack_high_water();
void        cli_task_init(uart_handle_t *uart_handle);
void        cli_task_start();
