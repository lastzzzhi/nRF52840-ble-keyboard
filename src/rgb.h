#ifndef RGB_H
#define RGB_H

#include <stdbool.h>

#include "app_types.h"

int rgb_init(void);
void rgb_show_status(enum app_mode mode, bool connected, bool numlock);
void rgb_off(void);

#endif
