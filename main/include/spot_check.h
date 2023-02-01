#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int8_t  temperature;
    uint8_t wind_speed;
    char    wind_dir[4];     // 3 characters for dir (SSW, etc) plus null
    char    tide_height[7];  // minus sign, two digits, decimal point, two digits, null
} conditions_t;

bool spot_check_download_and_save_conditions(conditions_t *new_conditions);

void spot_check_clear_date(bool force_clear);
bool spot_check_draw_date();
void spot_check_clear_time();
bool spot_check_draw_time();
void spot_check_clear_spot_name();
bool spot_check_draw_spot_name(char *spot_name);
void spot_check_clear_conditions(bool clear_temperature, bool clear_wind, bool clear_tide);
bool spot_check_draw_conditions(conditions_t *conditions);
bool spot_check_draw_conditions_error();
bool spot_check_clear_ota_start_text();
bool spot_check_draw_ota_finished_text();
bool spot_check_draw_ota_start_text();
void spot_check_show_unprovisioned_screen();
void spot_check_show_no_network_screen();
void spot_check_clear_checking_connection_screen();
void spot_check_show_checking_connection_screen();
void spot_check_show_no_internet_screen();
void spot_check_draw_fetching_conditions_text();
void spot_check_draw_fetching_conditions_text();

void spot_check_init();
