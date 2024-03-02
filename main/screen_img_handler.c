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
#include "spot_check.h"

#define TAG SC_TAG_SCREEN_IMG_HANDLER

#define WEATHER_CHART_X_COORD (50)
#define WEATHER_CHART_1_Y_COORD_PX (200)
#define WEATHER_CHART_2_Y_COORD_PX (400)

typedef struct {
    screen_img_t screen_img;
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
            metadata->screen_img_size_key   = SCREEN_IMG_TIDE_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_TIDE_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_TIDE_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_TIDE_CHART_OFFSET;
            metadata->screen_img_width      = 700;
            metadata->screen_img_height     = 200;
            metadata->endpoint              = "tides_chart";
            break;
        case SCREEN_IMG_SWELL_CHART:
            metadata->screen_img_size_key   = SCREEN_IMG_SWELL_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_SWELL_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_SWELL_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_SWELL_CHART_OFFSET;
            metadata->screen_img_width      = 700;
            metadata->screen_img_height     = 200;
            metadata->endpoint              = "swell_chart";
            break;
        case SCREEN_IMG_WIND_CHART:
            metadata->screen_img_size_key   = SCREEN_IMG_WIND_CHART_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_WIND_CHART_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_WIND_CHART_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_WIND_CHART_OFFSET;
            metadata->screen_img_width      = 700;
            metadata->screen_img_height     = 200;
            metadata->endpoint              = "wind_chart";
            break;
        case SCREEN_IMG_CUSTOM_SCREEN:
            metadata->screen_img_size_key   = SCREEN_IMG_CUSTOM_SCREEN_SIZE_NVS_KEY;
            metadata->screen_img_width_key  = SCREEN_IMG_CUSTOM_SCREEN_WIDTH_PX_NVS_KEY;
            metadata->screen_img_height_key = SCREEN_IMG_CUSTOM_SCREEN_HEIGHT_PX_NVS_KEY;
            metadata->screen_img_offset     = SCREEN_IMG_CUSTOM_SCREEN_OFFSET;
            metadata->screen_img_width      = 800;
            metadata->screen_img_height     = 600;

            // Making an assumption this will never be called before nvs is inited and loaded into mem
            spot_check_config_t *config = nvs_get_config();
            metadata->endpoint          = config->custom_screen_url;
            break;
        default:
            MEMFAULT_ASSERT(0);
    }

    bool success = nvs_get_uint32(metadata->screen_img_size_key, &metadata->screen_img_size);
    if (!success) {
        log_printf(LOG_LEVEL_WARN, "No screen img size value stored in NVS, setting to zero");
    }

    uint32_t temp_dim = 0;
    success           = nvs_get_uint32(metadata->screen_img_width_key, &temp_dim);
    if (!success) {
        log_printf(LOG_LEVEL_WARN,
                   "No screen img width value stored in NVS, keeping default of %u",
                   metadata->screen_img_width);
    } else {
        metadata->screen_img_width = temp_dim;
    }

    success = nvs_get_uint32(metadata->screen_img_height_key, &temp_dim);
    if (!success) {
        log_printf(LOG_LEVEL_WARN,
                   "No screen img height value stored in NVS, keeping default of %u",
                   metadata->screen_img_height);
    } else {
        metadata->screen_img_height = temp_dim;
    }
}
static void screen_img_handler_log_metadata(screen_img_metadata_t *metadata) {
    log_printf(LOG_LEVEL_DEBUG, "SCREEN IMG HANDLER METADATA:");
    // log_printf(LOG_LEVEL_DEBUG, "  x_coord: %u", metadata->x_coord);
    // log_printf(LOG_LEVEL_DEBUG, "  y_coord: %u", metadata->y_coord);
    log_printf(LOG_LEVEL_DEBUG, "  %s: %lu", metadata->screen_img_size_key, metadata->screen_img_size);
    log_printf(LOG_LEVEL_DEBUG, "  %s: %lu", metadata->screen_img_width_key, metadata->screen_img_width);
    log_printf(LOG_LEVEL_DEBUG, "  %s: %lu", metadata->screen_img_height_key, metadata->screen_img_height);
    log_printf(LOG_LEVEL_DEBUG, "  offset: %lu", metadata->screen_img_offset);
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
        nvs_set_uint32(metadata->screen_img_width_key, metadata->screen_img_width);
        nvs_set_uint32(metadata->screen_img_height_key, metadata->screen_img_height);

        log_printf(LOG_LEVEL_INFO, "Saved %u bytes to screen_img flash partition at 0x%X offset", bytes_saved, 0);
    }

    return bytes_saved;
}

/*
 * Return the correct Y coord for a chart depending on which active chart it is.
 * NOTE: Asserts if chart passed in is not one of the two active charts in the config.
 */
static uint32_t screen_img_handler_get_y_for_chart(screen_img_t screen_img) {
    uint32_t y = 0;

    spot_check_config_t *config = nvs_get_config();
    if (screen_img == config->active_chart_1) {
        y = WEATHER_CHART_1_Y_COORD_PX;
    } else if (screen_img == config->active_chart_2) {
        y = WEATHER_CHART_2_Y_COORD_PX;
    } else {
        MEMFAULT_ASSERT(0);
    }

    return y;
}

/*
 * Static function to hold to shared logic of drawing either a chart or any other screen image to the screen. The args
 * are calculated differently for the two cases, then passed into a call for this func.
 */
static void screen_img_handler_retrieve_and_render(uint32_t x,
                                                   uint32_t y,
                                                   size_t   size,
                                                   size_t   width,
                                                   size_t   height,
                                                   size_t   nvs_address_offset) {
    // TODO :: make sure screen_img_len length is less that buffer size (or at least a reasonable number to
    // malloc) mmap handles the large malloc internally, and the call the munmap below frees it
    const uint8_t          *mapped_flash = NULL;
    spi_flash_mmap_handle_t spi_flash_handle;
    const esp_partition_t  *screen_img_partition = flash_partition_get_screen_img_partition();
    esp_partition_mmap(screen_img_partition,
                       nvs_address_offset,
                       size,
                       SPI_FLASH_MMAP_DATA,
                       (const void **)&mapped_flash,
                       &spi_flash_handle);
    display_draw_image((uint8_t *)mapped_flash, width, height, 1, x, y);
    spi_flash_munmap(spi_flash_handle);

    log_printf(LOG_LEVEL_INFO,
               "Rendered image from flash at (%u, %u) sized %u bytes (W: %u, H: %u)",
               x,
               y,
               size,
               width,
               height);
}

void screen_img_handler_init() {
}

/*
 * Clear a non-chart image from the screen. NOTE: right now this assumes this image starts at 0,0 since the only
 * non-chart image we have is fullscreen custom screen. If that changes, this will have to be re-architected.
 */
bool screen_img_handler_clear_screen_img(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    display_clear_area(0, 0, metadata.screen_img_width, metadata.screen_img_height);
    return true;
}

/*
 * Clear a chart image from the screen. This checks which active chart the chart-to-clear is and then sets the x,y
 * coords from that. This will assert if clearing a chart that's not an active chart!
 */
bool screen_img_handler_clear_chart(screen_img_t screen_img) {
    uint32_t y = screen_img_handler_get_y_for_chart(screen_img);

    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    display_clear_area(WEATHER_CHART_X_COORD, y, metadata.screen_img_width, metadata.screen_img_height);
    return true;
}

bool screen_img_handler_draw_screen_img(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    if (metadata.screen_img_size == 0 || metadata.screen_img_width == 0 || metadata.screen_img_height == 0) {
        log_printf(LOG_LEVEL_INFO,
                   "Zero size, width, and/or height for screen image %d found in NVS. Assuming this image hasn't been "
                   "downloaded, returning from draw function",
                   screen_img);
        return false;
    }

    screen_img_handler_retrieve_and_render(0,
                                           0,
                                           metadata.screen_img_size,
                                           metadata.screen_img_width,
                                           metadata.screen_img_height,
                                           metadata.screen_img_offset);
    return true;
}

bool screen_img_handler_draw_chart(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);

    if (metadata.screen_img_size == 0 || metadata.screen_img_width == 0 || metadata.screen_img_height == 0) {
        log_printf(LOG_LEVEL_INFO,
                   "Zero size, width, and/or height for screen image %d found in NVS. Assuming this image hasn't been "
                   "downloaded, returning from draw function",
                   screen_img);
        return false;
    }

    uint32_t y = screen_img_handler_get_y_for_chart(screen_img);

    screen_img_handler_retrieve_and_render(WEATHER_CHART_X_COORD,
                                           y,
                                           metadata.screen_img_size,
                                           metadata.screen_img_width,
                                           metadata.screen_img_height,
                                           metadata.screen_img_offset);
    return true;
}

bool screen_img_handler_download_and_save(screen_img_t screen_img) {
    screen_img_metadata_t metadata = {0};
    screen_img_handler_get_metadata(screen_img, &metadata);
    screen_img_handler_log_metadata(&metadata);

    bool                 success        = true;
    size_t               max_url_length = 120;
    char                 url[max_url_length];
    http_request_t       req        = {0};
    uint8_t              num_params = 4;
    query_param          params[num_params];
    spot_check_config_t *config = nvs_get_config();

    if (config->operating_mode == SPOT_CHECK_MODE_CUSTOM) {
        req = http_client_build_external_get_request(metadata.endpoint, url, max_url_length);
    } else {
        req = http_client_build_get_request(metadata.endpoint, config, url, params, num_params);
    }

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
