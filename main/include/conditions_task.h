#ifndef CONDITIONS_TASK_H
#define CONDITIONS_TASK_H

#include <stdint.h>

typedef struct {
    int8_t  temperature;
    uint8_t wind_speed;
    char    wind_dir[4];     // 3 characters for dir (SSW, etc) plus null
    char    tide_height[7];  // minus sign, two digits, decimal point, two digits, null
} conditions_t;

void conditions_trigger_time_update();
void conditions_trigger_spot_name_update();
void conditions_trigger_conditions_update();
void conditions_trigger_tide_chart_update();
void conditions_trigger_swell_chart_update();
void conditions_trigger_both_charts_update();
void conditions_block_until_system_idle();
void conditions_set_busy(uint32_t system_idle_bitmask);
void conditions_set_idle(uint32_t system_idle_bitmask);
void conditions_update_task_init();
void conditions_update_task_start();

#endif
