#ifndef NVS_H
#define NVS_H

#include <stdbool.h>

#include "esp_err.h"

#include "http_server.h"

// number_of_days stored as string for bundling it into a queryparam
// later w/o having to alloc and pass around a new buff. Easier to go
// str -> int vs. other direction
typedef struct {
    char *spot_name;
    char *spot_uid;
    char *spot_lat;
    char *spot_lon;
    char *tz_str;
} spot_check_config;

void               nvs_init();
bool               nvs_get_uint32(char *key, uint32_t *val);
bool               nvs_set_uint32(char *key, uint32_t val);
bool               nvs_get_int8(char *key, int8_t *val);
bool               nvs_set_int8(char *key, int8_t val);
void               nvs_save_config(spot_check_config *config);
esp_err_t          nvs_full_erase();
spot_check_config *nvs_get_config();

#endif
