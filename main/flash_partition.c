#include "memfault/panics/assert.h"

#include "constants.h"
#include "flash_partition.h"
#include "screen_img_handler.h"

#define TAG SC_TAG_PART

const esp_partition_t *flash_partition_get_screen_img_partition() {
    const esp_partition_t *screen_img_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SCREEN_IMG_PARTITION_LABEL);
    MEMFAULT_ASSERT(screen_img_partition);
    return screen_img_partition;
}
