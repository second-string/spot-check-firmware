#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"

#include "esp_partition.h"
#include "esp_sntp.h"
#include "esp_spi_flash.h"

#include "display.h"
#include "flash_partition.h"
#include "http_client.h"
#include "log.h"
#include "nvs.h"
#include "screen_img_handler.h"
#include "sntp_time.h"

#define TAG "sc-screenimg"

#define TIME_DRAW_X_PX (75)
#define TIME_DRAW_Y_PX (120)

#define DATE_DRAW_X_PX (75)
#define DATE_DRAW_Y_PX (170)

#define CONDITIONS_DRAW_X_PX (725)  // text is right-aligned, so far side of rect
#define CONDITIONS_SPOT_NAME_DRAW_Y_PX (80)
#define CONDITIONS_TEMPERATURE_DRAW_Y_PX (120)
#define CONDITIONS_WIND_DRAW_Y_PX (150)
#define CONDITIONS_TIDE_DRAW_Y_PX (180)

static struct tm last_time_displayed = {0};

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
            configASSERT(0);
    }

    bool success = nvs_get_uint32(metadata->screen_img_size_key, &metadata->screen_img_size);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "No screen img size value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_width_key, &metadata->screen_img_width);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "No screen img width value stored in NVS, setting to zero");
    }
    success = nvs_get_uint32(metadata->screen_img_height_key, &metadata->screen_img_height);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "No screen img height value stored in NVS, setting to zero");
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
        log_printf(LOG_LEVEL_DEBUG, "Erased %u bytes from %u screen_img_t", metadata->screen_img_size, screen_img);
    } else {
        log_printf(LOG_LEVEL_DEBUG,
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

        log_printf(LOG_LEVEL_INFO, "Saved %u bytes to screen_img flash partition at 0x%X offset", bytes_saved, 0);
    }

    return bytes_saved;
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
        log_printf(LOG_LEVEL_ERROR, "Error making request, aborting");
        return false;
    }

    success = screen_img_handler_save(&client, screen_img, &metadata);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "Error saving screen img");
        return false;
    }

    return success;
}

/*
 * Draw the date string at the correct location. static scoped since it's only triggered from
 * screen_img_handler_draw_time if the date has advanced
 */
static bool screen_img_handler_draw_date() {
    struct tm now_local = {0};
    char      date_string[64];
    sntp_time_get_local_time(&now_local);
    sntp_time_get_time_str(&now_local, NULL, date_string);

    display_draw_text(date_string, DATE_DRAW_X_PX, DATE_DRAW_Y_PX, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_LEFT);
    return true;
}

/*
 * Clears the date string. Dumb-clear, clears full rect every time instead of determining single digit, month, day of
 * week, etc specific clear area.. static scoped since it's only triggered from screen_img_handler_draw_time if the date
 * has advanced
 */
static void screen_img_handler_clear_date() {
    char     date_string[64];
    uint32_t previous_date_width_px;
    uint32_t previous_date_height_px;
    sntp_time_get_time_str(&last_time_displayed, NULL, date_string);
    display_get_text_bounds(date_string,
                            DATE_DRAW_X_PX,
                            DATE_DRAW_Y_PX,
                            DISPLAY_FONT_SIZE_SHMEDIUM,
                            DISPLAY_FONT_ALIGN_LEFT,
                            &previous_date_width_px,
                            &previous_date_height_px);

    if (previous_date_width_px > 0 && previous_date_height_px > 0) {
        display_clear_area(DATE_DRAW_X_PX, DATE_DRAW_Y_PX, previous_date_width_px, previous_date_height_px);
    }
}

bool screen_img_handler_draw_time() {
    struct tm now_local = {0};
    char      time_string[6];
    sntp_time_get_local_time(&now_local);
    sntp_time_get_time_str(&now_local, time_string, NULL);

    display_draw_text(time_string, TIME_DRAW_X_PX, TIME_DRAW_Y_PX, DISPLAY_FONT_SIZE_LARGE, DISPLAY_FONT_ALIGN_LEFT);

    // If the day has advanced, update date text too
    if (now_local.tm_mday != last_time_displayed.tm_mday) {
        log_printf(LOG_LEVEL_INFO, "Date has advanced, clearing and re-rendering date as well");
        screen_img_handler_clear_date();
        screen_img_handler_draw_date();
    }

    memcpy(&last_time_displayed, &now_local, sizeof(struct tm));
    return true;
}

void screen_img_handler_clear_time() {
    // TODO :: HACK - make this cli command behavior
    // struct timeval  tv = {0};
    // struct timezone tz = {0};
    // gettimeofday(&tv, &tz);
    // tv.tv_sec = tv.tv_sec + 60;
    // settimeofday(&tv, &tz);

    char     time_string[6];
    uint32_t previous_time_width_px;
    uint32_t previous_time_height_px;
    sntp_time_get_time_str(&last_time_displayed, time_string, NULL);
    display_get_text_bounds(time_string,
                            TIME_DRAW_X_PX,
                            TIME_DRAW_Y_PX,
                            DISPLAY_FONT_SIZE_LARGE,
                            DISPLAY_FONT_ALIGN_LEFT,
                            &previous_time_width_px,
                            &previous_time_height_px);

    struct tm now_local = {0};
    sntp_time_get_local_time(&now_local);

    uint32_t erase_x      = 0;
    uint32_t erase_y      = 0;
    uint32_t erase_width  = 0;
    uint32_t erase_height = 0;
    uint32_t digit_width  = 0;
    uint32_t digit_height = 0;

    // If the hour changed at all (one or both digits), minutes has to have changed as well so just erase full time box.
    // If hours haven't changed but minutes have, erase one or two digits depending on what the new minutes value is.
    if (now_local.tm_hour != last_time_displayed.tm_hour) {
        erase_x      = TIME_DRAW_X_PX;
        erase_y      = TIME_DRAW_Y_PX;
        erase_width  = previous_time_width_px;
        erase_height = previous_time_height_px;
    } else if (now_local.tm_min != last_time_displayed.tm_min) {
        // Whether erasing one or two digits of minute, get the bounds of the one or two characters to use in
        // calculating what size of rect to erase (character width and kerning varies)
        if (now_local.tm_min >= 10 && ((now_local.tm_min % 10) == 0)) {
            // Erase both digits of minute
            char digit[3];
            sprintf(digit, "%02u", last_time_displayed.tm_min % 100);
            display_get_text_bounds(digit,
                                    0,
                                    0,
                                    DISPLAY_FONT_SIZE_LARGE,
                                    DISPLAY_FONT_ALIGN_LEFT,
                                    &digit_width,
                                    &digit_height);
        } else {
            // Erase ones digit of minute
            char digit[2];
            sprintf(digit, "%u", last_time_displayed.tm_min % 10);
            display_get_text_bounds(digit,
                                    0,
                                    0,
                                    DISPLAY_FONT_SIZE_LARGE,
                                    DISPLAY_FONT_ALIGN_LEFT,
                                    &digit_width,
                                    &digit_height);
        }

        erase_x      = TIME_DRAW_X_PX + previous_time_width_px - digit_width;
        erase_y      = TIME_DRAW_Y_PX - previous_time_height_px;
        erase_width  = digit_width;
        erase_height = previous_time_height_px;
    }

    // Text draws from x,y in lower left corner, but erase rect treats x,y as upper left, so compensate by subracting
    // height from y. Add a bit of padding to be 100% sure we clear all artifacts
    if (erase_width > 0 && erase_height > 0) {
        display_clear_area(erase_x - 5, erase_y - 5, erase_width + 10, erase_height + 10);
        log_printf(LOG_LEVEL_DEBUG,
                   "Erasing text starting (%u, %u) width: %u height: %u",
                   erase_x,
                   erase_y,
                   erase_width,
                   erase_height);
    }
}

void screen_img_handler_clear_conditions(bool clear_spot_name,
                                         bool clear_temperature,
                                         bool clear_wind,
                                         bool clear_tide) {
    // Hardcoding in separate variable for clarity
    const uint32_t max_conditions_width_px = 300;

    // If one of the condition lines is false, erase individual lines. Otherwise erase as large box
    if (clear_temperature && clear_wind && clear_tide) {
        // CONDITIONS_DRAW_TEMPERATURE_Y is the bottom left corner of the top row of text. Need to get bounds of text
        // for the height so we can subtract it from the constant Y to get the top left corner, which is what the clear
        // function expects its rect to start
        uint32_t font_width_px  = 0;
        uint32_t font_height_px = 0;
        display_get_text_bounds("F",
                                0,
                                0,
                                DISPLAY_FONT_SIZE_SHMEDIUM,
                                DISPLAY_FONT_ALIGN_RIGHT,
                                &font_width_px,
                                &font_height_px);

        // Erase from top left of conditions (top left of highest row either spot name or temp depending on params,
        // hardcoded max width of conditions block, down to bottom right of conditions (bottom right of tide text) plus
        // a little buffer)
        uint32_t top_row_y = clear_spot_name ? CONDITIONS_SPOT_NAME_DRAW_Y_PX : CONDITIONS_TEMPERATURE_DRAW_Y_PX;
        display_clear_area(CONDITIONS_DRAW_X_PX - max_conditions_width_px,
                           top_row_y - font_height_px,
                           max_conditions_width_px,
                           CONDITIONS_TIDE_DRAW_Y_PX - (top_row_y - font_height_px) + 10);
    } else {
        log_printf(LOG_LEVEL_ERROR, "CLEARINING INDIVIDUAL CONDITION LINES NOT YET SUPPORTED");
        configASSERT(0);
    }
}

bool screen_img_handler_draw_conditions(char *spot_name, conditions_t *conditions) {
    // Draw spot name and underline no matter what
    uint32_t spot_name_width  = 0;
    uint32_t spot_name_height = 0;
    display_get_text_bounds(spot_name,
                            CONDITIONS_DRAW_X_PX,
                            CONDITIONS_SPOT_NAME_DRAW_Y_PX,
                            DISPLAY_FONT_SIZE_SHMEDIUM,
                            DISPLAY_FONT_ALIGN_RIGHT,
                            &spot_name_width,
                            &spot_name_height);
    display_draw_text(spot_name,
                      CONDITIONS_DRAW_X_PX,
                      CONDITIONS_SPOT_NAME_DRAW_Y_PX,
                      DISPLAY_FONT_SIZE_SHMEDIUM,
                      DISPLAY_FONT_ALIGN_RIGHT);
    vTaskDelay(pdMS_TO_TICKS(1000));
    display_draw_rect(CONDITIONS_DRAW_X_PX - spot_name_width, CONDITIONS_SPOT_NAME_DRAW_Y_PX + 5, spot_name_width, 2);

    if (conditions == NULL) {
        display_draw_text("Fetching latest conditions...",
                          CONDITIONS_DRAW_X_PX,
                          CONDITIONS_TEMPERATURE_DRAW_Y_PX,
                          DISPLAY_FONT_SIZE_SMALL,
                          DISPLAY_FONT_ALIGN_RIGHT);
    } else {
        // Expect max 3 digit temp (or negative 2 digit)
        char temperature_str[9];
        // Expect max 2 digit speed & 3 char direction for wind
        char wind_str[12];
        // Expect max negative double-digit w/ decimal tide height
        char tide_str[18];
        sprintf(temperature_str, "%dº F", conditions->temperature);
        sprintf(wind_str, "%d kt. %s", conditions->wind_speed, conditions->wind_dir);
        // TODO :: still not retrieviing rising / falling from api
        sprintf(tide_str, "%s ft. %s", conditions->tide_height, "rising");
        display_draw_text(temperature_str,
                          CONDITIONS_DRAW_X_PX,
                          CONDITIONS_TEMPERATURE_DRAW_Y_PX,
                          DISPLAY_FONT_SIZE_SHMEDIUM,
                          DISPLAY_FONT_ALIGN_RIGHT);
        display_draw_text(wind_str,
                          CONDITIONS_DRAW_X_PX,
                          CONDITIONS_WIND_DRAW_Y_PX,
                          DISPLAY_FONT_SIZE_SHMEDIUM,
                          DISPLAY_FONT_ALIGN_RIGHT);
        display_draw_text(tide_str,
                          CONDITIONS_DRAW_X_PX,
                          CONDITIONS_TIDE_DRAW_Y_PX,
                          DISPLAY_FONT_SIZE_SHMEDIUM,
                          DISPLAY_FONT_ALIGN_RIGHT);
    }

    return true;
}

bool screen_img_handler_draw_conditions_error() {
    display_draw_text("Error fetching conditions",
                      CONDITIONS_DRAW_X_PX,
                      CONDITIONS_TEMPERATURE_DRAW_Y_PX,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_RIGHT);
    return true;
}

/*
 * Wrapper for display_render so our logic modules don't have a dependency on display driver
 */
void screen_img_handler_render() {
    display_render();
}
