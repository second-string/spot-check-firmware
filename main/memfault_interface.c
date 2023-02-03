#include "memfault_interface.h"

#include "log.h"
#include "memfault/http/http_client.h"

#define TAG SC_TAG_MFLT_INTRFC

bool memfault_interface_post_data() {
    sMfltHttpClient *http_client = memfault_http_client_create();
    if (!http_client) {
        log_printf(LOG_LEVEL_ERROR, "Failed to create memfault http_client");
        return false;
    }

    const eMfltPostDataStatus err = memfault_http_client_post_data(http_client);
    if (err < 0) {
        log_printf(LOG_LEVEL_ERROR, "%s error: %d", __func__, err);
    }

    const uint32_t timeout_ms = 30 * 1000;
    memfault_http_client_wait_until_requests_completed(http_client, timeout_ms);
    memfault_http_client_destroy(http_client);

    return err == 0;
}
