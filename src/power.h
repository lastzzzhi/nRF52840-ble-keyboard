#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

enum power_charge_state {
	POWER_CHARGE_UNKNOWN,
	POWER_CHARGE_DISCHARGING,
	POWER_CHARGE_CHARGING,
	POWER_CHARGE_FULL,
};

int power_init(void);
int power_get_battery_percent(void);
int power_get_battery_mv(void);
enum power_charge_state power_get_charge_state(void);
void power_set_idle(bool idle);
void power_ip5306_keepalive_tick(bool enabled);
void power_ip5306_keepalive_kick(void);
void power_set_rgb_enabled(bool enabled);
bool power_ip5306_read_reg(uint8_t reg, uint8_t *value);

#endif
