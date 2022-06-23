#pragma once

#include "constants.h"
#include "esp_http_client.h"
#include "esp_partition.h"

const esp_partition_t *flash_partition_get_screen_img_partition();
