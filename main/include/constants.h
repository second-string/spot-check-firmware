#ifndef CONSTANTS_H
#define CONSTANTS_H

// #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

// esp freertos port configMINIMAL_STACK_SIZE is 768 base plus optionals compiled in
// (esp-idf/components/freertos/esp_additions/include/freertos/FreeRTOSConfg.h).
// Define our own minimum here (esp freertos port takes stack size in BYTES not WORDS like vanilla, so stupid)
#define SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES (1024)

#define MS_PER_SECOND (1000)
#define SECONDS_PER_MIN (60)

// Keys in NVS for the current number of bytes saved in the screen_img partition as a single image.
// Saving the key separately in the NVS KVS avoids having to use a packed header prefix in the image data partition for
// metadata
// NOTE : max key length is 15 bytes (null term does not count for a byte)
#define SCREEN_IMG_SIZE_NVS_KEY "screen_img_size"
#define SCREEN_IMG_WIDTH_PX_NVS_KEY "screen_img_w"
#define SCREEN_IMG_HEIGHT_PX_NVS_KEY "screen_img_h"

// Name of the NVS partition that the screen data bytes are saved
#define SCREEN_IMG_PARTITION_LABEL "screen_img"

#endif
