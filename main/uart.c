#include <string.h>

#include "freertos/FreeRTOS.h"

#include "log.h"
#include "uart.h"

#define TAG "sc-uart"

void uart_init(uart_port_t       port,
               uint16_t          rx_ring_buffer_size,
               uint16_t          tx_ring_buffer_size,
               uint8_t           event_queue_size,
               uint16_t          rx_buffer_size,
               process_char_func process_char_cb,
               uart_handle_t    *handle) {
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
    ESP_ERROR_CHECK(uart_driver_install(handle->port,
                                        rx_ring_buffer_size,
                                        tx_ring_buffer_size,
                                        event_queue_size,
                                        &handle->queue,
                                        0));

    handle->rx_buffer = pvPortMalloc(sizeof(char) * rx_buffer_size);
    assert(handle->rx_buffer);

    handle->process_char = process_char_cb;
}

void uart_generic_rx_task(void *args) {
    uart_handle_t *handle = args;
    assert(handle);

    while (1) {
        // Underlying impl uses esp-freertos xRingBufferReceive which I'm pretty sure used a freertos primitive below
        // it, so this portMAX_DELAY should do our regular yield like we want instead of spinning
        const int bytes_read = uart_read_bytes(handle->port, handle->rx_buffer, 1, portMAX_DELAY);
        if (bytes_read) {
            if (handle->process_char) {
                handle->process_char(handle->rx_buffer[0]);
            } else {
                char  *base     = "uart_generic_rx_task RX byte, no process_byte handler:";
                size_t base_len = strlen(base);
                char   full[base_len + 3];
                sprintf(full, "%s %c", base, handle->rx_buffer[0]);
                uart_write_bytes(handle->port, full, base_len + 2);
            }
        } else {
            log_printf(LOG_LEVEL_ERROR,
                       "uart_read_bytes returned with no bytes read, but we should yield forever until bytes exist");
        }
    }
}
