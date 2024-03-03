#ifndef SCHEDULER_TASK_H
#define SCHEDULER_TASK_H

#include <stdint.h>

typedef enum {
    SCHEDULER_MODE_INIT,
    SCHEDULER_MODE_OFFLINE,
    SCHEDULER_MODE_ONLINE,
    SCHEDULER_MODE_OTA,
} scheduler_mode_t;

void             scheduler_trigger();
void             scheduler_schedule_network_check();
void             scheduler_schedule_time_update();
void             scheduler_schedule_date_update();
void             scheduler_schedule_spot_name_update();
void             scheduler_schedule_conditions_update();
void             scheduler_schedule_tide_chart_update();
void             scheduler_schedule_swell_chart_update();
void             scheduler_schedule_wind_chart_update();
void             scheduler_schedule_both_charts_update();
void             scheduler_schedule_ota_check();
void             scheduler_schedule_mflt_upload();
void             scheduler_schedule_screen_dirty();
void             scheduler_schedule_custom_screen_update();
void             scheduler_block_until_system_idle();
void             scheduler_set_busy(uint32_t system_idle_bitmask);
void             scheduler_set_idle(uint32_t system_idle_bitmask);
void             scheduler_set_offline_mode();
void             scheduler_set_ota_mode();
void             scheduler_set_online_mode();
scheduler_mode_t scheduler_get_mode();
UBaseType_t      scheduler_task_get_stack_high_water();
void             scheduler_task_init();
void             scheduler_task_start();

#endif
