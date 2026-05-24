#ifndef RGB_H
#define RGB_H

#include <stdbool.h>

#include "app_types.h"

int rgb_init(void);
void rgb_show_status(enum app_mode mode, bool connected, bool numlock, bool idle);
void rgb_set_low_battery(bool low_battery);
void rgb_tick(void);
void rgb_off(void);

#endif
