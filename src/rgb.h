#ifndef RGB_H
#define RGB_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

struct rgb_host_config {
	bool enabled;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t brightness;
	uint8_t idle_brightness;
};

int rgb_init(void);
void rgb_show_status(enum app_mode mode, bool connected, bool numlock, bool idle);
void rgb_set_low_battery(bool low_battery);
void rgb_recover_after_power_event(void);
void rgb_set_host_config(const struct rgb_host_config *config);
void rgb_get_host_config(struct rgb_host_config *config);
void rgb_tick(void);
void rgb_off(void);

#endif
