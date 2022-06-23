#ifndef CONDITIONS_TASK_H
#define CONDITIONS_TASK_H

#include <stdint.h>

typedef struct {
    int8_t  temperature;
    uint8_t wind_speed;
    char    wind_dir[4];     // 3 characters for dir (SSW, etc) plus null
    char    tide_height[7];  // minus sign, two digits, decimal point, two digits, null
} conditions_t;

void conditions_trigger_conditions_update();
void conditions_trigger_tide_chart_update();
void conditions_trigger_swell_chart_update();
void conditions_trigger_both_charts_update();
void conditions_display(conditions_t *conditions);
void conditions_display_last_retrieved();
void conditions_update_task_start();

#endif
