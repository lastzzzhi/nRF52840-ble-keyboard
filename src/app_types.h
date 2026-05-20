#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdbool.h>
#include <stdint.h>

enum app_mode {
	APP_MODE_BLE = 0,
	APP_MODE_USB,
	APP_MODE_OFF,
};

#define HID_KEYBOARD_REPORT_SIZE 8
#define HID_KEYBOARD_MAX_KEYS 6
#define HID_CONSUMER_MUTE 0x00e2
#define HID_CONSUMER_VOLUME_UP 0x00e9
#define HID_CONSUMER_VOLUME_DOWN 0x00ea

struct hid_keyboard_report {
	uint8_t modifiers;
	uint8_t reserved;
	uint8_t keys[HID_KEYBOARD_MAX_KEYS];
};

#endif
