#include "constants.h"

#include "log.h"
#include "mdns.h"

#include "mdns_local.h"

#define TAG "sc-mdns"

static char *hostname = "spot-check";

void mdns_local_init() {
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
    unsigned int port = 5207;
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_spot-check", "_tcp", port, NULL, 0));
    log_printf(LOG_LEVEL_INFO, "Advertising _spot-check mDNS service on port %d with hostname %s", port, hostname);
}
