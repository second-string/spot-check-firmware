#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "cli_task.h"

#include "esp_log.h"

#define TAG "sc-cli"

// Lower number, lower priority (idle == 0)
#define CLI_TASK_PRIORITY (1)

static uart_port_t uart;

TaskHandle_t cli_task_handle;

static void cli_task_loop() {
    while (1) {
        ESP_LOGI(TAG, "cli loop");
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void cli_task_init(uart_port_t uart_arg) {
    uart = uart_arg;
}

void cli_task_start() {
    BaseType_t rval =
        xTaskCreate(cli_task_loop, "CLI", configMINIMAL_STACK_SIZE * 2, NULL, CLI_TASK_PRIORITY, &cli_task_handle);
    (void)rval;
    // assert(rval);
}
