#pragma once
#include <stdint.h>

enum net_state_t {
    NET_STATE_INIT,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_DISCONNECTED,
};

void net_init(void);
void net_tick(void);
net_state_t net_get_state(void);
const char* net_get_device_name(void);
const char* net_get_ip_address(void);
bool net_has_bonds(void);
void net_clear_bonds(void);
bool net_has_data(void);
const char* net_get_data(void);
void net_send_ack(void);
void net_send_nack(void);
void net_send_response(const char* response);
void net_request_refresh(void);

// Wi-Fi switching helpers
bool net_is_using_home(void);
void net_connect_home(void);
void net_connect_phone(void);
bool net_is_home_detected(void);
void net_clear_home_detected(void);
