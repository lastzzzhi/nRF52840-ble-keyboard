#include "power.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define CTRL_NODE DT_NODELABEL(board_controls)
#define USER_NODE DT_PATH(zephyr_user)
#define IP5306_I2C_ADDR 0x75
#define IP5306_REG_SYS_0 0x70
#define IP5306_REG_SYS_1 0x71
#define IP5306_STATE_CHARGE BIT(3)
#define IP5306_STATE_FULL BIT(3)
#define BATTERY_DIVIDER_NUM 200
#define BATTERY_DIVIDER_DEN 100
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200
#define BATTERY_ADC_SETTLE_MS 50
#define BATTERY_ADC_SAMPLES 4
#define BATTERY_ADC_START_DELAY_MS 3000
/* Sample less often while idle; wakeup forces an immediate fresh sample. */
#define BATTERY_ADC_INTERVAL_MS 30000
#define BATTERY_ADC_IDLE_INTERVAL_MS 120000
#define BATTERY_ADC_RUNTIME_ENABLED 1
#define IP5306_KEEPALIVE_INTERVAL_MS 10000
#define IP5306_KEEPALIVE_PULSE_MS 80

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
static enum power_charge_state last_charge_state = POWER_CHARGE_UNKNOWN;
static int64_t next_battery_sample_ms;
static int64_t next_keepalive_ms;
static int64_t keepalive_release_ms;
static bool keepalive_active;
static bool power_idle;

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
#if !BATTERY_ADC_RUNTIME_ENABLED
	return last_battery_mv;
#else
	int64_t now = k_uptime_get();
	int16_t samples[BATTERY_ADC_SAMPLES];
	int32_t sample_sum = 0;
	int32_t pin_mv;
	struct adc_sequence_options options = {
		.extra_samplings = BATTERY_ADC_SAMPLES - 1,
	};
	struct adc_sequence seq = {
		.options = &options,
		.buffer = samples,
		.buffer_size = sizeof(samples),
	};
	int err;

	if (next_battery_sample_ms == 0) {
		next_battery_sample_ms = now + BATTERY_ADC_START_DELAY_MS;
		return last_battery_mv;
	}

	if (now < next_battery_sample_ms) {
		return last_battery_mv;
	}

	next_battery_sample_ms = now + (power_idle ? BATTERY_ADC_IDLE_INTERVAL_MS :
					BATTERY_ADC_INTERVAL_MS);

	if (!adc_is_ready_dt(&bat_adc)) {
		return last_battery_mv;
	}

	(void)gpio_pin_set_dt(&bat_adc_en, 1);
	k_sleep(K_MSEC(BATTERY_ADC_SETTLE_MS));

	err = adc_sequence_init_dt(&bat_adc, &seq);
	if (err) {
		(void)gpio_pin_set_dt(&bat_adc_en, 0);
		LOG_WRN("Battery ADC sequence init failed: %d", err);
		return last_battery_mv;
	}

	err = adc_read_dt(&bat_adc, &seq);
	if (err) {
		(void)gpio_pin_set_dt(&bat_adc_en, 0);
		LOG_WRN("Battery ADC read failed: %d", err);
		return last_battery_mv;
	}

	(void)gpio_pin_set_dt(&bat_adc_en, 0);

	for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
		sample_sum += samples[i];
	}

	pin_mv = sample_sum / ARRAY_SIZE(samples);
	if (adc_raw_to_millivolts_dt(&bat_adc, &pin_mv) != 0) {
		return last_battery_mv;
	}

	last_battery_mv = (pin_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
	LOG_DBG("Battery raw avg %d pin %d mV pack %d mV",
		(int)(sample_sum / ARRAY_SIZE(samples)), (int)pin_mv, last_battery_mv);
	return last_battery_mv;
#endif
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
	return last_battery_percent;
}

void power_set_idle(bool idle)
{
	if (power_idle == idle) {
		return;
	}

	power_idle = idle;
	if (!idle) {
		next_battery_sample_ms = k_uptime_get();
	}
}

enum power_charge_state power_get_charge_state(void)
{
	uint8_t sys0;
	uint8_t sys1;

	if (!device_is_ready(ip5306_i2c)) {
		return last_charge_state;
	}

	if (i2c_reg_read_byte(ip5306_i2c, IP5306_I2C_ADDR,
			      IP5306_REG_SYS_1, &sys1) != 0) {
		return last_charge_state;
	}

	if ((sys1 & IP5306_STATE_FULL) != 0) {
		last_charge_state = POWER_CHARGE_FULL;
		return last_charge_state;
	}

	if (i2c_reg_read_byte(ip5306_i2c, IP5306_I2C_ADDR,
			      IP5306_REG_SYS_0, &sys0) != 0) {
		return last_charge_state;
	}

	last_charge_state = ((sys0 & IP5306_STATE_CHARGE) != 0) ?
			    POWER_CHARGE_CHARGING : POWER_CHARGE_DISCHARGING;
	return last_charge_state;
}

void power_set_rgb_enabled(bool enabled)
{
	if (gpio_is_ready_dt(&rgb_power_en)) {
		(void)gpio_pin_set_dt(&rgb_power_en, enabled ? 1 : 0);
	}
}

void power_ip5306_keepalive_tick(bool enabled)
{
	int64_t now = k_uptime_get();

	if (!gpio_is_ready_dt(&ip5306_wakeup)) {
		return;
	}

	if (!enabled) {
		if (keepalive_active) {
			(void)gpio_pin_set_dt(&ip5306_wakeup, 0);
			keepalive_active = false;
		}
		next_keepalive_ms = now + IP5306_KEEPALIVE_INTERVAL_MS;
		return;
	}

	if (keepalive_active) {
		if (now >= keepalive_release_ms) {
			(void)gpio_pin_set_dt(&ip5306_wakeup, 0);
			keepalive_active = false;
			next_keepalive_ms = now + IP5306_KEEPALIVE_INTERVAL_MS;
		}
		return;
	}

	if (next_keepalive_ms == 0) {
		next_keepalive_ms = now + IP5306_KEEPALIVE_INTERVAL_MS;
		return;
	}

	if (now >= next_keepalive_ms) {
		(void)gpio_pin_set_dt(&ip5306_wakeup, 1);
		keepalive_active = true;
		keepalive_release_ms = now + IP5306_KEEPALIVE_PULSE_MS;
	}
}

void power_ip5306_keepalive_kick(void)
{
	int64_t now = k_uptime_get();

	if (!gpio_is_ready_dt(&ip5306_wakeup)) {
		return;
	}

	(void)gpio_pin_set_dt(&ip5306_wakeup, 1);
	keepalive_active = true;
	keepalive_release_ms = now + IP5306_KEEPALIVE_PULSE_MS;
	next_keepalive_ms = keepalive_release_ms + IP5306_KEEPALIVE_INTERVAL_MS;
}

bool power_ip5306_read_reg(uint8_t reg, uint8_t *value)
{
	if ((value == NULL) || !device_is_ready(ip5306_i2c)) {
		return false;
	}

	return i2c_reg_read_byte(ip5306_i2c, IP5306_I2C_ADDR, reg, value) == 0;
}
