#pragma once

#include "constants.h"

typedef enum {
    DISPLAY_FONT_ALIGN_LEFT,
    DISPLAY_FONT_ALIGN_CENTER,
    DISPLAY_FONT_ALIGN_RIGHT,

    DISPLAY_FONT_ALIGN_COUNT
} display_font_align_t;

typedef enum {
    DISPLAY_FONT_SIZE_SMALL,
    DISPLAY_FONT_SIZE_SHMEDIUM,
    DISPLAY_FONT_SIZE_MEDIUM,
    DISPLAY_FONT_SIZE_LARGE,

    DISPLAY_FONT_SIZE_COUNT,
} display_font_size_t;

void display_init();
void display_start();
void display_render();
void display_full_clear();
void display_clear_area(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void display_render_splash_screen();
void display_draw_text(char                *text,
                       uint32_t             x_coord,
                       uint32_t             y_coord,
                       display_font_size_t  size,
                       display_font_align_t alignment);
void display_draw_image(uint8_t *image_buffer,
                        size_t   width_px,
                        size_t   height_px,
                        uint8_t  bytes_per_px,
                        uint32_t screen_x,
                        uint32_t screen_y);
void display_draw_rect(uint32_t x, uint32_t y, uint32_t width_px, uint32_t height_px);
void display_draw_image_fullscreen(uint8_t *image_buffer, uint8_t bytes_per_px);
void display_get_text_bounds(char                *text,
                             uint32_t             x,
                             uint32_t             y,
                             display_font_size_t  size,
                             display_font_align_t alignment,
                             uint32_t            *width,
                             uint32_t            *height);
