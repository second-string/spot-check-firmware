#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "cli_task.h"
#include "constants.h"
#include "uart.h"

#include "log.h"

#define TAG "sc-cli"

// Lower number, lower priority (idle == 0)
#define CLI_TASK_PRIORITY (tskIDLE_PRIORITY)
#define CLI_COMMAND_BUFFER_BYTES (128)

static uart_handle_t *handle;
static char          *command_buffer;
static uint8_t        command_char_idx;

// TaskHandle_t cli_task_handle;

// static void cli_task_loop() {
//     // RX already set up by sdk for logging, setup tx and install IRQs for both

//     while (1) {
//         const char *test = "test write out\n";
//         uart_write_bytes(uart_port, test, strlen(test));
//         vTaskDelay(pdMS_TO_TICKS(4000));
//     }
// }

static void cli_process_char(char c) {
    command_buffer[command_char_idx++] = c;
    if (c == '\n' || c == '\r') {
        command_buffer[command_char_idx++] = '\0';

        char  *base = "processing cmd:";
        char   full[strlen(base) + strlen(command_buffer) + 1];
        size_t full_size = sprintf(full, "%s %s", base, command_buffer);
        uart_write_bytes(handle->port, full, full_size);

        command_char_idx = 0;
    }
}

void cli_task_init(uart_handle_t *uart_handle) {
    assert(uart_handle);
    handle = uart_handle;

    // Weird situation where this uart is already fully inited since we need to do it first thing to log out init
    // sequence. For other uart users, they'd probably init the entire uart_handle_t with uart_init themselves.
    // Instead we just assign the callback here and drop any received chars that happen in init sequence
    handle->process_char = cli_process_char;

    command_buffer = pvPortMalloc(CLI_COMMAND_BUFFER_BYTES * sizeof(char));
    assert(command_buffer);
    command_char_idx = 0;
}

void cli_task_start() {
    BaseType_t rval = xTaskCreate(uart_generic_rx_task,
                                  "CLI",
                                  SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 2,
                                  handle,
                                  CLI_TASK_PRIORITY,
                                  NULL);
    assert(rval);
}
