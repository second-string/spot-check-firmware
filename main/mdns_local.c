#include "log.h"
#include "mdns.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "mdns_local.h"

#define TAG SC_TAG_MDNS

static char *hostname = "spot-check";

static bool mdns_advertising = false;

void mdns_local_init() {
    MEMFAULT_ASSERT(!mdns_advertising);

    // initialize mDNS service
    ESP_ERROR_CHECK(mdns_init());

    // set hostname that ESP will be available under. Usually this would need to be
    // discovered by an external device by querying the _spot-check mDNS service
    mdns_hostname_set(hostname);

    // Instance name is the readable text supplied from the initial mDNS lookup of  _spot-check
    mdns_instance_name_set("Spot Check");

    log_printf(LOG_LEVEL_INFO, "mDNS initialized");
}

void mdns_advertise_tcp_service() {
    if (mdns_advertising) {
        log_printf(LOG_LEVEL_INFO,
                   "mdns_advertise_tcp_service re-called after reconnection to wifi, skipping call to add new service "
                   "since previous should still exist");
    } else {
        unsigned int port = 5207;
        ESP_ERROR_CHECK(mdns_service_add(NULL, "_spot-check", "_tcp", port, NULL, 0));
        mdns_advertising = true;
        log_printf(LOG_LEVEL_INFO, "Advertising _spot-check mDNS service on port %d with hostname %s", port, hostname);
    }
}
