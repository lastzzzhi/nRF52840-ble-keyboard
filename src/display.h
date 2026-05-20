#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>

#include "app_types.h"

int app_display_init(void);
void display_update_status(enum app_mode mode, int battery_percent,
			   bool connected, bool numlock);

#endif
