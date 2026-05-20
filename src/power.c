#include "power.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define CTRL_NODE DT_NODELABEL(board_controls)
#define USER_NODE DT_PATH(zephyr_user)
#define IP5306_I2C_ADDR 0x75
#define BATTERY_DIVIDER_NUM 780
#define BATTERY_DIVIDER_DEN 100
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

static const struct gpio_dt_spec bat_adc_en =
	GPIO_DT_SPEC_GET(CTRL_NODE, battery_adc_enable_gpios);
static const struct gpio_dt_spec rgb_power_en =
	GPIO_DT_SPEC_GET(CTRL_NODE, rgb_power_enable_gpios);
static const struct gpio_dt_spec ip5306_wakeup =
	GPIO_DT_SPEC_GET(CTRL_NODE, ip5306_wakeup_gpios);
static const struct adc_dt_spec bat_adc =
	ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 0);
static const struct device *const ip5306_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static int last_battery_mv = -1;
static int last_battery_percent = -1;

int power_init(void)
{
	int err;

	if (gpio_is_ready_dt(&bat_adc_en)) {
		err = gpio_pin_configure_dt(&bat_adc_en, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	if (gpio_is_ready_dt(&rgb_power_en)) {
		err = gpio_pin_configure_dt(&rgb_power_en, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	if (gpio_is_ready_dt(&ip5306_wakeup)) {
		err = gpio_pin_configure_dt(&ip5306_wakeup, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	if (!adc_is_ready_dt(&bat_adc)) {
		LOG_WRN("Battery ADC is not ready");
		return 0;
	}

	err = adc_channel_setup_dt(&bat_adc);
	if (err) {
		LOG_WRN("Battery ADC setup failed: %d", err);
	}

	if (!device_is_ready(ip5306_i2c)) {
		LOG_WRN("IP5306 I2C bus is not ready");
	}

	return 0;
}

int power_get_battery_mv(void)
{
	int16_t sample = 0;
	int32_t pin_mv;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (!adc_is_ready_dt(&bat_adc)) {
		return last_battery_mv;
	}

	(void)gpio_pin_set_dt(&bat_adc_en, 1);
	k_sleep(K_MSEC(2));

	if (adc_sequence_init_dt(&bat_adc, &seq) != 0) {
		(void)gpio_pin_set_dt(&bat_adc_en, 0);
		return last_battery_mv;
	}

	if (adc_read_dt(&bat_adc, &seq) != 0) {
		(void)gpio_pin_set_dt(&bat_adc_en, 0);
		return last_battery_mv;
	}

	(void)gpio_pin_set_dt(&bat_adc_en, 0);

	pin_mv = sample;
	if (adc_raw_to_millivolts_dt(&bat_adc, &pin_mv) != 0) {
		return last_battery_mv;
	}

	last_battery_mv = (pin_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
	return last_battery_mv;
}

int power_get_battery_percent(void)
{
	int mv = power_get_battery_mv();
	int percent;

	if (mv <= 0) {
		return last_battery_percent;
	}

	percent = ((mv - BATTERY_EMPTY_MV) * 100) /
		  (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
	if (percent < 0) {
		percent = 0;
	} else if (percent > 100) {
		percent = 100;
	}

	last_battery_percent = percent;
	return percent;
}

void power_set_rgb_enabled(bool enabled)
{
	if (gpio_is_ready_dt(&rgb_power_en)) {
		(void)gpio_pin_set_dt(&rgb_power_en, enabled ? 1 : 0);
	}
}

bool power_ip5306_read_reg(uint8_t reg, uint8_t *value)
{
	if ((value == NULL) || !device_is_ready(ip5306_i2c)) {
		return false;
	}

	return i2c_reg_read_byte(ip5306_i2c, IP5306_I2C_ADDR, reg, value) == 0;
}
