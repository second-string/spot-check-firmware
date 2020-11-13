#ifndef LED_STRIP_H
#define LED_STRIP_H

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define NUM_LEDS CONFIG_WS2812_NUM_LEDS

/* Separate struct declaration from typedef to enable us to use this typedef as an arg for func ptrs in struct */
typedef struct led_strip_s led_strip_t;

/*
 * Wrapping function declarations in a struct allows each implementing file to create their own implementation
 * and struct to hold the info they need while allowing them to implement each function and assign those function
 * ptrs to these opaque values
 */
struct led_strip_s {
    /* Set the RGB value of an individual pixel in the strip where the rgb_val param is a uint32_t holding
    the R, G, and B values in that order in the lowest 24 bytes (the upper byte is discarded). For example:
    R = 0xFF, B = 0xAA, G = 0x55
    =
    rgb_val = 0x00FFAA55
    */
    esp_err_t (*set_pixel)(led_strip_t *strip, int pixel_index, uint32_t rgb_val);

    /* Must be called to flush and re-apply every pixel change from set_pixel to the strip */
    esp_err_t (*show)(led_strip_t *strip);

    /* Sets every pixel to 0,0,0 for RBG (aka turns it off) and automatically applies that to the strip */
    esp_err_t (*clear)(led_strip_t *strip);
};

/*
 * CURRENTLY ONLY SUPPORTS BEING INITED ONCE.
 * This component does not dynamically allocated any memory, so there is a single instance of the
 * underlying struct that holds all of the data. This could be improved but is complicated to do without
 * heap allocation, so for now make sure this is only ever called once.
 */
led_strip_t *led_strip_init_ws2812();

#endif
