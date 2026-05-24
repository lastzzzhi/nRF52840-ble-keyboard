#include "keymap.h"

#include <stddef.h>

#include <zephyr/usb/class/hid.h>

#define HID_KEY_KP_DOT 99
#define HID_KEY_NONE 0

static bool numlock_enabled = true;

static const uint8_t numeric_layer[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS] = {
	{ HID_KEY_ESC,      HID_KEY_NONE,       HID_KEY_NONE,       HID_KEY_NONE },
	{ HID_KEY_NUMLOCK,  HID_KEY_KPSLASH,    HID_KEY_KPASTERISK, HID_KEY_KPMINUS },
	{ HID_KEY_KP_7,     HID_KEY_KP_8,       HID_KEY_KP_9,       HID_KEY_KPPLUS },
	{ HID_KEY_KP_4,     HID_KEY_KP_5,       HID_KEY_KP_6,       HID_KEY_KPPLUS },
	{ HID_KEY_KP_1,     HID_KEY_KP_2,       HID_KEY_KP_3,       HID_KEY_KPPLUS },
	{ HID_KEY_KP_0,     HID_KEY_KP_DOT,     HID_KEY_NONE,       HID_KEY_KPENTER },
};

static const uint8_t nav_layer[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS] = {
	{ HID_KEY_ESC,      HID_KEY_NONE,       HID_KEY_NONE,       HID_KEY_NONE },
	{ HID_KEY_NUMLOCK,  HID_KEY_KPSLASH,    HID_KEY_KPASTERISK, HID_KEY_KPMINUS },
	{ HID_KEY_HOME,     HID_KEY_UP,         HID_KEY_PAGEUP,     HID_KEY_KPPLUS },
	{ HID_KEY_LEFT,     HID_KEY_KP_5,       HID_KEY_RIGHT,      HID_KEY_KPPLUS },
	{ HID_KEY_END,      HID_KEY_DOWN,       HID_KEY_PAGEDOWN,   HID_KEY_KPPLUS },
	{ HID_KEY_INSERT,   HID_KEY_DELETE,     HID_KEY_NONE,       HID_KEY_KPENTER },
};

void keymap_init(void)
{
	numlock_enabled = true;
}

uint8_t keymap_usage_for_event(const struct keyboard_event *event)
{
	if ((event == NULL) || (event->row >= KEYBOARD_MATRIX_ROWS) ||
	    (event->col >= KEYBOARD_MATRIX_COLS)) {
		return HID_KEY_NONE;
	}

	if (event->row == 1 && event->col == 0) {
		return HID_KEY_NUMLOCK;
	}

	return numlock_enabled ? numeric_layer[event->row][event->col] :
				 nav_layer[event->row][event->col];
}

void keymap_note_event(const struct keyboard_event *event)
{
	if ((event != NULL) && event->pressed && event->row == 1 && event->col == 0) {
		numlock_enabled = !numlock_enabled;
	}
}

bool keymap_numlock_enabled(void)
{
	return numlock_enabled;
}

void keymap_set_numlock(bool enabled)
{
	numlock_enabled = enabled;
}

const char *keymap_layer_name(void)
{
	return numlock_enabled ? "NUM" : "NAV";
}
