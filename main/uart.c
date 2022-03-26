#include "uart.h"

void uart_init(uart_port_t    port,
               uint16_t       rx_buffer_size,
               uint16_t       tx_buffer_size,
               uint8_t        event_queue_size,
               uart_handle_t *handle) {
    handle->config.baud_rate = 115200;
    handle->config.data_bits = UART_DATA_8_BITS;
    handle->config.parity    = UART_PARITY_DISABLE;
    handle->config.stop_bits = UART_STOP_BITS_1;
    handle->config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    ESP_ERROR_CHECK(uart_param_config(port, &handle->config));

    // Probably unnecessary but ¯\_(ツ)_/¯
    ESP_ERROR_CHECK(
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Don't know what the INTR flags last arg is for
    handle->port = port;
    ESP_ERROR_CHECK(
        uart_driver_install(handle->port, rx_buffer_size, tx_buffer_size, event_queue_size, &handle->queue, 0));
}

void uart_start() {
}
