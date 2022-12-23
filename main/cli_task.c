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

#define TAG SC_TAG_CLI

// Lower number, lower priority (idle == 0)
#define CLI_TASK_PRIORITY (tskIDLE_PRIORITY)
#define CLI_COMMAND_BUFFER_BYTES (128)

#define CLI_CMD_PROCESS_TASK_PRIORITY (tskIDLE_PRIORITY)
#define CLI_COMMAND_QUEUE_SIZE (12)
#define CLI_COMMAND_PROCESS_OUT_BUFFER_BYTES (256)

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
static char         *command_processing_out;

static void cli_process_char(char c) {
    // TODO :: handle backspace
    if (c == 0x08) {
        command_buffer[--command_char_idx] = 0x00;
        // Echo out a backspace to move the cursor back, a space to cover up the old char, then another backspace to
        // remove the space. Don't put any of it in our buffer because we dgaf about those shenanigans to make it look
        // good to the user.
        uart_write_bytes(handle->port, "\x08\x20\x08", 3);
    } else {
        // Add to buffer and echo back
        command_buffer[command_char_idx++] = c;
        uart_write_bytes(handle->port, &c, 1);
    }

    if (c == '\n' || c == '\r') {
        // Overwrite newline with null term FreeRTOS+CLI to be able to process it
        // This is assuming that we're only getting a single CR or LF - if we get two this will break command parsing
        command_char_idx--;
        command_buffer[command_char_idx] = 0x00;

        uart_write_bytes(handle->port, "\r\n", 2);

        // Either free on failure here or dequeuer responsible for freeing
        char *cmd_copy = pvPortMalloc((command_char_idx + 1) * sizeof(char));
        memcpy(cmd_copy, command_buffer, command_char_idx + 1);
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

    BaseType_t    rval      = pdFALSE;
    BaseType_t    more_data = pdFALSE;
    cli_command_t cmd;
    while (1) {
        rval = xQueueReceive(queue, &cmd, portMAX_DELAY);
        assert(rval);

        do {
            more_data =
                FreeRTOS_CLIProcessCommand(cmd.cmd, command_processing_out, CLI_COMMAND_PROCESS_OUT_BUFFER_BYTES);
            log_printf(LOG_LEVEL_INFO, "%s", command_processing_out);
        } while (more_data);

        // Free the string malloced by uart rx task
        // vPortFree(cmd.cmd);
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
    command_processing_out = pvPortMalloc(CLI_COMMAND_PROCESS_OUT_BUFFER_BYTES * sizeof(char));
    assert(command_processing_out);
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
                       SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 4,
                       queue_handle,
                       CLI_CMD_PROCESS_TASK_PRIORITY,
                       NULL);
}
