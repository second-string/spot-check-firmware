#ifndef LED_TEXT_H
#define LED_TEXT_H

#include "esp_err.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*

  Scrolling logic contained in led_text.c borrowed and apapted from Allen C. Huffman's Library here:
  https://github.com/allenhuffman/LEDSign
  Simple LED Scrolling Message
  By Allen C. Huffman (alsplace@pobox.com)
  www.subethasoftware.com

*/

/* Set to true to enable printing the led pattern serially each scroll increment */
#define DEBUG_LOG_LED_TEXT false

typedef enum { ZIGZAG, STRAIGHT } row_orientation;

typedef enum { LEFT, RIGHT } col_direction;

typedef enum {
    IDLE,      // We have nothing on the strips. Most commonly once we've finished scrolling
    STATIC,    // The last action we took was writing static, non-scrolling text
    SCROLLING  // Actively scrolling text right now
} led_text_state;

typedef struct {
    esp_err_t (*set_pixel)(int pixel_index, uint32_t rgb_value);
    esp_err_t (*show)(void);
} led_strip_funcs;

extern led_text_state led_text_current_state;

void led_text_init(const unsigned char *font,
                   int                  rows,
                   int                  num_per_row,
                   row_orientation      row_direction,
                   led_strip_funcs      strip_funcs);

void led_text_show_text(char *text, size_t text_len);

void led_text_scroll_text_blocking(char *text, size_t text_len);

void led_text_scroll_text_async(char *text, size_t text_len, bool scroll_continously);

void led_text_stop_scroll();

#endif
