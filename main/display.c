#include "freertos/FreeRTOS.h"

#include "display.h"
#include "epd_driver.h"
#include "epd_highlevel.h"
#include "firasans_20.h"
#include "log.h"
#include "timer.h"

#define TAG "sc-display"

#define FONT FiraSans_20

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
