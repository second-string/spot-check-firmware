#ifndef LED_TEXT_H
#define LED_TEXT_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    ZIGZAG,
    STRAIGHT
} row_orientation;

void led_text_init(const unsigned char *font, int rows, int num_per_row, row_orientation row_direction);

void led_text_scroll_text_blocking(char *text, size_t text_len);

void led_text_scroll_text_async(char *text, size_t text_len, bool scroll_continously);

void led_text_stop_scroll();

#endif
