#include "memfault_interface.h"

#include "memfault/components.h"
#include "memfault/http/http_client.h"

#include "cli_task.h"
#include "log.h"
#include "ota_task.h"
#include "scheduler_task.h"

#define TAG SC_TAG_MFLT_INTRFC

#define UPLOAD_TIMEOUT_MS (30 * MS_PER_SEC)

/*
 * Returns success
 */
bool memfault_interface_post_data() {
    log_printf(LOG_LEVEL_INFO, "Executing memfault upload function");

    sMfltHttpClient *http_client = memfault_http_client_create();
    if (!http_client) {
        log_printf(LOG_LEVEL_ERROR, "Failed to create memfault http_client");
        return false;
    }

    bool success = false;
    int  err     = 0;
    do {
        eMfltPostDataStatus post_err = memfault_http_client_post_data(http_client);
        success = (post_err == kMfltPostDataStatus_NoDataFound || post_err == kMfltPostDataStatus_Success);
        if (post_err == kMfltPostDataStatus_NoDataFound) {
            log_printf(LOG_LEVEL_DEBUG, "No heartbeat or coredump data to upload.");
            break;
        } else if (post_err < 0) {
            log_printf(LOG_LEVEL_ERROR, "Error in initial call to memfault post data func: %d", post_err);
            break;
        }

        err     = memfault_http_client_wait_until_requests_completed(http_client, UPLOAD_TIMEOUT_MS);
        success = (err == 0);
        if (success) {
            log_printf(LOG_LEVEL_INFO, "Successfully uploaded all available data to memfault");
        } else {
            log_printf(LOG_LEVEL_ERROR, "Error waiting until mflt http req completed: %d", err);
        }
    } while (0);

    err     = memfault_http_client_destroy(http_client);
    success = (err == 0);
    if (!success) {
        log_printf(LOG_LEVEL_ERROR, "Error tearing down memfault http client: %d", err);
    }

    return success;
}

/*
 * Memfault weak function, override here to bundle custom metrics every time heartbeat elapsed and data sent
 */
void memfault_metrics_heartbeat_collect_data() {
    size_t      total                 = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t      free                  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t      min_free              = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t      largest_block         = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    UBaseType_t cli_total_words       = cli_task_get_stack_high_water();
    UBaseType_t ota_total_words       = ota_task_get_stack_high_water();
    UBaseType_t scheduler_total_words = scheduler_task_get_stack_high_water();

    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(total_heap_bytes), total);
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(free_heap_bytes), free);
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(low_watermark_heap_bytes), min_free);
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(largest_free_heap_block_bytes), largest_block);
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(cli_task_high_water_stack_bytes),
                                            cli_total_words * sizeof(uint32_t));
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(ota_task_high_water_stack_bytes),
                                            ota_total_words * sizeof(uint32_t));
    memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(scheduler_task_high_water_stack_bytes),
                                            scheduler_total_words * sizeof(uint32_t));
}
