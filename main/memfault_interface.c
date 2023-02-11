#include "memfault_interface.h"

#include "memfault/components.h"
#include "memfault/http/http_client.h"

#include "log.h"

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
