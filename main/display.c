#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "memfault/panics/assert.h"

#include "constants.h"
#include "display.h"
#include "epd_driver.h"
#include "epd_highlevel.h"
#include "firasans_10.h"
#include "firasans_15.h"
#include "firasans_20.h"
#include "firasans_40.h"
#include "flash_partition.h"
#include "log.h"

#define TAG SC_TAG_DISPLAY

#define FONT_40 FiraSans_40
#define FONT_20 FiraSans_20
#define FONT_15 FiraSans_15
#define FONT_10 FiraSans_10

#define ED060SC4_WIDTH_PX 800
#define ED060SC4_HEIGHT_PX 600

static EpdiyHighlevelState hl;
static uint32_t            display_height;
static uint32_t            display_width;
static SemaphoreHandle_t   render_lock;

static enum EpdFontFlags display_get_epd_font_flags_enum(display_font_align_t alignment) {
    MEMFAULT_ASSERT(alignment < DISPLAY_FONT_ALIGN_COUNT);

    switch (alignment) {
        case DISPLAY_FONT_ALIGN_LEFT:
            return EPD_DRAW_ALIGN_LEFT;
        case DISPLAY_FONT_ALIGN_CENTER:
            return EPD_DRAW_ALIGN_CENTER;
        case DISPLAY_FONT_ALIGN_RIGHT:
            return EPD_DRAW_ALIGN_RIGHT;
        default:
            log_printf(LOG_LEVEL_ERROR, "Invalid font alignment! Defaulting to left aligned.");
            return EPD_DRAW_ALIGN_LEFT;
    }
}

static const EpdFont *display_get_epd_font_enum(display_font_size_t size) {
    MEMFAULT_ASSERT(size < DISPLAY_FONT_SIZE_COUNT);

    switch (size) {
        case DISPLAY_FONT_SIZE_SMALL:
            return &FONT_10;
        case DISPLAY_FONT_SIZE_SHMEDIUM:
            return &FONT_15;
        case DISPLAY_FONT_SIZE_MEDIUM:
            return &FONT_20;
        case DISPLAY_FONT_SIZE_LARGE:
            return &FONT_40;
        default:
            log_printf(LOG_LEVEL_ERROR, "Invalid font size! Defaulting to medium.");
            return &FONT_20;
    }
}

static char *display_get_epd_font_flags_enum_string(display_font_align_t alignment) {
    MEMFAULT_ASSERT(alignment < DISPLAY_FONT_ALIGN_COUNT);

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
    MEMFAULT_ASSERT(size < DISPLAY_FONT_SIZE_COUNT);

    switch (size) {
        case DISPLAY_FONT_SIZE_SMALL:
            return "small";
        case DISPLAY_FONT_SIZE_SHMEDIUM:
            return "shmedium";
        case DISPLAY_FONT_SIZE_MEDIUM:
            return "medium";
        case DISPLAY_FONT_SIZE_LARGE:
            return "large";
        default:
            return "invalid";
    }
}

static bool render_acquire_lock(const char *calling_func, uint32_t line) {
    log_printf(LOG_LEVEL_DEBUG, "trying to acquire lock from %s:%d", calling_func, line);
    BaseType_t success = xSemaphoreTake(render_lock, pdMS_TO_TICKS(500));
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "Couldn't acquire render lock even after 500ms!");
    }

    return success;
}

static void render_release_lock() {
    xSemaphoreGive(render_lock);
    log_printf(LOG_LEVEL_DEBUG, "released lock");
}

static void display_render_mode(enum EpdDrawMode mode, const char *calling_func, uint32_t line) {
    if (!render_acquire_lock(__func__, __LINE__)) {
        return;
    }

    epd_poweron();
    vTaskDelay(pdMS_TO_TICKS(20));
    enum EpdDrawError err = epd_hl_update_screen(&hl, mode, 25);
    (void)err;
    // TODO :: error check
    epd_poweroff();

    render_release_lock();
}

void display_init() {
    epd_init(EPD_LUT_1K);
    hl          = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    uint8_t *fb = epd_hl_get_framebuffer(&hl);
    memset(fb, 0x00, EPD_WIDTH / 2 * EPD_HEIGHT);

    render_lock = xSemaphoreCreateMutex();

    display_width  = epd_rotated_display_width();
    display_height = epd_rotated_display_height();
    log_printf(LOG_LEVEL_DEBUG, "Display dimensions,  width: %dpx height: %dpx", display_width, display_height);
}

void display_start() {
    MEMFAULT_ASSERT(hl.front_fb && hl.back_fb);

    //  We need at least 3 because there is no memory of what is displayed on screen from last run, so the diffing
    //  internal to epdiy won't run to help the  clearing process.
    display_full_clear_cycles(3);
}

void display_render(const char *calling_func, uint32_t line) {
    display_render_mode(MODE_GC16, calling_func, line);
}

void display_full_clear_cycles(uint8_t cycles) {
    if (!render_acquire_lock(__func__, __LINE__)) {
        return;
    }

    epd_poweron();
    epd_hl_set_all_white(&hl);
    enum EpdDrawError err = epd_hl_update_screen(&hl, MODE_GC16, 25);
    (void)err;
    vTaskDelay(pdMS_TO_TICKS(20));
    epd_clear_area_cycles(epd_full_screen(), cycles, 12);
    epd_poweroff();

    render_release_lock();
}

/*
 * Default full clear function just does single flash for simplicity. epdiy 'standard' fullclear uses 3
 */
void display_full_clear() {
    display_full_clear_cycles(3);
}

void display_clear_area(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!render_acquire_lock(__func__, __LINE__)) {
        return;
    }

    // Limit these bounds to be w/in the framebuffer, epdiy will happily buffer overflow it
    MEMFAULT_ASSERT(x + width <= ED060SC4_WIDTH_PX);
    MEMFAULT_ASSERT(y + height <= ED060SC4_HEIGHT_PX);

    EpdRect rect = {
        .x      = x,
        .y      = y,
        .width  = width,
        .height = height,
    };

    // Fill framebuffer with white to ovewrite any drawn data in epd_hl_update_area
    uint8_t *fb = epd_hl_get_framebuffer(&hl);
    epd_fill_rect(rect, 0xFF, fb);

    // Add in 1-pixel padding to erase area to make sure a gray outline isn't left from bleedover
    if (rect.x > 0) {
        rect.x -= 1;
    }
    if (rect.y > 0) {
        rect.y -= 1;
    }
    if (rect.x + width < ED060SC4_WIDTH_PX) {
        rect.width += 2;
    }
    if (rect.y + height < ED060SC4_HEIGHT_PX) {
        rect.height += 2;
    }

    epd_poweron();
    epd_hl_update_area(&hl, MODE_GC16, 18, rect);
    vTaskDelay(pdMS_TO_TICKS(40));
    epd_clear_area_cycles(rect, 1, 12);
    vTaskDelay(pdMS_TO_TICKS(40));
    epd_poweroff();

    render_release_lock();

    log_printf(LOG_LEVEL_DEBUG, "Cleared %uw %uh rect at (%u, %u)", width, height, x, y);
}

void display_render_splash_screen(char *fw_version, char *hw_version) {
    MEMFAULT_ASSERT(hl.front_fb && hl.back_fb);

    const uint8_t fw_version_str_len   = 50;
    const uint8_t hw_version_str_len   = 50;
    const uint8_t full_version_str_len = fw_version_str_len + hw_version_str_len + 10;
    char          fw_version_str[fw_version_str_len];
    char          hw_version_str[hw_version_str_len];
    char          full_version_str[full_version_str_len];
    sprintf(fw_version_str, "FW: %s", fw_version);
    sprintf(hw_version_str, "HW: %s", hw_version);

    MEMFAULT_ASSERT(strlen(fw_version_str) < fw_version_str_len);
    MEMFAULT_ASSERT(strlen(hw_version_str) < hw_version_str_len);
    MEMFAULT_ASSERT(strlen(fw_version_str) + strlen(hw_version_str) < full_version_str_len);
    sprintf(full_version_str, "%s    %s", hw_version_str, fw_version_str);

    display_draw_text("Spot Check", ED060SC4_WIDTH_PX / 2, 300, DISPLAY_FONT_SIZE_MEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text("Second String Studios",
                      ED060SC4_WIDTH_PX / 2,
                      epd_rotated_display_height() - 60,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text(full_version_str,
                      ED060SC4_WIDTH_PX / 2,
                      epd_rotated_display_height() - 30,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    log_printf(LOG_LEVEL_DEBUG, "Rendering splash screen on display");
    display_render(__func__, __LINE__);
}

void display_draw_text(char                *text,
                       uint32_t             x_coord,
                       uint32_t             y_coord,
                       display_font_size_t  size,
                       display_font_align_t alignment) {
    MEMFAULT_ASSERT(x_coord < ED060SC4_WIDTH_PX);
    MEMFAULT_ASSERT(y_coord < ED060SC4_HEIGHT_PX);

    int x = x_coord;
    int y = y_coord;

    EpdFontProperties font_props = epd_font_properties_default();
    font_props.flags             = display_get_epd_font_flags_enum(alignment);
    const EpdFont *font          = display_get_epd_font_enum(size);
    uint8_t       *fb            = epd_hl_get_framebuffer(&hl);

    log_printf(LOG_LEVEL_DEBUG,
               "Rendering %s, %s-aligned text at (%u, %u): '%s'",
               display_get_epd_font_enum_string(size),
               display_get_epd_font_flags_enum_string(alignment),
               x,
               y,
               text);

    epd_write_string(font, text, &x, &y, fb, &font_props);
}

void display_invert_text(char                *text,
                         uint32_t             x_coord,
                         uint32_t             y_coord,
                         display_font_size_t  size,
                         display_font_align_t alignment) {
    MEMFAULT_ASSERT(x_coord < ED060SC4_WIDTH_PX);
    MEMFAULT_ASSERT(y_coord < ED060SC4_HEIGHT_PX);

    int x = x_coord;
    int y = y_coord;

    EpdFontProperties font_props = epd_font_properties_default();
    font_props.flags             = display_get_epd_font_flags_enum(alignment);
    font_props.fg_color          = 0xF;
    const EpdFont *font          = display_get_epd_font_enum(size);
    uint8_t       *fb            = epd_hl_get_framebuffer(&hl);

    log_printf(LOG_LEVEL_DEBUG,
               "Inverting %s, %s-aligned text at (%u, %u): '%s'",
               display_get_epd_font_enum_string(size),
               display_get_epd_font_flags_enum_string(alignment),
               x,
               y,
               text);

    epd_write_string(font, text, &x, &y, fb, &font_props);
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
    // Limit these bounds to be w/in the framebuffer, epdiy will happily buffer overflow it
    MEMFAULT_ASSERT(screen_x + width_px <= ED060SC4_WIDTH_PX);
    MEMFAULT_ASSERT(screen_y + height_px <= ED060SC4_HEIGHT_PX);

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

void display_draw_rect(uint32_t x, uint32_t y, uint32_t width_px, uint32_t height_px) {
    // Limit these bounds to be w/in the framebuffer, epdiy will happily buffer overflow it
    MEMFAULT_ASSERT(x + width_px <= ED060SC4_WIDTH_PX);
    MEMFAULT_ASSERT(y + height_px <= ED060SC4_HEIGHT_PX);

    EpdRect rect = {
        .x      = x,
        .y      = y,
        .width  = width_px,
        .height = height_px,
    };

    uint8_t *fb = epd_hl_get_framebuffer(&hl);
    epd_fill_rect(rect, 0x0, fb);

    log_printf(LOG_LEVEL_DEBUG, "Rendering %uw %uh rect at (%u, %u)", width_px, height_px, x, y);
}

/*
 * Assumes array holds enough data for full screen, cannot check bounds and will crash if not. See display_render_image
 * for internals.
 */
void display_draw_image_fullscreen(uint8_t *image_buffer, uint8_t bytes_per_px) {
    display_draw_image(image_buffer, ED060SC4_WIDTH_PX, ED060SC4_HEIGHT_PX, bytes_per_px, 0, 0);
}

void display_get_text_bounds(char                *text,
                             uint32_t             x,
                             uint32_t             y,
                             display_font_size_t  size,
                             display_font_align_t alignment,
                             uint32_t            *width,
                             uint32_t            *height) {
    EpdFontProperties font_props = {
        .flags = display_get_epd_font_flags_enum(alignment),
    };

    const EpdFont *font = display_get_epd_font_enum(size);
    int            x1   = 0;
    int            y1   = 0;
    epd_get_text_bounds(font, text, (int *)&x, (int *)&y, &x1, &y1, (int *)width, (int *)height, &font_props);
    log_printf(LOG_LEVEL_DEBUG,
               "BOUNDS for '%s': x: %d, y: %d, x1: %d, y1: %d, width: %d, height: %d",
               text,
               x,
               y,
               x1,
               y1,
               *width,
               *height);
}

void display_mark_all_lines_dirty() {
    for (int i = 0; i < (EPD_WIDTH / 2 * EPD_HEIGHT); i++) {
        hl.back_fb[i] = ~hl.front_fb[i];
    }
}

void display_mark_rect_dirty(uint32_t x_coord, uint32_t y_coord, uint32_t width, uint32_t height) {
    uint32_t row_start_index   = 0;
    uint32_t dirty_rect_height = MIN(height, EPD_HEIGHT - y_coord);
    uint32_t dirty_rect_width  = MIN(width, EPD_WIDTH - x_coord);

    for (int i = y_coord; i < (y_coord + dirty_rect_height); i++) {
        row_start_index = i * (EPD_WIDTH / 2);

        for (int j = (x_coord / 2); j < ((x_coord + dirty_rect_width) / 2); j++) {
            hl.back_fb[row_start_index + j] = ~hl.front_fb[row_start_index + j];
        }
    }
}
