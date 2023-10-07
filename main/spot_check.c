#include <string.h>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "display.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "nvs.h"
#include "scheduler_task.h"
#include "sntp_time.h"
#include "spot_check.h"

#define TAG SC_TAG_SPOT_CHECK

// TODO :: these should all be calc'd off of the display width/height macros
#define TIME_DRAW_X_PX (75)
#define TIME_DRAW_Y_PX (120)

#define DATE_DRAW_X_PX (75)
#define DATE_DRAW_Y_PX (170)

#define CONDITIONS_DRAW_X_PX (725)  // text is right-aligned, so far side of rect
#define CONDITIONS_SPOT_NAME_DRAW_Y_PX (80)
#define CONDITIONS_TEMPERATURE_DRAW_Y_PX (120)
#define CONDITIONS_WIND_DRAW_Y_PX (150)
#define CONDITIONS_TIDE_DRAW_Y_PX (180)

#define OTA_DRAW_X_PX (400)
#define OTA_DRAW_Y_PX \
    (250)  // Draw right in the middle of tide chart - since it only updates every 24hr the likelihood of it updating
           // and re-drawing while we're firmware update is super low

#define OFFLINE_TEXT_DRAW_X_PX (400)
#define OFFLINE_TEXT_DRAW_Y_PX (30)

#define NUM_BYTES_VERSION_STR (26)

const char *const ota_start_text    = "Firmware update in progress, please do not unplug Spot Check device";
const char *const ota_finished_text = "Firmware update successful! Rebooting...";
const char *const offline_text = "Spot Check is having trouble accessing the network, please check your connection";

static struct tm last_time_displayed = {0};
static char      device_serial[20];
static char      firmware_version[NUM_BYTES_VERSION_STR + 1];  // 5-8 bytes for version, 1 for dash, 16 msb of elf hash.
static char      hw_version[10];                               // always less, hardcoded below in ifdefs

char *spot_check_get_serial() {
    return device_serial;
}

char *spot_check_get_fw_version() {
    return firmware_version;
}

char *spot_check_get_hw_version() {
    return hw_version;
}

/*
 * Returns success
 * */
bool spot_check_download_and_save_conditions(conditions_t *new_conditions) {
    if (new_conditions == NULL) {
        return false;
    }

    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 80];
    uint8_t            num_params = 4;
    query_param        params[num_params];
    http_request_t     request = http_client_build_get_request("conditions", config, url_buf, params, num_params);

    char                    *server_response    = NULL;
    size_t                   response_data_size = 0;
    esp_http_client_handle_t client;
    int                      content_length = 0;
    bool                     success        = http_client_perform_with_retries(&request, 1, &client, &content_length);

    // This MUST be here to short circuit execution. If http_client_read_response_to_* is called after a failure of
    // http_client_perform_with_retries, the inner call to client cleanup function will assert and crash and there's
    // nothing we can do to wrap or error check it
    if (!success) {
        log_printf(LOG_LEVEL_ERROR,
                   "Received false success trying to perform req before reading response, bailing out of process");
        return false;
    }

    esp_err_t http_err =
        http_client_read_response_to_buffer(&client, content_length, &server_response, &response_data_size);

    if (http_err == ESP_OK && response_data_size != 0) {
        cJSON *json               = parse_json(server_response);
        cJSON *data_value         = cJSON_GetObjectItem(json, "data");
        cJSON *temperature_object = cJSON_GetObjectItem(data_value, "temp");
        cJSON *wind_speed_object  = cJSON_GetObjectItem(data_value, "wind_speed");
        cJSON *wind_dir_object    = cJSON_GetObjectItem(data_value, "wind_dir");
        cJSON *tide_height_object = cJSON_GetObjectItem(data_value, "tide_height");

        log_printf(LOG_LEVEL_DEBUG, "Server response: %s", server_response);
        if (server_response != NULL) {
            free(server_response);
            server_response = NULL;
        }

        if (wind_dir_object == NULL || tide_height_object == NULL || wind_speed_object == NULL ||
            temperature_object == NULL) {
            log_printf(LOG_LEVEL_ERROR,
                       "Parsed at least one field to a null cJSON object. That means the field wasn't in the response "
                       "at all but a successful request response "
                       "code (usually  a wifi login portal default login page)");
            return false;
        }

        // char *temperature_debug_str = cJSON_Print(temperature_object);
        // char *wind_speed_debug_str  = cJSON_Print(wind_speed_object);
        // char *wind_dir_debug_str    = cJSON_Print(wind_dir_object);
        // char *tide_height_debug_str = cJSON_Print(tide_height_object);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for temperature: %s", temperature_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for wind_speed: %s", wind_speed_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for wind_dir: %s", wind_dir_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for tide_height: %s", tide_height_debug_str);
        // free(temperature_debug_str);
        // free(wind_speed_debug_str);
        // free(wind_dir_debug_str);
        // free(tide_height_debug_str);

        // Parse out end-result values with fallbacks in case value for key is not expected type
        int8_t temperature = 0;
        if (cJSON_IsNumber(temperature_object)) {
            temperature = temperature_object->valueint;
        } else {
            log_printf(LOG_LEVEL_WARN, "Expecting number from api for temp key, did not get one. Defaulting to -99");
            temperature = -99;
        }

        uint8_t wind_speed = 0;
        if (cJSON_IsNumber(wind_speed_object)) {
            wind_speed = wind_speed_object->valueint;
        } else {
            log_printf(LOG_LEVEL_WARN,
                       "Expecting number from api for wind_speed key, did not get one. Defaulting to 99");
            wind_speed = 99;
        }

        char *wind_dir_str = NULL;
        if (cJSON_IsString(wind_dir_object)) {
            wind_dir_str = cJSON_GetStringValue(wind_dir_object);
        } else {
            log_printf(LOG_LEVEL_WARN, "Expecting string from api for wind_dir key, did not get one. Defaulting to ?");
            wind_dir_str = "X";
        }

        char *tide_height_str = NULL;
        if (cJSON_IsString(tide_height_object)) {
            tide_height_str = cJSON_GetStringValue(tide_height_object);
        } else {
            log_printf(LOG_LEVEL_WARN,
                       "Expecting string from api for tide_height key, did not get one. Defaulting to ?");
            tide_height_str = "?";
        }

        // Copy into global conditions after every field set
        new_conditions->temperature = temperature;
        new_conditions->wind_speed  = wind_speed;
        strcpy(new_conditions->wind_dir, wind_dir_str);
        strcpy(new_conditions->tide_height, tide_height_str);

        cJSON_Delete(json);
    } else {
        log_printf(LOG_LEVEL_INFO, "Failed to get new conditions, leaving last saved values displayed");
        return false;
    }

    // Caller responsible for freeing buffer if non-null on return
    if (server_response != NULL) {
        free(server_response);
        server_response = NULL;
    }

    return true;
}
/*
 * Dirties the whole time rect to ensure no gray-in in the time bounding box over time (mostly noticeable around the
minutes digits)
*/
void spot_check_mark_time_dirty() {
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

    if (previous_time_width_px > 0 && previous_time_height_px > 0) {
        display_mark_rect_dirty(TIME_DRAW_X_PX - 5,
                                TIME_DRAW_Y_PX - previous_time_height_px - 5,
                                previous_time_width_px + 10,
                                previous_time_height_px + 10);
        log_printf(LOG_LEVEL_DEBUG,
                   "Marking text rect as dirty coords (%u, %u) width: %u height: %u",
                   TIME_DRAW_X_PX,
                   TIME_DRAW_Y_PX - previous_time_height_px,
                   previous_time_width_px,
                   previous_time_height_px);
    }
}

/*
 * Clears time text by inverting previously-written time
 */
void spot_check_clear_time() {
    char time_string[6];
    sntp_time_get_time_str(&last_time_displayed, time_string, NULL);
    display_invert_text(time_string, TIME_DRAW_X_PX, TIME_DRAW_Y_PX, DISPLAY_FONT_SIZE_LARGE, DISPLAY_FONT_ALIGN_LEFT);
}

bool spot_check_draw_time() {
    struct tm now_local = {0};
    char      time_string[6];
    sntp_time_get_local_time(&now_local);
    sntp_time_get_time_str(&now_local, time_string, NULL);

    display_draw_text(time_string, TIME_DRAW_X_PX, TIME_DRAW_Y_PX, DISPLAY_FONT_SIZE_LARGE, DISPLAY_FONT_ALIGN_LEFT);

    memcpy(&last_time_displayed, &now_local, sizeof(struct tm));
    return true;
}

/*
 * Clears the date string. Dumb-clear, clears full rect every time instead of determining single digit, month, day of
 * week, etc specific clear area.
 */
void spot_check_clear_date() {
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
        // Add 5px buffer for lowercase letters
        display_clear_area(DATE_DRAW_X_PX - 5,
                           DATE_DRAW_Y_PX - previous_date_height_px - 5,
                           previous_date_width_px + 10,
                           previous_date_height_px + 10);
    }
}

/*
 * Draw the date string at the correct location.
 */
bool spot_check_draw_date() {
    struct tm now_local = {0};
    char      date_string[64];
    sntp_time_get_local_time(&now_local);
    sntp_time_get_time_str(&now_local, NULL, date_string);

    display_draw_text(date_string, DATE_DRAW_X_PX, DATE_DRAW_Y_PX, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_LEFT);
    return true;
}

void spot_check_clear_spot_name() {
    // TODO :: would be nicer to pass currently displayed spot name here so we can smart invert erase instead of
    // block erasing based on max width. See todo in conditions task for same point
    const uint32_t max_spot_name_width_px = 300;

    uint32_t spot_name_width  = 0;
    uint32_t spot_name_height = 0;
    display_get_text_bounds("O",
                            CONDITIONS_DRAW_X_PX,
                            CONDITIONS_SPOT_NAME_DRAW_Y_PX,
                            DISPLAY_FONT_SIZE_SHMEDIUM,
                            DISPLAY_FONT_ALIGN_RIGHT,
                            &spot_name_width,
                            &spot_name_height);
    display_clear_area(CONDITIONS_DRAW_X_PX - max_spot_name_width_px - 5,
                       CONDITIONS_SPOT_NAME_DRAW_Y_PX - spot_name_height - 5,
                       max_spot_name_width_px + 10,
                       spot_name_height + 10);

    // Underline
    display_draw_rect(CONDITIONS_DRAW_X_PX - spot_name_width, CONDITIONS_SPOT_NAME_DRAW_Y_PX + 5, spot_name_width, 2);
}

bool spot_check_draw_spot_name(char *spot_name) {
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

    // Underline
    display_draw_rect(CONDITIONS_DRAW_X_PX - spot_name_width, CONDITIONS_SPOT_NAME_DRAW_Y_PX + 5, spot_name_width, 2);
    return true;
}

void spot_check_clear_conditions(bool clear_temperature, bool clear_wind, bool clear_tide) {
    const char *max_conditions_string = "Fetching latest conditions...";
    uint32_t    max_conditions_width_px;
    uint32_t    max_conditions_height_px;
    display_get_text_bounds((char *)max_conditions_string,
                            0,
                            0,
                            DISPLAY_FONT_SIZE_SMALL,
                            DISPLAY_FONT_ALIGN_RIGHT,
                            &max_conditions_width_px,
                            &max_conditions_height_px);

    // If one of the condition lines is false, erase individual lines. Otherwise erase as large box
    if (clear_temperature && clear_wind && clear_tide) {
        // CONDITIONS_DRAW_TEMPERATURE_Y is the bottom left corner of the top row of text. Need to get bounds of
        // text for the height so we can subtract it from the constant Y to get the top left corner, which is what
        // the clear function expects its rect to start
        uint32_t font_width_px  = 0;
        uint32_t font_height_px = 0;
        display_get_text_bounds("F",
                                0,
                                0,
                                DISPLAY_FONT_SIZE_SHMEDIUM,
                                DISPLAY_FONT_ALIGN_RIGHT,
                                &font_width_px,
                                &font_height_px);

        // Erase from top left of conditions  hardcoded max width of conditions block, down to bottom right of
        // conditions (bottom right of tide text) plus a little buffer
        display_clear_area(CONDITIONS_DRAW_X_PX - max_conditions_width_px,
                           CONDITIONS_TEMPERATURE_DRAW_Y_PX - font_height_px,
                           max_conditions_width_px,
                           CONDITIONS_TIDE_DRAW_Y_PX - (CONDITIONS_TEMPERATURE_DRAW_Y_PX - font_height_px) + 10);
    } else {
        log_printf(LOG_LEVEL_ERROR, "CLEARINING INDIVIDUAL CONDITION LINES NOT YET SUPPORTED");
        MEMFAULT_ASSERT(0);
    }
}

bool spot_check_draw_conditions(conditions_t *conditions) {
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

bool spot_check_draw_conditions_error() {
    display_draw_text("Error fetching conditions",
                      CONDITIONS_DRAW_X_PX,
                      CONDITIONS_TEMPERATURE_DRAW_Y_PX,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_RIGHT);
    return true;
}

bool spot_check_clear_ota_start_text() {
    display_invert_text((char *)ota_start_text,
                        OTA_DRAW_X_PX,
                        OTA_DRAW_Y_PX,
                        DISPLAY_FONT_SIZE_MEDIUM,
                        DISPLAY_FONT_ALIGN_CENTER);

    // uint32_t ota_text_width;
    // uint32_t ota_text_height;
    // display_get_text_bounds((char *)ota_start_text,
    //                         &ota_text_width,
    //                         &ota_text_height);

    return true;
}

bool spot_check_draw_ota_finished_text() {
    display_draw_text((char *)ota_finished_text,
                      OTA_DRAW_X_PX,
                      OTA_DRAW_Y_PX,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    return true;
}

/*
 * Give user notification that FW is updating so don't unplug
 */
bool spot_check_draw_ota_start_text() {
    display_draw_text((char *)ota_start_text,
                      OTA_DRAW_X_PX,
                      OTA_DRAW_Y_PX,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    return true;
}

void spot_check_show_unprovisioned_screen() {
    log_printf(LOG_LEVEL_WARN, "No prov info saved, showing provisioning screen without network checks.");
    display_full_clear();
    display_draw_text(
        "Download the Spot Check app and follow\nthe configuration steps to connect\n your device to a wifi "
        "network",
        400,
        300,
        DISPLAY_FONT_SIZE_SHMEDIUM,
        DISPLAY_FONT_ALIGN_CENTER);
}

void spot_check_show_no_network_screen() {
    log_printf(LOG_LEVEL_ERROR, "Prov info is saved, but could not find or connect to saved network.");
    display_full_clear();
    display_draw_text("Network not found", 400, 250, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text(
        "Spot Check could not find or connect to the network used previously.\nVerify network is "
        "available or use the Spot Check app to connect to a new network",
        400,
        300,
        DISPLAY_FONT_SIZE_SMALL,
        DISPLAY_FONT_ALIGN_CENTER);
}

void spot_check_clear_checking_connection_screen() {
    display_invert_text("Connecting to network...", 400, 350, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
}

void spot_check_show_checking_connection_screen() {
    log_printf(
        LOG_LEVEL_INFO,
        "Connection to network successful, showing 'connecting to network' screen while performing api healthcheck");
    display_draw_text("Connecting to network...", 400, 350, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
}

void spot_check_show_no_internet_screen() {
    log_printf(LOG_LEVEL_ERROR, "Connection to network successful and assigned IP, but no internet connection");
    display_full_clear();
    display_draw_text("No internet connection", 400, 250, DISPLAY_FONT_SIZE_SHMEDIUM, DISPLAY_FONT_ALIGN_CENTER);
    display_draw_text("Spot Check is connected to the the WiFi\nnetwork but cannot reach the internet.",
                      400,
                      325,
                      DISPLAY_FONT_SIZE_SMALL,
                      DISPLAY_FONT_ALIGN_CENTER);

    display_draw_text(
        "Verify local network is "
        "connected to the internet or\nuse the Spot Check app to connect to a new network",
        400,
        400,
        DISPLAY_FONT_SIZE_SMALL,
        DISPLAY_FONT_ALIGN_CENTER);
}

void spot_check_draw_fetching_conditions_text() {
    log_printf(
        LOG_LEVEL_INFO,
        "Connection to network successful, showing 'fetching data' screen while waiting for scheduler to full update");
    display_draw_text("Fetching latest conditions...", 400, 300, DISPLAY_FONT_SIZE_SMALL, DISPLAY_FONT_ALIGN_CENTER);
}

/*
 * Wrappers for display render funcs so our logic modules don't have a dependency on display driver
 */
void spot_check_full_clear() {
    display_full_clear();
}

void spot_check_mark_all_lines_dirty() {
    display_mark_all_lines_dirty();
}

void spot_check_render(const char *calling_func, uint32_t line) {
    display_render(calling_func, line);
}

/*
 * Wrapper function for any module to call when device transitions to offline. Handles both updating the scheduler and
 * drawing necessary updates to the display.
 */
void spot_check_set_offline_mode() {
    log_printf(LOG_LEVEL_WARN, "%s called", __func__);

    bool draw_and_render_text = scheduler_get_mode() != SCHEDULER_MODE_OFFLINE;

    scheduler_set_offline_mode();

    // TODO :: need to handle case where offline mode switched back to online mode, but then gets kicked back to offline
    // before this is hit again. This code would assume that offline text had already been drawn, because it didn't know
    // it was cleared. Not a huge deal, just means the user won't be notified about connection issues but FW logic is
    // still solid
    if (draw_and_render_text) {
        display_draw_text((char *)offline_text,
                          OFFLINE_TEXT_DRAW_X_PX,
                          OFFLINE_TEXT_DRAW_Y_PX,
                          DISPLAY_FONT_SIZE_SMALL,
                          DISPLAY_FONT_ALIGN_CENTER);

        spot_check_render(__func__, __LINE__);
    }
}

/*
 * Main FW init for spot check specific data
 */
void spot_check_init() {
    // Init last_time_display with epoch so date update logic always executes to start with
    time_t epoch = 0;
    memcpy(&last_time_displayed, localtime(&epoch), sizeof(struct tm));

    uint8_t mac[6];
    // Note: must use this mac-reading func, it's the base one that actually pulls values from EFUSE while others just
    // check that copy in RAM

    // Store device ID
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        MEMFAULT_ASSERT(0);
    }
    snprintf(device_serial, 20, "%02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Store firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // Assuming min length of version is 5 (1.1.1) and max is 8 (11.11.11). Always take 8 chars / 4 bytes from elf sha
    snprintf(firmware_version,
             NUM_BYTES_VERSION_STR,
             "%*.*s-%02x%02x%02x%02x",
             5,
             8,
             app_desc->version,
             app_desc->app_elf_sha256[0],
             app_desc->app_elf_sha256[1],
             app_desc->app_elf_sha256[2],
             app_desc->app_elf_sha256[3]);

    // Store HW ID
#ifdef CONFIG_SPOT_CHECK_REV_3_1
    sprintf(hw_version, "rev3.1");
#elif defined(CONFIG_SPOT_CHECK_REV_2)
    sprintf(hw_version, "rev2.0");
#elif defined(CONFIG_ESP32_DEVBOARD)
    sprintf(hw_version, "revDEV");
#else
#error Current HW rev specified in menuconfig is not supported in memfault config
#endif
}
