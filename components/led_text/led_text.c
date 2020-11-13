#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_text.h"
#include "fonts.h"

#define TAG "led-text"

typedef struct {
    char *text;
    size_t text_len;
    bool scroll_continously;
} scroll_text_args;

static const unsigned char *font_ptr;
static uint8_t width_of_letter;
static uint8_t height_of_letter;
static uint8_t font_start_char;

static int led_rows;
static int leds_per_row;
static row_orientation orientation;

static TaskHandle_t scroll_text_task_handle;
static scroll_text_args args;

void led_text_init(const unsigned char *font, int rows, int num_per_row, row_orientation row_direction) {
    font_ptr = font;
    width_of_letter = font_ptr[0];
    height_of_letter = font_ptr[1];
    font_start_char = font_ptr[2];

    led_rows = rows;
    leds_per_row = num_per_row;
    orientation = row_direction;

    ESP_LOGI(TAG, "Font width: %d", width_of_letter);
    ESP_LOGI(TAG, "Font height: %d", height_of_letter);
    ESP_LOGI(TAG, "First ASCII value in array: %d", font_start_char);
    ESP_LOGI(TAG, "# led strip rows: %d", led_rows);
    ESP_LOGI(TAG, "# LEDs per row: %d", leds_per_row);
    ESP_LOGI(TAG, "Row orientation: %s", orientation == ZIGZAG ? "ZIGZAG" : orientation == STRAIGHT ? "STRAIGHT" : "undefined");
}

static void led_text_scroll_text(void *args) {
    scroll_text_args *casted_args = (scroll_text_args *)args;
    char *text = casted_args->text;
    size_t text_len = casted_args->text_len;
    bool scroll_continously = casted_args->scroll_continously;

    int first_letter;
    int text_inner_offset;
    int current_led_row;
    int current_letter_offset;
    int current_led_column;
    uint8_t font_bit;
    char text_letter;

    while (1) {
        // Each time this increments, we've scrolled a full font letter width and were shifting
        // one letter fully off the matrix
        for (first_letter = 0; first_letter < text_len; first_letter++) {
            // The full iteration of this loop scrolls the full text message one text_letter. Each iteration
            // moves the current first character in the message one pixel closer to being fully scrolled off
            for (text_inner_offset = 0; text_inner_offset < width_of_letter; text_inner_offset++) {
                // Start of the inner loops responsible for one complete static display of our text
                for (current_led_row = 0; current_led_row < led_rows && current_led_row < height_of_letter; current_led_row++) {
                    current_letter_offset = 0;
                    font_bit = text_inner_offset;
                    for (current_led_column = 0; current_led_column < leds_per_row; current_led_column++) {
                        if ((first_letter + current_letter_offset) >= text_len) {
                            // If we've printed the whole message, print spaces from now on
                            text_letter = ' ';
                        } else {
                            text_letter = text[first_letter + current_letter_offset];
                        }

                        // Get single byte of font data on the current row for the specific char we're trying to display
                        // ESP_LOGI(TAG, "Indexing into font data: [%d + (%d - %d) * %d + %d] (total: %d)",
                        //     FONT_DATA_OFFSET, text_letter, font_start_char, height_of_letter, current_led_row,
                        //     FONT_DATA_OFFSET + (text_letter - font_start_char) * height_of_letter + current_led_row);
                        char font_data = font_ptr[FONT_DATA_OFFSET + (text_letter - font_start_char) * height_of_letter + current_led_row];

                        // Reverse-index to get the correct bit value since 0 for our font_bit corresponds to the furthest-left
                        // bit in the font byte data, but that's technically the MSB for what we read out of the font array
                        uint8_t is_font_bit_set = font_data & (1 << (7 - font_bit));
                        if (is_font_bit_set) {
                            printf("X");
                        } else {
                            printf(" ");
                        }

                        // Move to the next letter in our text once we've set all the bits for
                        // this letter on this led row
                        font_bit++;
                        if (font_bit >= width_of_letter) {
                            font_bit = 0;
                            current_letter_offset++;
                        }
                    }

                    printf("\n");
                }

                int separators;
                for (separators = 0; separators < leds_per_row; separators++) {
                    printf("-");
                }
                printf("\n");

                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }

        if (!scroll_continously) {
            scroll_text_task_handle = NULL;
            vTaskDelete(NULL);
        }
    }
}

void led_text_scroll_text_blocking(char *text, size_t text_len) {
    args = (scroll_text_args) {
        .text = text,
        .text_len = text_len,
        .scroll_continously = false
    };

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

    args = (scroll_text_args) {
        .text = text,
        .text_len = text_len,
        .scroll_continously = scroll_continously
    };

    xTaskCreate(&led_text_scroll_text, "scroll-text", 4096 / 4, &args, tskIDLE_PRIORITY, &scroll_text_task_handle);
}

void led_text_stop_scroll() {
    vTaskDelete(scroll_text_task_handle);
    scroll_text_task_handle = NULL;
}
