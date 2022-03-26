#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "FreeRTOS_CLI.h"
#include "cli_task.h"
#include "constants.h"
#include "uart.h"

#include "log.h"

#define TAG "sc-cli"

// Lower number, lower priority (idle == 0)
#define CLI_TASK_PRIORITY (tskIDLE_PRIORITY)
#define CLI_COMMAND_BUFFER_BYTES (128)

#define CLI_CMD_PROCESS_TASK_PRIORITY (tskIDLE_PRIORITY)
#define CLI_COMMAND_QUEUE_SIZE (12)

typedef struct {
    char  *cmd;
    size_t len;
} cli_command_t;

static uart_handle_t *handle;
static char          *command_buffer;
static uint8_t        command_char_idx;

static QueueHandle_t queue_handle;
static StaticQueue_t queue_buffer;
static uint8_t      *queue_data_buffer;

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

    // TODO :: handle backspace
    // Echo
    uart_write_bytes(handle->port, &c, 1);

    if (c == '\n' || c == '\r') {
        command_buffer[command_char_idx] = '\0';

        // char  *base = "processing cmd:";
        // char   full[strlen(base) + strlen(command_buffer) + 1];
        // size_t full_size = sprintf(full, "%s %s", base, command_buffer);

        uart_write_bytes(handle->port, "\r\n", 2);

        // Either free on failure here or dequeuer responsible for freeing
        char *cmd_copy = pvPortMalloc(command_char_idx * sizeof(char));
        memcpy(cmd_copy, command_buffer, command_char_idx);
        cli_command_t cmd = {
            .cmd = cmd_copy,
            .len = command_char_idx * sizeof(char),
        };

        // Queue will copy cmd struct values to internal queue items, no need to malloc it
        BaseType_t rval = xQueueSendToBack(queue_handle, &cmd, pdMS_TO_TICKS(10));
        if (!rval) {
            vPortFree(cmd_copy);
        }

        command_char_idx = 0;
    }
}

/*
 * Task to pop commands off the queue and execute them. Seperates long or blocking command handlers from serial rx
 */
static void cli_process_command(void *args) {
    QueueHandle_t queue = args;

    BaseType_t    rval = pdFALSE;
    cli_command_t cmd;
    while (1) {
        rval = xQueueReceive(queue, &cmd, portMAX_DELAY);
        assert(rval);
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

    queue_data_buffer = pvPortMalloc(CLI_COMMAND_QUEUE_SIZE * sizeof(cli_command_t));
    assert(queue_data_buffer);
    queue_handle = xQueueCreateStatic(CLI_COMMAND_QUEUE_SIZE, sizeof(cli_command_t), queue_data_buffer, &queue_buffer);
    assert(queue_handle);
}

void cli_task_start() {
    BaseType_t rval = xTaskCreate(uart_generic_rx_task,
                                  "CLI UART RX",
                                  SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 2,
                                  handle,
                                  CLI_TASK_PRIORITY,
                                  NULL);
    assert(rval);

    rval = xTaskCreate(cli_process_command,
                       "CLI cmd process",
                       SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 2,
                       queue_handle,
                       CLI_CMD_PROCESS_TASK_PRIORITY,
                       NULL);
}
