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

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[RGB_LED_COUNT];
static enum app_mode last_mode = APP_MODE_OFF;
static bool last_connected;
static bool last_numlock;
static bool last_state_valid;

int rgb_init(void)
{
	if (!device_is_ready(strip)) {
		LOG_WRN("LED strip is not ready");
		return -ENODEV;
	}

	rgb_off();
	return 0;
}

void rgb_show_status(enum app_mode mode, bool connected, bool numlock)
{
	struct led_rgb color = { 0 };

	if (!device_is_ready(strip)) {
		return;
	}

	if (last_state_valid && mode == last_mode &&
	    connected == last_connected && numlock == last_numlock) {
		return;
	}

	last_mode = mode;
	last_connected = connected;
	last_numlock = numlock;
	last_state_valid = true;

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
		rgb_off();
		return;
	}

	if (!numlock) {
		color.r = 24;
		color.g = 10;
		color.b = 0;
	}

	power_set_rgb_enabled(true);
	for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
		pixels[i] = color;
	}
	(void)led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
}

void rgb_off(void)
{
	last_state_valid = false;

	if (device_is_ready(strip)) {
		memset(pixels, 0, sizeof(pixels));
		(void)led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
	}
	power_set_rgb_enabled(false);
}
