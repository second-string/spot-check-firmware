#pragma once

#include "conditions_task.h"

// Keys in NVS for the current number of bytes saved in the screen_img partition as a single image.
// Saving the key separately in the NVS KVS avoids having to use a packed header prefix in the image data partition for
// metadata
// NOTE : max key length is 15 bytes (null term does not count for a byte)
#define SCREEN_IMG_TIDE_CHART_SIZE_NVS_KEY "tide_img_sz"
#define SCREEN_IMG_TIDE_CHART_WIDTH_PX_NVS_KEY "tide_img_w"
#define SCREEN_IMG_TIDE_CHART_HEIGHT_PX_NVS_KEY "tide_img_h"
#define SCREEN_IMG_SWELL_CHART_SIZE_NVS_KEY "swell_img_sz"
#define SCREEN_IMG_SWELL_CHART_WIDTH_PX_NVS_KEY "swell_img_w"
#define SCREEN_IMG_SWELL_CHART_HEIGHT_PX_NVS_KEY "swell_img_h"

// Start byte of each image saved inthe screen_img partition. Assumes the partition is 512K, which puts the second image
// at the halfway point. If each image is 700x200 px, each takes up 140kb, so we've got a fair amount of
// overcompensation. Could be dynamically calced but this is simpler for now.
#define SCREEN_IMG_TIDE_CHART_OFFSET 0x0
#define SCREEN_IMG_SWELL_CHART_OFFSET 0x40000

// Name of the NVS partition that the screen data bytes are saved. Generic since it holds multiple images
#define SCREEN_IMG_PARTITION_LABEL "screen_img"

typedef enum {
    SCREEN_IMG_TIDE_CHART,
    SCREEN_IMG_SWELL_CHART,

    SCREEN_IMG_COUNT,
} screen_img_t;

bool screen_img_handler_download_and_save(screen_img_t screen_img);
bool screen_img_handler_draw_screen_img(screen_img_t screen_img);
void screen_img_handler_clear_time();
bool screen_img_handler_draw_time();
void screen_img_handler_clear_conditions(bool clear_spot_name,
                                         bool clear_temperature,
                                         bool clear_wind,
                                         bool clear_tide);
bool screen_img_handler_draw_conditions(char *spot_name, conditions_t *conditions);
bool screen_img_handler_draw_conditions_error();
void screen_img_handler_render();
