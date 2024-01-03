#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"

#include "esp_partition.h"
#include "esp_sntp.h"
#include "memfault/panics/assert.h"
#include "spi_flash_mmap.h"

#include "constants.h"
#include "display.h"
#include "flash_partition.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "nvs.h"
#include "screen_img_handler.h"
#include "sntp_time.h"

#define TAG SC_TAG_SCREEN_IMG_HANDLER

typedef struct {
    screen_img_t screen_img;
    uint32_t     x_coord;
    uint32_t     y_coord;
    char        *screen_img_size_key;
    char        *screen_img_width_key;
    char        *screen_img_height_key;
    uint32_t     screen_img_offset;
    uint32_t     screen_img_size;
    uint32_t     screen_img_width;
    uint32_t     screen_img_height;
    char        *endpoint;
} screen_img_metadata_t;

static void screen_img_handler_get_metadata(screen_img_t screen_img, screen_img_metadata_t *metadata) {
    switch (screen_img) {
        case SCREEN_IMG_TIDE_CHART:
            metadata->x_coord               = 50;
            metadata->y_coord               = 200;
            metadata->screen_img_size_key   = SCREEN_IMG_TIDE_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_TIDE_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_TIDE_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_TIDE_CHART_OFFSET;
            metadata->endpoint              = "tides_chart";
            break;
        case SCREEN_IMG_SWELL_CHART:
            metadata->x_coord               = 50;
            metadata->y_coord               = 400;
            metadata->screen_img_size_key   = SCREEN_IMG_SWELL_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_SWELL_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_SWELL_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_SWELL_CHART_OFFSET;
            metadata->endpoint              = "swell_chart";
            break;
        default:
            MEMFAULT_ASSERT(0);
    }

    bool success = nvs_get_uint32(metadata->screen_img_size_key, &metadata->screen_img_size);
    if (!success) {
        log_printf(LOG_LEVEL_WARN, "No screen img size value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_width_key, &metadata->screen_img_width);
    if (!success) {
        log_printf(LOG_LEVEL_WARN, "No screen img width value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_height_key, &metadata->screen_img_height);
    if (!success) {
        log_printf(LOG_LEVEL_WARN, "No screen img height value stored in NVS, setting to zero");
    }
}

/*
 * Finished process of saving a screen_img to the proper location in the flash partition. Request must have been built
 * and sent with http_client_build_request and http_client_perform_with_retries already.
 */
static int screen_img_handler_save(esp_http_client_handle_t *client,
                                   screen_img_t              screen_img,
                                   screen_img_metadata_t    *metadata,
                                   int                       content_length) {
    const esp_partition_t *part = flash_partition_get_screen_img_partition();

    // Erase only the size of the image currently stored (internal spi flash functions will erase to page
    // boundary automatically)
    if (metadata->screen_img_size) {
        uint32_t alignment_remainder = metadata->screen_img_size % 4096;
        uint32_t size_to_erase =
            alignment_remainder ? (metadata->screen_img_size + (4096 - alignment_remainder)) : metadata->screen_img;

        esp_err_t err = esp_partition_erase_range(part, metadata->screen_img_offset, size_to_erase);
        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_ERROR, "Error erasing partition range: %s", esp_err_to_name(err));
            return 0;
        }

        nvs_set_uint32(metadata->screen_img_size_key, 0);
        nvs_set_uint32(metadata->screen_img_width_key, 0);
        nvs_set_uint32(metadata->screen_img_height_key, 0);
        log_printf(LOG_LEVEL_DEBUG, "Erased %u bytes from %u screen_img_t", size_to_erase, screen_img);
    } else {
        log_printf(LOG_LEVEL_DEBUG,
                   "%s NVS key had zero value, not erasing any of screen img partition",
                   metadata->screen_img_size_key);
    }

    size_t    bytes_saved = 0;
    esp_err_t err         = http_client_read_response_to_flash(client,
                                                       content_length,
                                                       (esp_partition_t *)part,
                                                       metadata->screen_img_offset,
                                                       &bytes_saved);
    if (err == ESP_OK && bytes_saved > 0) {
        // Save metadata as last action to make sure all steps have succeeded and there's a valid image in
        // flash
        nvs_set_uint32(metadata->screen_img_size_key, bytes_saved);
        nvs_set_uint32(metadata->screen_img_width_key, 700);
        nvs_set_uint32(metadata->screen_img_height_key, 200);

        log_printf(LOG_LEVEL_INFO, "Saved %u bytes to screen_img flash partition at 0x%X offset", bytes_saved, 0);
    }

    return bytes_saved;
}

void screen_img_handler_init() {
}

bool screen_img_handler_clear_screen_img(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    display_clear_area(metadata.x_coord, metadata.y_coord, metadata.screen_img_width, metadata.screen_img_height);
    return true;
}

bool screen_img_handler_draw_screen_img(screen_img_t screen_img) {
    const esp_partition_t *screen_img_partition = flash_partition_get_screen_img_partition();
    screen_img_metadata_t  metadata             = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    if (metadata.screen_img_size == 0 || metadata.screen_img_width == 0 || metadata.screen_img_height == 0) {
        log_printf(LOG_LEVEL_INFO,
                   "Zero size, width, and/or height for screen image %d found in NVS. Assuming this image hasn't been "
                   "downloaded, returning from draw function");
        return false;
    }

    // TODO :: make sure screen_img_len length is less that buffer size (or at least a reasonable number to
    // malloc) mmap handles the large malloc internally, and the call the munmap below frees it
    const uint8_t          *mapped_flash = NULL;
    spi_flash_mmap_handle_t spi_flash_handle;
    esp_partition_mmap(screen_img_partition,
                       metadata.screen_img_offset,
                       metadata.screen_img_size,
                       SPI_FLASH_MMAP_DATA,
                       (const void **)&mapped_flash,
                       &spi_flash_handle);
    display_draw_image((uint8_t *)mapped_flash,
                       metadata.screen_img_width,
                       metadata.screen_img_height,
                       1,
                       metadata.x_coord,
                       metadata.y_coord);
    spi_flash_munmap(spi_flash_handle);

    log_printf(LOG_LEVEL_INFO,
               "Rendered image from flash at (%u, %u) sized %u bytes (W: %u, H: %u)",
               metadata.x_coord,
               metadata.y_coord,
               metadata.screen_img_size,
               metadata.screen_img_width,
               metadata.screen_img_height);

    return true;
}

bool screen_img_handler_download_and_save(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    bool                 success = true;
    char                 url[80];
    spot_check_config_t *config     = nvs_get_config();
    const uint8_t        num_params = 4;
    query_param          params[num_params];

    http_request_t req = http_client_build_get_request(metadata.endpoint, config, url, params, num_params);
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_http_client_handle_t client;
    int                      content_length = 0;
    success                                 = http_client_perform_with_retries(&req, 1, &client, &content_length);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "Error making request, aborting");
        return false;
    }

    success = screen_img_handler_save(&client, screen_img, &metadata, content_length);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "Error saving screen img");
        return false;
    }

    return success;
}
