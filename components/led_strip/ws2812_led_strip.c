#include <string.h>

#include "driver/rmt.h"
#include "led_strip.h"

#define LED_RMT_TX_CHANNEL CONFIG_WS2812_LED_RMT_TX_CHANNEL
#define LED_RMT_TX_GPIO CONFIG_WS2812_LED_RMT_TX_GPIO

/* ws2812 high and low pulse times for a 0 or 1 bit */
#define WS2812_T0H_NS (350)
#define WS2812_T1H_NS (1000)
#define WS2812_T0L_NS (1000)
#define WS2812_T1L_NS (350)

/* ws2812-specific implementation of our led_strip behavior.
 * led_data array is NUM_LEDS * 3 because each led needs 24 bits,
 * 8 for R, G, and B values. This can be optimized in the future
 * to just a bit array I think if we only want 1 color.
 */
typedef struct {
    led_strip_t   interface;
    rmt_channel_t rmt_channel;
    uint32_t      num_leds;
    uint8_t       led_data[NUM_LEDS * 3];
} ws2812_t;

static ws2812_t strip_handle;
static uint32_t ws2812_t0h_ticks = 0;
static uint32_t ws2812_t1h_ticks = 0;
static uint32_t ws2812_t0l_ticks = 0;
static uint32_t ws2812_t1l_ticks = 0;

/*
 * When we write our led_data buffer to RMT, we do it holding RGB data. This function handles the
 * conversion from each single readable RGB value to the 'dest' rmt_item32_t value
 * This function may be called from an ISR, so, the code should be short and efficient.
 *
 * src: Pointer to src buffer of R, G, or B uint8_ts that we want to convert to RMT
 * dest[out]: Pointer to the output buffer holding each source bit's RMT counterpart (that means there are 8 additions
 * to this buffer for a single src uint8_t element) src_size: The number of uint8_t src elements to iterate through
 * wanted_num: The number of rmt_items expected to be added to the dest buffer
 * translated_size[out]: The number of src uint8_t elements that have been converted to rmt_itmes (0 if none converted).
 *           This number should exactly equal item_num / 8, since there are 8 rmt_items produced for a single src
 * element item_num[out]: The number of rmt_items that were appended to the dest buffer. This should exactly equal
 *           translated_size * 8, since there are 8 rmt_items per single src element
 */
static void IRAM_ATTR ws2812_rmt_translator(const void *src, rmt_item32_t *dest, size_t src_size, size_t wanted_num,
                                            size_t *translated_size, size_t *item_num) {
    if (src == NULL || dest == NULL) {
        *translated_size = 0;
        *item_num        = 0;
        return;
    }

    // local const because we need the post-compile-computed value for tick counts
    const rmt_item32_t rmt_low  = {{{ws2812_t0h_ticks, 1, ws2812_t0l_ticks, 0}}};
    const rmt_item32_t rmt_high = {{{ws2812_t1h_ticks, 1, ws2812_t1l_ticks, 0}}};

    // Increments for every every full uint8_t src value translated into 8 rmt_items
    size_t num_src_items_copied = 0;
    // Increments for every new rmt_item copied from a single src bit
    size_t        num_rmt_items_copied = 0;
    uint8_t *     src_item             = (uint8_t *)src;
    rmt_item32_t *dest_item            = dest;
    while (num_src_items_copied < src_size && num_rmt_items_copied < wanted_num) {
        for (int i = 0; i < 8; i++) {
            // For each bit (starting at MSB), convert from a binary 1/0
            // to the equivalent rmt_item32_t and set the destination pointer's
            // value to the full 32 bit value of that rmt_item
            if (*src_item & 1 << (7 - i)) {
                dest_item->val = rmt_high.val;
            } else {
                dest_item->val = rmt_low.val;
            }

            num_rmt_items_copied++;
            dest_item++;
        }

        num_src_items_copied++;
        src_item++;
    }

    *translated_size = num_src_items_copied;
    *item_num        = num_rmt_items_copied;
}

static esp_err_t ws2812_set_pixel(int pixel_index, uint32_t rgb_val) {
    if (pixel_index > (strip_handle.num_leds - 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    // ws2812 strips are actually GRB so pull the middle byte, then the first, and finally the B last
    uint32_t strip_offset                   = pixel_index * 3;
    strip_handle.led_data[strip_offset]     = (rgb_val >> 8) & 0xFF;
    strip_handle.led_data[strip_offset + 1] = (rgb_val >> 16) & 0xFF;
    strip_handle.led_data[strip_offset + 2] = rgb_val & 0xFF;

    return ESP_OK;
}

static esp_err_t ws2812_show() {
    // Routes data through our translator function and blocks until full buffer is sent before continuing
    esp_err_t err = rmt_write_sample(strip_handle.rmt_channel, strip_handle.led_data, strip_handle.num_leds * 3, true);
    if (err != ESP_OK) {
        return err;
    }

    // I think this is completely irrelevant since we're already blocking above with no timeout
    return rmt_wait_tx_done(strip_handle.rmt_channel, pdMS_TO_TICKS(100));
}

static esp_err_t ws2812_clear(led_strip_t *strip) {
    memset(strip_handle.led_data, 0, strip_handle.num_leds * 3);
    return ws2812_show(&strip_handle.interface);
}

led_strip_t *led_strip_init_ws2812() {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(LED_RMT_TX_GPIO, LED_RMT_TX_CHANNEL);
    config.clk_div      = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    uint32_t rmt_clock_hz = 0;
    ESP_ERROR_CHECK(rmt_get_counter_clock(LED_RMT_TX_CHANNEL, &rmt_clock_hz));

    // Convert our nanosecond macros to ticks, which the rmt_item32_t items need
    float ratio      = (float)rmt_clock_hz / 1e9;
    ws2812_t0h_ticks = (ratio * WS2812_T0H_NS);
    ws2812_t1h_ticks = (ratio * WS2812_T1H_NS);
    ws2812_t0l_ticks = (ratio * WS2812_T0L_NS);
    ws2812_t1l_ticks = (ratio * WS2812_T1L_NS);

    rmt_translator_init(LED_RMT_TX_CHANNEL, ws2812_rmt_translator);

    strip_handle.rmt_channel         = LED_RMT_TX_CHANNEL;
    strip_handle.num_leds            = NUM_LEDS;
    strip_handle.interface.set_pixel = ws2812_set_pixel;
    strip_handle.interface.clear     = ws2812_clear;
    strip_handle.interface.show      = ws2812_show;
    return &strip_handle.interface;
}
