#include "constants.h"

#include "mdns.h"
#include "esp_log.h"

#include "mdns_local.h"

static char *hostname = "spot-check";

void mdns_init_local() {
    //initialize mDNS service
    ESP_ERROR_CHECK(mdns_init());

    //set hostname that ESP will be available under. Usually this would need to be
    // discovered by an external device by querying the _spot-check mDNS service
    mdns_hostname_set(hostname);

    // Instance name is the readable text supplied from the initial mDNS lookup of  _spot-check
    mdns_instance_name_set("Spot Check");

    ESP_LOGI(TAG, "mDNS initialized");
}

void mdns_advertise_tcp_service() {
    unsigned int port = 5207;
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_spot-check", "_tcp", port, NULL, 0));
    ESP_LOGI(TAG, "Advertising _spot-check mDNS service on port %d with hostname %s", port, hostname);
}
