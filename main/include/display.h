#pragma once

#include "constants.h"

void display_init();
void display_start();
void display_full_clear();
void display_render_splash_screen();
void display_render_text(char *text);
void display_render_image(uint8_t *image_buffer,
                          size_t   width_px,
                          size_t   height_px,
                          uint8_t  bytes_per_px,
                          uint32_t screen_x,
                          uint32_t screen_y);
void display_render_image_fullscreen(uint8_t *image_buffer, uint8_t bytes_per_px);
