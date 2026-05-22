#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

int power_init(void);
int power_get_battery_percent(void);
int power_get_battery_mv(void);
void power_ip5306_keepalive_tick(bool enabled);
void power_set_rgb_enabled(bool enabled);
bool power_ip5306_read_reg(uint8_t reg, uint8_t *value);

#endif
