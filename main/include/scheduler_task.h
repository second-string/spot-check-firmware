#ifndef SCHEDULER_TASK_H
#define SCHEDULER_TASK_H

#include <stdint.h>

void scheduler_trigger_time_update();
void scheduler_trigger_spot_name_update();
void scheduler_trigger_conditions_update();
void scheduler_trigger_tide_chart_update();
void scheduler_trigger_swell_chart_update();
void scheduler_trigger_both_charts_update();
void scheduler_block_until_system_idle();
void scheduler_set_busy(uint32_t system_idle_bitmask);
void scheduler_set_idle(uint32_t system_idle_bitmask);
void scheduler_task_init();
void scheduler_task_start();

#endif
