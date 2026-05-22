#ifndef HID_TRANSPORT_H
#define HID_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

int hid_transport_init(enum app_mode initial_mode);
void hid_transport_set_mode(enum app_mode mode);
void hid_transport_tick(void);
enum app_mode hid_transport_get_mode(void);
bool hid_transport_ble_ready(void);
bool hid_transport_connected(void);
int hid_transport_release_all(void);
int hid_transport_send_keyboard(const struct hid_keyboard_report *report);
int hid_transport_send_consumer(uint16_t usage);

#endif
