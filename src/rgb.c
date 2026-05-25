#include "rgb.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "power.h"

LOG_MODULE_REGISTER(rgb, LOG_LEVEL_INF);

#define STRIP_NODE DT_ALIAS(led_strip)
#define RGB_LED_COUNT DT_PROP(STRIP_NODE, chain_length)
#define RGB_IDLE_REFRESH_MS 100
#define RGB_ACTIVE_REFRESH_MS 1000
#define RGB_RECOVERY_REFRESH_MS 100
#define RGB_RECOVERY_WINDOW_MS 2500
#define RGB_IDLE_BREATH_PERIOD_MS 2400
#define RGB_IDLE_MIN_SCALE 22
#define RGB_IDLE_MAX_SCALE 78
/* Below 20% battery, keep the same status colors but lower overall brightness. */
#define RGB_LOW_BATTERY_SCALE 55

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[RGB_LED_COUNT];
static enum app_mode last_mode = APP_MODE_OFF;
static bool last_connected;
static bool last_numlock;
static bool last_idle;
static bool low_battery;
static bool last_state_valid;
static int64_t next_refresh_ms;
static int64_t recovery_until_ms;
static struct rgb_host_config host_config = {
	.enabled = false,
	.r = 0,
	.g = 24,
	.b = 0,
	.brightness = 100,
	.idle_brightness = 40,
};

static uint8_t scale_channel(uint8_t value, uint8_t scale)
{
	return (uint8_t)(((uint16_t)value * scale) / 100U);
}

static uint8_t idle_breath_scale(int64_t now)
{
	int64_t phase = now % RGB_IDLE_BREATH_PERIOD_MS;
	int32_t span = RGB_IDLE_MAX_SCALE - RGB_IDLE_MIN_SCALE;
	int32_t level;

	if (phase < RGB_IDLE_BREATH_PERIOD_MS / 2) {
		level = (int32_t)((phase * span) / (RGB_IDLE_BREATH_PERIOD_MS / 2));
	} else {
		level = (int32_t)(((RGB_IDLE_BREATH_PERIOD_MS - phase) * span) /
				  (RGB_IDLE_BREATH_PERIOD_MS / 2));
	}

	return (uint8_t)(RGB_IDLE_MIN_SCALE + level);
}

static struct led_rgb status_color(enum app_mode mode, bool connected,
				   bool numlock)
{
	struct led_rgb color = { 0 };

	switch (mode) {
	case APP_MODE_BLE:
		color.b = connected ? 32 : 24;
		color.g = connected ? 12 : 0;
		break;
	case APP_MODE_USB:
		if (connected) {
			color.g = 24;
		} else {
			color.r = 24;
		}
		break;
	case APP_MODE_OFF:
	default:
		break;
	}

	if (!numlock) {
		color.r = 24;
		color.g = 10;
		color.b = 0;
	}

	return color;
}

static void rgb_render(int64_t now)
{
	struct led_rgb color;

	if (!device_is_ready(strip) || !last_state_valid) {
		return;
	}

	if (last_mode == APP_MODE_OFF) {
		rgb_off();
		return;
	}

	color = status_color(last_mode, last_connected, last_numlock);
	if (host_config.enabled) {
		color.r = host_config.r;
		color.g = host_config.g;
		color.b = host_config.b;
	}
	if (low_battery) {
		color.r = scale_channel(color.r, RGB_LOW_BATTERY_SCALE);
		color.g = scale_channel(color.g, RGB_LOW_BATTERY_SCALE);
		color.b = scale_channel(color.b, RGB_LOW_BATTERY_SCALE);
	}
	if (host_config.enabled) {
		uint8_t brightness = host_config.brightness;

		if (last_idle && host_config.idle_brightness < brightness) {
			brightness = host_config.idle_brightness;
		}
		color.r = scale_channel(color.r, brightness);
		color.g = scale_channel(color.g, brightness);
		color.b = scale_channel(color.b, brightness);
	}
	if (last_idle) {
		uint8_t scale = idle_breath_scale(now);

		color.r = scale_channel(color.r, scale);
		color.g = scale_channel(color.g, scale);
		color.b = scale_channel(color.b, scale);
	}

	power_set_rgb_enabled(true);
	for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
		pixels[i] = color;
	}
	(void)led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
	if (last_idle) {
		next_refresh_ms = now + RGB_IDLE_REFRESH_MS;
	} else if (now < recovery_until_ms) {
		next_refresh_ms = now + RGB_RECOVERY_REFRESH_MS;
	} else {
		next_refresh_ms = now + RGB_ACTIVE_REFRESH_MS;
	}
}

int rgb_init(void)
{
	if (!device_is_ready(strip)) {
		LOG_WRN("LED strip is not ready");
		return -ENODEV;
	}

	rgb_off();
	return 0;
}

void rgb_show_status(enum app_mode mode, bool connected, bool numlock, bool idle)
{
	int64_t now = k_uptime_get();

	if (!device_is_ready(strip)) {
		return;
	}

	if (last_state_valid && mode == last_mode &&
	    connected == last_connected && numlock == last_numlock &&
	    idle == last_idle && !idle) {
		return;
	}

	last_mode = mode;
	last_connected = connected;
	last_numlock = numlock;
	last_idle = idle;
	last_state_valid = true;

	if (mode == APP_MODE_OFF) {
		rgb_off();
		return;
	}

	LOG_INF("RGB status mode=%d connected=%d numlock=%d idle=%d",
		mode, connected, numlock, idle);
	rgb_render(now);
}

void rgb_set_low_battery(bool low_battery_enabled)
{
	if (low_battery == low_battery_enabled) {
		return;
	}

	low_battery = low_battery_enabled;
	rgb_render(k_uptime_get());
}

void rgb_set_host_config(const struct rgb_host_config *config)
{
	if (config == NULL) {
		return;
	}

	host_config = *config;
	if (host_config.brightness > 100) {
		host_config.brightness = 100;
	}
	if (host_config.idle_brightness > 100) {
		host_config.idle_brightness = 100;
	}
	rgb_render(k_uptime_get());
}

void rgb_get_host_config(struct rgb_host_config *config)
{
	if (config != NULL) {
		*config = host_config;
	}
}

void rgb_recover_after_power_event(void)
{
	int64_t now = k_uptime_get();

	recovery_until_ms = now + RGB_RECOVERY_WINDOW_MS;
	LOG_INF("RGB recovery after power event");
	rgb_render(now);
}

void rgb_tick(void)
{
	int64_t now;

	if (!last_state_valid) {
		return;
	}

	now = k_uptime_get();
	if (now < next_refresh_ms) {
		return;
	}

	rgb_render(now);
}

void rgb_off(void)
{
	last_state_valid = false;
	last_idle = false;
	next_refresh_ms = 0;
	recovery_until_ms = 0;

	if (device_is_ready(strip)) {
		memset(pixels, 0, sizeof(pixels));
		(void)led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
	}
	power_set_rgb_enabled(false);
}
