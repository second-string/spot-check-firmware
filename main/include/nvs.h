#ifndef NVS_H
#define NVS_H

#include <stdbool.h>

#include "esp_err.h"

#include "http_server.h"
#include "log.h"
#include "screen_img_handler.h"
#include "spot_check.h"

typedef struct {
    char             *spot_name;
    char             *spot_uid;
    char             *spot_lat;
    char             *spot_lon;
    char             *tz_str;
    char             *tz_display_name;
    spot_check_mode_t operating_mode;
    char             *custom_screen_url;
    uint32_t          custom_update_interval_secs;
    screen_img_t      active_chart_1;
    screen_img_t      active_chart_2;
} spot_check_config_t;

void                 nvs_init();
void                 nvs_start();
bool                 nvs_get_uint32(char *key, uint32_t *val, uint32_t fallback);
bool                 nvs_set_uint32(char *key, uint32_t val);
bool                 nvs_get_int8(char *key, int8_t *val, int8_t fallback);
bool                 nvs_set_int8(char *key, int8_t val);
bool                 nvs_get_string(char *key, char *val, size_t *val_size, char *fallback);
bool                 nvs_set_string(char *key, char *val);
void                 nvs_save_config(spot_check_config_t *config);
void                 nvs_print_config(log_level_t level);
esp_err_t            nvs_full_erase();
spot_check_config_t *nvs_get_config();
bool                 nvs_chart_string_to_enum(char *chart_str_in, screen_img_t *enum_out);
void                 nvs_chart_enum_to_string(screen_img_t enum_in, char *chart_str_out);

#endif
