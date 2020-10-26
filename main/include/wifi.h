#ifndef WIFI_H
#define WIFI_H

#include "freertos/event_groups.h"

 #define PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY  3

typedef struct {
    char *ssid;
    char *password;
    size_t ssid_len;
    size_t password_len;
} connect_to_network_task_args;

/* Base init that needs to be done on boot no matter what mode we're headed for */
void wifi_init(void *event_handler);


/*
 * Inits config and event handler for provisioning. Supports being called
 * more than once
 */
void wifi_init_provisioning();

/*
 * Kicks off provisioning mode. If we've previously provisioned and saved
 * connection info, it will deinit itself and switch over into STA mode to
 * connect to network. If we haven't it starts the cycle
 */
void wifi_start_provisioning(bool force_reprovision);

/* Deinits provisioning manager, unregisters from event handler, and sets internal flag */
void wifi_deinit_provisioning();

#endif
