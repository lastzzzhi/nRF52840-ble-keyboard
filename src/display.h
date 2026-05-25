#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "power.h"

int app_display_init(void);
void display_update_status(enum app_mode mode, int battery_percent,
			   int battery_mv, enum power_charge_state charge_state,
			   bool connected, bool numlock);
void display_set_host_time(uint32_t unix_time, int16_t timezone_offset_min,
			   uint8_t flags);
void app_display_tick(void);
void app_display_set_idle(bool idle);

#endif
