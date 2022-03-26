#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "driver/uart.h"

// Positively SWIMMING in ram
#define CLI_UART_RX_BUFFER_BYTES (1024)
#define CLI_UART_TX_BUFFER_BYTES (1024)
#define CLI_UART_QUEUE_SIZE (10)

typedef struct {
    uart_config_t config;
    QueueHandle_t queue;
} uart_handle_t;

// Build out handle properties
void uart_init(uart_port_t    port,
               uint16_t       rx_buffer_size,
               uint16_t       tx_buffer_size,
               uint8_t        event_queue_size,
               uart_handle_t *handle);
void uart_start();
