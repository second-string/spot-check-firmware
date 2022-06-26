#include "freertos/FreeRTOS.h"

#include "display.h"
#include "epd_driver.h"
#include "epd_highlevel.h"
#include "firasans_10.h"
#include "firasans_20.h"
#include "firasans_40.h"
#include "flash_partition.h"
#include "log.h"
#include "timer.h"

#define TAG "sc-display"

#define FONT_40 FiraSans_40
#define FONT_20 FiraSans_20
#define FONT_10 FiraSans_10

#define ED060SC4_WIDTH_PX 800
#define ED060SC4_HEIGHT_PX 600

static EpdiyHighlevelState hl;
static uint32_t            display_height;
static uint32_t            display_width;

static enum EpdFontFlags display_get_epd_font_flags_enum(display_font_align_t alignment) {
    configASSERT(alignment < DISPLAY_FONT_ALIGN_COUNT);

    switch (alignment) {
        case DISPLAY_FONT_ALIGN_LEFT:
            return EPD_DRAW_ALIGN_LEFT;
        case DISPLAY_FONT_ALIGN_CENTER:
            return EPD_DRAW_ALIGN_CENTER;
        case DISPLAY_FONT_ALIGN_RIGHT:
            return EPD_DRAW_ALIGN_RIGHT;
        default:
            log_printf(TAG, LOG_LEVEL_ERROR, "Invalid font alignment! Defaulting to left aligned.");
            return EPD_DRAW_ALIGN_LEFT;
    }
}

static const EpdFont *display_get_epd_font_enum(display_font_size_t size) {
    configASSERT(size < DISPLAY_FONT_SIZE_COUNT);

    switch (size) {
        case DISPLAY_FONT_SIZE_SMALL:
            return &FONT_10;
        case DISPLAY_FONT_SIZE_MEDIUM:
            return &FONT_20;
        case DISPLAY_FONT_SIZE_LARGE:
            return &FONT_40;
        default:
            log_printf(TAG, LOG_LEVEL_ERROR, "Invalid font size! Defaulting to medium.");
            return &FONT_20;
    }
}

static char *display_get_epd_font_flags_enum_string(display_font_align_t alignment) {
    configASSERT(alignment < DISPLAY_FONT_ALIGN_COUNT);

    switch (alignment) {
        case DISPLAY_FONT_ALIGN_LEFT:
            return "left";
        case DISPLAY_FONT_ALIGN_CENTER:
            return "center";
        case DISPLAY_FONT_ALIGN_RIGHT:
            return "right";
        default:
            return "invalid";
    }
}

static const char *display_get_epd_font_enum_string(display_font_size_t size) {
    configASSERT(size < DISPLAY_FONT_SIZE_COUNT);

    switch (size) {
        case DISPLAY_FONT_SIZE_SMALL:
            return "small";
        case DISPLAY_FONT_SIZE_MEDIUM:
            return "medium";
        case DISPLAY_FONT_SIZE_LARGE:
            return "large";
        default:
            return "invalid";
    }
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

void display_render() {
    epd_poweron();
    enum EpdDrawError err = epd_hl_update_screen(&hl, MODE_GL16, 25);
    (void)err;
    // TODO :: error check
    // TODO :: do we need this delay?
    vTaskDelay(pdMS_TO_TICKS(100));
    epd_poweroff();
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

    display_draw_text("Spot Check", ED060SC4_WIDTH_PX / 2, 250, DISPLAY_FONT_SIZE_MEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text("rev. 3.1", ED060SC4_WIDTH_PX / 2, 300, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text("Second String Studios",
                      ED060SC4_WIDTH_PX / 2,
                      epd_rotated_display_height() - 50,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    log_printf(TAG, LOG_LEVEL_DEBUG, "Rendering splash screen on display");
    display_render();
}

void display_draw_text(char                *text,
                       uint32_t             x_coord,
                       uint32_t             y_coord,
                       display_font_size_t  size,
                       display_font_align_t alignment) {
    configASSERT(x_coord < ED060SC4_WIDTH_PX);
    configASSERT(y_coord < ED060SC4_HEIGHT_PX);

    EpdFontProperties font_props = {
        .flags = display_get_epd_font_flags_enum(alignment),
    };
    const EpdFont *font = display_get_epd_font_enum(size);
    uint8_t       *fb   = epd_hl_get_framebuffer(&hl);
    epd_write_string(font, text, (int32_t *)&x_coord, (int32_t *)&y_coord, fb, &font_props);

    log_printf(TAG,
               LOG_LEVEL_DEBUG,
               "Rendering %s, %s-aligned text on display: '%s'",
               display_get_epd_font_enum_string(size),
               display_get_epd_font_flags_enum_string(alignment),
               text);
}

/*
 * Display a decoded JPEG on the e-ink display. Takes a pointer to data buffer (flash or ram), height and width of image
 * in px, bytes per pixel, and starting x,y on screen. Bytes per pix will be 1 if rendering black and white, otherwise
 * will be 3 or 4 for RGB and RBGA data.
 */
void display_draw_image(uint8_t *image_buffer,
                        size_t   width_px,
                        size_t   height_px,
                        uint8_t  bytes_per_px,
                        uint32_t screen_x,
                        uint32_t screen_y) {
    uint8_t *fb   = epd_hl_get_framebuffer(&hl);
    EpdRect  rect = {
         .x      = screen_x,
         .y      = screen_y,
         .width  = width_px,
         .height = height_px,
    };

    // Data MUST be 2 pixels per byte, aka 1 pixel per 4-bit nibble.
    epd_copy_to_framebuffer(rect, image_buffer, fb);
}

/*
 * Assumes array holds enough data for full screen, cannot check bounds and will crash if not. See display_render_image
 * for internals.
 */
void display_draw_image_fullscreen(uint8_t *image_buffer, uint8_t bytes_per_px) {
    display_draw_image(image_buffer, ED060SC4_WIDTH_PX, ED060SC4_HEIGHT_PX, bytes_per_px, 0, 0);
}
