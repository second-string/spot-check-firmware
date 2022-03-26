#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "cli_task.h"
#include "constants.h"

#include "esp_log.h"

#define TAG "sc-cli"

// Lower number, lower priority (idle == 0)
#define CLI_TASK_PRIORITY (tskIDLE_PRIORITY)

static uart_port_t uart_port;

TaskHandle_t cli_task_handle;

static void cli_task_loop() {
    // RX already set up by sdk for logging, setup tx and install IRQs for both

    while (1) {
        const char *test = "test write out\n";
        uart_write_bytes(uart_port, test, strlen(test));
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void cli_task_init(uart_port_t uart) {
    uart_port = uart;
}

void cli_task_start() {
    BaseType_t rval = xTaskCreate(cli_task_loop,
                                  "CLI",
                                  SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 2,
                                  NULL,
                                  CLI_TASK_PRIORITY,
                                  &cli_task_handle);
    (void)rval;
    // assert(rval);
}
