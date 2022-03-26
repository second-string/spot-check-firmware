#pragma once

#include "uart.h"

void cli_task_init(uart_handle_t *uart_handle);
void cli_task_start();
