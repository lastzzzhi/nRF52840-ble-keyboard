#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

int app_display_init(void);
void display_set_boot_info(uint32_t boot_count, uint32_t reset_cause);
void display_update_status(enum app_mode mode, int battery_percent,
			   bool connected, bool numlock);
void app_display_tick(void);

#endif
