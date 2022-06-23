#include "freertos/FreeRTOS.h"

#include "esp_partition.h"
#include "esp_spi_flash.h"

#include "display.h"
#include "flash_partition.h"
#include "http_client.h"
#include "log.h"
#include "nvs.h"
#include "screen_img_handler.h"

#define TAG "sc-screenimg"

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
            metadata->endpoint              = "test_tide_chart.raw";
            break;
        case SCREEN_IMG_SWELL_CHART:
            metadata->x_coord               = 50;
            metadata->y_coord               = 400;
            metadata->screen_img_size_key   = SCREEN_IMG_SWELL_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_SWELL_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_SWELL_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_SWELL_CHART_OFFSET;
            metadata->endpoint              = "test_swell_chart.raw";
            break;
        default:
            configASSERT(0);
    }

    bool success = nvs_get_uint32(metadata->screen_img_size_key, &metadata->screen_img_size);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "No screen img size value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_width_key, &metadata->screen_img_width);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "No screen img width value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_height_key, &metadata->screen_img_height);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "No screen img height value stored in NVS, setting to zero");
    }
}

/*
 * Finished process of saving a screen_img to the proper location in the flash partition. Request must have been built
 * and sent with http_client_build_request and http_client_perform_request already.
 */
static int screen_img_handler_save(esp_http_client_handle_t *client,
                                   screen_img_t              screen_img,
                                   screen_img_metadata_t    *metadata) {
    const esp_partition_t *part = flash_partition_get_screen_img_partition();

    // Erase only the size of the image currently stored (internal spi flash functions will erase to page
    // boundary automatically)
    if (metadata->screen_img_size) {
        esp_partition_erase_range(part, metadata->screen_img_offset, metadata->screen_img_size);
        nvs_set_uint32(metadata->screen_img_size_key, 0);
        nvs_set_uint32(metadata->screen_img_width_key, 0);
        nvs_set_uint32(metadata->screen_img_height_key, 0);
        log_printf(TAG, LOG_LEVEL_DEBUG, "Erased %u bytes from %u screen_img_t", metadata->screen_img_size, screen_img);
    } else {
        log_printf(TAG,
                   LOG_LEVEL_DEBUG,
                   "%s NVS key had zero value, not erasing any of screen img partition",
                   metadata->screen_img_size_key);
    }

    int bytes_saved = http_client_read_response_to_flash(client, (esp_partition_t *)part, metadata->screen_img_offset);
    if (bytes_saved > 0) {
        // Save metadata as last action to make sure all steps have succeeded and there's a valid image in
        // flash
        nvs_set_uint32(metadata->screen_img_size_key, bytes_saved);
        nvs_set_uint32(metadata->screen_img_width_key, 700);
        nvs_set_uint32(metadata->screen_img_height_key, 200);

        log_printf(TAG, LOG_LEVEL_INFO, "Saved %u bytes to screen_img flash partition at 0x%X offset", bytes_saved, 0);
    }

    return bytes_saved;
}

bool screen_img_handler_render_screen_img(screen_img_t screen_img) {
    const esp_partition_t *screen_img_partition = flash_partition_get_screen_img_partition();
    screen_img_metadata_t  metadata             = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

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
    display_render_image((uint8_t *)mapped_flash,
                         metadata.screen_img_width,
                         metadata.screen_img_height,
                         1,
                         metadata.x_coord,
                         metadata.y_coord);
    spi_flash_munmap(spi_flash_handle);

    log_printf(TAG,
               LOG_LEVEL_INFO,
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

    bool               success = true;
    char               url[80];
    spot_check_config *config     = nvs_get_config();
    const uint8_t      num_params = 3;
    query_param        params[num_params];

    request req = http_client_build_request(metadata.endpoint, config, url, params, num_params);
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_http_client_handle_t client;
    success = http_client_perform_request(&req, &client);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Error making request, aborting");
        return false;
    }

    success = screen_img_handler_save(&client, screen_img, &metadata);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Error saving screen img");
        return false;
    }

    return success;
}
