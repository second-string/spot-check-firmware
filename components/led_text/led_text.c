#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_text.h"

#define TAG "led-text"

// Only log if we have our debug flag set in the header
#if DEBUG_LOG_LED_TEXT
#define LED_TEXT_LOG(x) printf(x)
#else
#define LED_TEXT_LOG(x)
#endif

typedef struct {
    char  *text;
    size_t text_len;
    bool   scroll_continously;
} scroll_text_args;

static const unsigned char *font_ptr;
static uint8_t              width_of_letter;
static uint8_t              height_of_letter;
static uint8_t              font_start_char;

static int             led_rows;
static int             leds_per_row;
static row_orientation orientation;
static col_direction   direction;

static TaskHandle_t     scroll_text_task_handle;
static scroll_text_args args;

static led_strip_funcs strip_funcs;

led_text_state led_text_current_state;

void led_text_init(const unsigned char *font,
                   int                  rows,
                   int                  num_per_row,
                   row_orientation      row_direction,
                   led_strip_funcs      funcs) {
    font_ptr         = font;
    width_of_letter  = font_ptr[0];
    height_of_letter = font_ptr[1];
    font_start_char  = font_ptr[2];

    led_rows     = rows;
    leds_per_row = num_per_row;
    orientation  = row_direction;
    direction    = RIGHT;

    strip_funcs            = funcs;
    led_text_current_state = IDLE;

    ESP_LOGI(TAG, "Font width: %d", width_of_letter);
    ESP_LOGI(TAG, "Font height: %d", height_of_letter);
    ESP_LOGI(TAG, "First ASCII value in array: %d", font_start_char);
    ESP_LOGI(TAG, "# led strip rows: %d", led_rows);
    ESP_LOGI(TAG, "# LEDs per row: %d", leds_per_row);
    ESP_LOGI(TAG,
             "Row orientation: %s",
             orientation == ZIGZAG     ? "ZIGZAG"
             : orientation == STRAIGHT ? "STRAIGHT"
                                       : "undefined");
}

/*
 * Performs logic for iterating over every accessible LED and setting the pixel on or off depending on the text data.
 * strip.show() must still be called to flush data through RMT
 *
 * text: the char array of text that we're trying to display on the LEDs
 * text_len: the number of characters in the 'text' arg
 * first_letter_idx: the index of the letter we should start at in the 'text' arg. This will be 0 every time for
 * displaying static text text_inner_offset: the number of pixels inside a letter the text should be shifted over by
 * before display. Again, 0 for static text
 */
static void led_text_set_static_text(char *text, size_t text_len, int first_letter_idx, int text_inner_offset) {
    int     current_led_row;
    int     current_letter_offset;
    int     current_led_column;
    int     current_row_column_offset;
    uint8_t font_bit;
    char    text_letter;

    // Start of the inner loops responsible for one complete static display of our text
    for (current_led_row = 0; current_led_row < led_rows && current_led_row < height_of_letter; current_led_row++) {
        current_letter_offset = 0;
        font_bit              = text_inner_offset;
        for (current_led_column = 0; current_led_column < leds_per_row; current_led_column++) {
            // If we're zigzagging rows, we need to adjust our column offset to be from the right
            // or the left as we move down the columns of this row
            if (direction == RIGHT) {
                current_row_column_offset = current_led_column;
            } else {
                current_row_column_offset = leds_per_row - 1 - current_led_column;
            }

            if ((first_letter_idx + current_letter_offset) >= text_len) {
                // If we've printed the whole message, print spaces from now on
                text_letter = ' ';
            } else {
                text_letter = text[first_letter_idx + current_letter_offset];
            }

            // Get single byte of font data on the current row for the specific char we're trying to display
            // ESP_LOGI(TAG, "Indexing into font data: [%d + (%d - %d) * %d + %d] (total: %d)",
            //     FONT_DATA_OFFSET, text_letter, font_start_char, height_of_letter, current_led_row,
            //     FONT_DATA_OFFSET + (text_letter - font_start_char) * height_of_letter + current_led_row);
            char font_data =
                font_ptr[FONT_DATA_OFFSET + (text_letter - font_start_char) * height_of_letter + current_led_row];

            // Reverse-index to get the correct bit value since 0 for our font_bit corresponds to the furthest-left
            // bit in the font byte data, but that's technically the MSB for what we read out of the font array
            uint8_t is_font_bit_set = font_data & (1 << (7 - font_bit));
            int     pixel_idx       = current_led_row * leds_per_row + current_row_column_offset;
            if (is_font_bit_set) {
                LED_TEXT_LOG("X");
                ESP_ERROR_CHECK(strip_funcs.set_pixel(pixel_idx, 0x3F0000));
            } else {
                LED_TEXT_LOG(" ");
                ESP_ERROR_CHECK(strip_funcs.set_pixel(pixel_idx, 0x000000));
            }

            // Move to the next letter in our text once we've set all the bits for
            // this letter on this led row
            font_bit++;
            if (font_bit >= width_of_letter) {
                font_bit = 0;
                current_letter_offset++;
            }
        }

        // if we're zig zagging rows, switch the direction we're laying out pixels
        // for the next row
        if (orientation == ZIGZAG) {
            if (direction == LEFT) {
                direction = RIGHT;
            } else {
                direction = LEFT;
            }
        }
        LED_TEXT_LOG("\n");
    }
}

static void led_text_scroll_text(void *args) {
    led_text_current_state = SCROLLING;

    scroll_text_args *casted_args        = (scroll_text_args *)args;
    char             *text               = casted_args->text;
    size_t            text_len           = casted_args->text_len;
    bool              scroll_continously = casted_args->scroll_continously;

    int first_letter;
    int text_inner_offset;

    while (1) {
        // Each time this increments, we've scrolled a full font letter width and were shifting
        // one letter fully off the matrix
        for (first_letter = 0; first_letter < text_len; first_letter++) {
            // The full iteration of this loop scrolls the full text message one text_letter. Each iteration
            // moves the current first character in the message one pixel closer to being fully scrolled off
            for (text_inner_offset = 0; text_inner_offset < width_of_letter; text_inner_offset++) {
                led_text_set_static_text(text, text_len, first_letter, text_inner_offset);
                strip_funcs.show();

                int separators;
                for (separators = 0; separators < leds_per_row; separators++) {
                    LED_TEXT_LOG("-");
                }
                LED_TEXT_LOG("\n");

                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }

        if (!scroll_continously) {
            led_text_current_state  = IDLE;
            scroll_text_task_handle = NULL;
            vTaskDelete(NULL);
        }
    }
}

void led_text_show_text(char *text, size_t text_len) {
    led_text_set_static_text(text, text_len, 0, 0);
    led_text_current_state = STATIC;

    strip_funcs.show();
}

void led_text_scroll_text_blocking(char *text, size_t text_len) {
    args = (scroll_text_args){.text = text, .text_len = text_len, .scroll_continously = false};

    return led_text_scroll_text(&args);
}

void led_text_scroll_text_async(char *text, size_t text_len, bool scroll_continously) {
    if (scroll_text_task_handle != NULL) {
        eTaskState state = eTaskGetState(scroll_text_task_handle);
        switch (state) {
            case eReady:
                ESP_LOGI(TAG, "CALLED ASYNC TEXT SCROLL AND GOT READY EVEN W/ NON NULL HANDLE");
                break;
            case eRunning:
                ESP_LOGI(TAG, "Don't call led_text_scroll_text_async from within the same task that's running it");
                return;
            case eBlocked:
                ESP_LOGI(TAG, "led_text_scroll_text_async already running, call led_text_stop_scroll_text to stop it");
                return;
            default:
                ESP_LOGI(TAG, "Got unsupported switch case when checking scroll text task state: %d", state);
                break;
        }
    }

    args = (scroll_text_args){.text = text, .text_len = text_len, .scroll_continously = scroll_continously};

    xTaskCreate(&led_text_scroll_text, "scroll-text", 4096 / 4, &args, tskIDLE_PRIORITY, &scroll_text_task_handle);
}

void led_text_stop_scroll() {
    // TODO :: I think clear all the leds otherwise they'll be frozen on last write
    if (scroll_text_task_handle) {
        vTaskDelete(scroll_text_task_handle);
    }

    led_text_current_state  = IDLE;
    scroll_text_task_handle = NULL;
}
