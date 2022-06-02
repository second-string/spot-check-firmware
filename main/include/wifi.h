#ifndef WIFI_H
#define WIFI_H

#include "freertos/event_groups.h"

#define PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY 3

typedef struct {
    char  *ssid;
    char  *password;
    size_t ssid_len;
    size_t password_len;
} connect_to_network_task_args;

/* Base init that needs to be done on boot no matter what mode we're headed for */
void wifi_init();

/* Uses an event group internally to yield until the wifi task sets the connected bit */
void wifi_block_until_connected();

/* Used by tasks to check if connected yet in order to perform logic without having to block on event group */
bool wifi_is_network_connected();

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

/* Check if we're provisioned */
bool wifi_is_provisioned();

#endif
