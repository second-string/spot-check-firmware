#include "memfault_interface.h"

#include "memfault/components.h"
#include "memfault/http/http_client.h"

#include "log.h"

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

void memfault_interface_test_coredump_memory() {
    // memfault docs say to disable interrupts, couldn't get it not to crash idk
    // portMUX_TYPE mux;
    // taskENTER_CRITICAL(&mux);
    memfault_coredump_storage_debug_test_begin();
    // taskEXIT_CRITICAL(&mux);

    memfault_coredump_storage_debug_test_finish();
}
