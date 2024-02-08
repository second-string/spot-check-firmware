#pragma once

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
#define SCREEN_IMG_WIND_CHART_SIZE_NVS_KEY "wind_img_sz"
#define SCREEN_IMG_WIND_CHART_WIDTH_PX_NVS_KEY "wind_img_w"
#define SCREEN_IMG_WIND_CHART_HEIGHT_PX_NVS_KEY "wind_img_h"
#define SCREEN_IMG_CUSTOM_SCREEN_SIZE_NVS_KEY "cstm_img_sz"
#define SCREEN_IMG_CUSTOM_SCREEN_WIDTH_PX_NVS_KEY "cstm_img_w"
#define SCREEN_IMG_CUSTOM_SCREEN_HEIGHT_PX_NVS_KEY "cstm_img_h"

/*
 * Start byte of each image saved inthe screen_img partition. Screen partition as of now is 512kb large. If each chart
 * is 700x200 px with 2-pixels-ber-byte, each takes up 70kb (0x11170). But when erasing with page boundaries, fw will
 * erase up to 0x12000. Could be dynamically calced but this is simpler for now.
 *
 * NOTE: These MUST by 4k aligned for proper erasing
 *
 * TODO :: this could save a lot of space just by having a 'chart 1 offset'/'chart 2 offset' and just placing each chart
 * there instead of in its own individual place in the partition. This isn't scalable, but passes for now until more
 * charts might be added.
 */
#define SCREEN_IMG_TIDE_CHART_OFFSET 0x0
#define SCREEN_IMG_SWELL_CHART_OFFSET 0x12000
#define SCREEN_IMG_WIND_CHART_OFFSET 0x24000

// Allow the fullscreen custom screen image to occupy the same space as they'll never be used together
#define SCREEN_IMG_CUSTOM_SCREEN_OFFSET 0x0

// Name of the NVS partition that the screen data bytes are saved. Generic since it holds multiple images
#define SCREEN_IMG_PARTITION_LABEL "screen_img"

typedef enum {
    SCREEN_IMG_TIDE_CHART,
    SCREEN_IMG_SWELL_CHART,
    SCREEN_IMG_WIND_CHART,
    SCREEN_IMG_CUSTOM_SCREEN,

    SCREEN_IMG_COUNT,
} screen_img_t;

void screen_img_handler_init();
bool screen_img_handler_download_and_save(screen_img_t screen_img);

bool screen_img_handler_clear_screen_img(screen_img_t screen_img);
bool screen_img_handler_clear_chart(screen_img_t screen_img);
bool screen_img_handler_draw_screen_img(screen_img_t screen_img);
bool screen_img_handler_draw_chart(screen_img_t screen_img);
