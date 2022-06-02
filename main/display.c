#include "freertos/FreeRTOS.h"

#include "display.h"
#include "epd_driver.h"
#include "epd_highlevel.h"
#include "firasans_20.h"
#include "log.h"
#include "timer.h"

#define TAG "sc-display"

#define FONT FiraSans_20
#define ED060SC4_WIDTH_PX 800
#define ED060SC4_HEIGHT_PX 600

static EpdiyHighlevelState hl;
static uint32_t            display_height;
static uint32_t            display_width;

static void display_render() {
    epd_poweron();
    enum EpdDrawError err = epd_hl_update_screen(&hl, MODE_GC16, 25);
    (void)err;
    // TODO :: error check
    // TODO :: do we need this delay?
    vTaskDelay(pdMS_TO_TICKS(100));
    epd_poweroff();
}

void display_init() {
    epd_init(EPD_LUT_1K);
    hl             = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    display_width  = epd_rotated_display_width();
    display_height = epd_rotated_display_height();
    log_printf(TAG, LOG_LEVEL_INFO, "Display dimensions,  width: %dpx height: %dpx", display_width, display_height);
}

void display_start() {
    configASSERT(hl.front_fb && hl.back_fb);
    display_full_clear();
}

void display_full_clear() {
    epd_poweron();
    epd_fullclear(&hl, 25);
    // TODO :: delay needed?
    vTaskDelay(pdMS_TO_TICKS(100));
    epd_poweroff();
    log_printf(TAG, LOG_LEVEL_DEBUG, "Cleared full display");
}

void display_render_splash_screen() {
    configASSERT(hl.front_fb && hl.back_fb);

    uint8_t *fb = epd_hl_get_framebuffer(&hl);

    int cursor_x = 300;
    int cursor_y = 250;
    epd_write_default(&FONT, "Spot Check", &cursor_x, &cursor_y, fb);
    cursor_x = 350;
    cursor_y = 300;
    epd_write_default(&FONT, "rev. 3.1", &cursor_x, &cursor_y, fb);
    cursor_x = 200;
    cursor_y = epd_rotated_display_height() - 50;
    epd_write_default(&FONT, "Second String Studios", &cursor_x, &cursor_y, fb);

    log_printf(TAG, LOG_LEVEL_DEBUG, "Rendering splash screen on display");
    display_render();
}

void display_render_text(char *text) {
    display_full_clear();

    uint8_t *fb       = epd_hl_get_framebuffer(&hl);
    int      cursor_x = 300;
    int      cursor_y = display_height / 2 - 20;
    epd_write_default(&FONT, text, &cursor_x, &cursor_y, fb);

    log_printf(TAG, LOG_LEVEL_DEBUG, "Rendering text on display: '%s'", text);
    display_render();
}

/*
 * Display a decoded JPEG on the e-ink display. Takes a pointer to data buffer (flash or ram), height and width of image
 * in px, bytes per pixel, and starting x,y on screen. Bytes per pix will be 1 if rendering black and white, otherwise
 * will be 3 or 4 for RGB and RBGA data.
 */
void display_render_image(uint8_t *image_buffer,
                          size_t   width_px,
                          size_t   height_px,
                          uint8_t  bytes_per_px,
                          uint32_t screen_x,
                          uint32_t screen_y) {
    display_full_clear();
    uint8_t *fb = epd_hl_get_framebuffer(&hl);

    const uint32_t row_width = width_px * bytes_per_px;
    const uint32_t height    = height_px;
    for (uint32_t i = 0; i < height - 1; i++) {
        for (uint32_t j = 0, x_px = 0; j < row_width - 1; j += bytes_per_px) {
            // For now assume any non-0xFF is black
            if (j > 0) {
                x_px = j / bytes_per_px;
            }

            epd_draw_pixel(screen_x + x_px, screen_y + i, image_buffer[i * row_width + j] == 0xFF ? 0xFF : 0x00, fb);
        }
    }

    display_render();
}

/*
 * Assumes array holds enough data for full screen, cannot check bounds and will crash if not. See display_render_image
 * for internals.
 */
void display_render_image_fullscreen(uint8_t *image_buffer, uint8_t bytes_per_px) {
    display_render_image(image_buffer, ED060SC4_WIDTH_PX, ED060SC4_HEIGHT_PX, bytes_per_px, 0, 0);
}
