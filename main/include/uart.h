#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "driver/uart.h"

// Positively SWIMMING in ram
#define CLI_UART_RX_RING_BUFFER_BYTES (1024)
#define CLI_UART_TX_RING_BUFFER_BYTES (1024)
#define CLI_UART_RX_BUFFER_BYTES (1024)
#define CLI_UART_QUEUE_SIZE (10)

typedef void (*process_char_func)(char c);

typedef struct {
    uart_port_t       port;
    uart_config_t     config;
    QueueHandle_t     queue;
    char             *rx_buffer;
    process_char_func process_char;

} uart_handle_t;

// Build out handle properties
void uart_init(uart_port_t       port,
               uint16_t          rx_ring_buffer_size,
               uint16_t          tx_ring_buffer_size,
               uint8_t           event_queue_size,
               uint16_t          rx_buffer_size,
               process_char_func process_char_cb,
               uart_handle_t    *handle);
void uart_generic_rx_task(void *args);
