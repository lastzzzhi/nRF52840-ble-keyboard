#include "display.h"

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_display, LOG_LEVEL_INF);

#define CTRL_NODE DT_NODELABEL(board_controls)

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec screen_bl =
	GPIO_DT_SPEC_GET(CTRL_NODE, screen_bl_gpios);
static bool display_ready;

static const char *mode_name(enum app_mode mode)
{
	switch (mode)
	{
	case APP_MODE_BLE:
		return "BLE";
	case APP_MODE_USB:
		return "USB";
	case APP_MODE_OFF:
		return "OFF";
	default:
		return "?";
	}
}

int app_display_init(void)
{
	if (gpio_is_ready_dt(&screen_bl))
	{
		(void)gpio_pin_configure_dt(&screen_bl, GPIO_OUTPUT_ACTIVE);
	}

	if (!device_is_ready(display_dev))
	{
		LOG_WRN("Display is not ready");
		return -ENODEV;
	}

	if (display_set_pixel_format(display_dev, PIXEL_FORMAT_MONO10) != 0 &&
		display_set_pixel_format(display_dev, PIXEL_FORMAT_MONO01) != 0)
	{
		LOG_WRN("Display pixel format not supported");
		return -ENOTSUP;
	}

	if (cfb_framebuffer_init(display_dev) != 0)
	{
		LOG_WRN("Display framebuffer init failed");
		return -EIO;
	}

	cfb_framebuffer_clear(display_dev, true);
	cfb_framebuffer_set_font(display_dev, 0);
	cfb_set_kerning(display_dev, 0);
	display_blanking_off(display_dev);
	display_ready = true;
	return 0;
}

void display_update_status(enum app_mode mode, int battery_percent,
						   bool connected, bool numlock)
{
	char line[32];

	if (!display_ready)
	{
		return;
	}

	cfb_framebuffer_clear(display_dev, false);
	snprintf(line, sizeof(line), "AI BLE KEYBOARD");
	(void)cfb_print(display_dev, line, 0, 0);

	snprintf(line, sizeof(line), "MODE:%s %s", mode_name(mode),
			 connected ? "LINK" : "----");
	(void)cfb_print(display_dev, line, 0, 16);

	if (battery_percent >= 0)
	{
		snprintf(line, sizeof(line), "BAT:%3d%%", battery_percent);
	}
	else
	{
		snprintf(line, sizeof(line), "BAT:---%%");
	}
	(void)cfb_print(display_dev, line, 0, 32);

	snprintf(line, sizeof(line), "LAYER:%s", numlock ? "NUM" : "NAV");
	(void)cfb_print(display_dev, line, 0, 48);

	cfb_framebuffer_finalize(display_dev);
}
