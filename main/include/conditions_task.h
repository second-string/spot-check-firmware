#ifndef CONDITIONS_TASK_H
#define CONDITIONS_TASK_H

#include <stdint.h>

typedef struct {
    int8_t  temperature;
    uint8_t wind_speed;
    char    wind_dir[4];     // 3 characters for dir (SSW, etc) plus null
    char    tide_height[7];  // minus sign, two digits, decimal point, two digits, null
} conditions_t;

void update_conditions_task(void *args);
void display_conditions(conditions_t *conditions);
void display_last_retrieved_conditions();

#endif
