#include "display.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_display, LOG_LEVEL_INF);

#define CTRL_NODE DT_NODELABEL(board_controls)
#define BATTERY_FILL_MAX_WIDTH 116
#define SCREEN_W 320
#define SCREEN_H 240
#define VISIBLE_Y 34

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec screen_bl =
	GPIO_DT_SPEC_GET(CTRL_NODE, screen_bl_gpios);

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *mode_label;
static lv_obj_t *link_label;
static lv_obj_t *battery_label;
static lv_obj_t *battery_box;
static lv_obj_t *battery_fill;
static lv_obj_t *charge_label;
static lv_obj_t *layer_label;
static lv_obj_t *status_label;
static lv_obj_t *mode_status_label;
static lv_obj_t *mode_panel;
static lv_obj_t *battery_panel;
static lv_obj_t *link_panel;
static lv_obj_t *layer_panel;
static lv_obj_t *battery_tip;
static lv_style_t style_screen;
static lv_style_t style_text;
static lv_style_t style_muted;
static lv_style_t style_big;
static lv_style_t style_panel;
static lv_style_t style_panel_accent;
static lv_style_t style_box;
static lv_style_t style_fill;
static lv_style_t style_icon;
static bool display_ready;
static bool display_idle;
static int64_t build_epoch_seconds;
static int last_display_mode = -1;
static int last_display_battery = -999;
static int last_display_battery_mv = -999;
static int last_display_hour = -1;
static int last_display_minute = -1;
static int last_display_second = -1;
static int last_display_day = -1;
static bool last_display_connected;
static bool last_display_numlock;

static void screen_backlight_probe(void)
{
	if (!gpio_is_ready_dt(&screen_bl)) {
		LOG_WRN("Screen BL GPIO is not ready");
		return;
	}

	(void)gpio_pin_configure_dt(&screen_bl, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_set_dt(&screen_bl, 1);
	display_idle = false;
}

static void screen_backlight_set(bool on)
{
	if (gpio_is_ready_dt(&screen_bl)) {
		(void)gpio_pin_set_dt(&screen_bl, on ? 1 : 0);
	}
}

static const char *mode_name(enum app_mode mode)
{
	switch (mode) {
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

static int month_from_name(const char *name)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *pos = strstr(months, name);

	if (pos == NULL) {
		return 1;
	}

	return ((int)(pos - months) / 3) + 1;
}

static bool is_leap(int year)
{
	return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static int days_in_month(int year, int month)
{
	static const uint8_t days[] = {
		31, 28, 31, 30, 31, 30,
		31, 31, 30, 31, 30, 31,
	};

	if (month == 2 && is_leap(year)) {
		return 29;
	}

	return days[month - 1];
}

static int64_t date_to_epoch_seconds(int year, int month, int day,
				     int hour, int minute, int second)
{
	int64_t days = 0;

	for (int y = 1970; y < year; y++) {
		days += is_leap(y) ? 366 : 365;
	}

	for (int m = 1; m < month; m++) {
		days += days_in_month(year, m);
	}

	days += day - 1;
	return (((days * 24) + hour) * 60 + minute) * 60 + second;
}

static void epoch_to_date(int64_t seconds, int *year, int *month, int *day,
			  int *hour, int *minute, int *second)
{
	int64_t days = seconds / 86400;
	int rem = (int)(seconds % 86400);
	int y = 1970;
	int m = 1;

	while (true) {
		int year_days = is_leap(y) ? 366 : 365;

		if (days < year_days) {
			break;
		}
		days -= year_days;
		y++;
	}

	while (true) {
		int month_days = days_in_month(y, m);

		if (days < month_days) {
			break;
		}
		days -= month_days;
		m++;
	}

	*year = y;
	*month = m;
	*day = (int)days + 1;
	*hour = rem / 3600;
	*minute = (rem % 3600) / 60;
	*second = rem % 60;
}

static int64_t build_time_seconds(void)
{
	char month_name[4] = { 0 };
	int year;
	int day;
	int hour;
	int minute;
	int second;

	if (sscanf(__DATE__, "%3s %d %d", month_name, &day, &year) != 3 ||
	    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
		return date_to_epoch_seconds(2026, 1, 1, 0, 0, 0);
	}

	return date_to_epoch_seconds(year, month_from_name(month_name), day,
				     hour, minute, second);
}

static void init_styles(void)
{
	lv_style_init(&style_screen);
	lv_style_set_bg_color(&style_screen, lv_color_hex(0x05070b));
	lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
	lv_style_set_pad_all(&style_screen, 0);

	lv_style_init(&style_text);
	lv_style_set_text_color(&style_text, lv_color_hex(0xe8eef7));
	lv_style_set_bg_opa(&style_text, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_text, 0);
	lv_style_set_text_font(&style_text, &lv_font_montserrat_14);

	lv_style_init(&style_muted);
	lv_style_set_text_color(&style_muted, lv_color_hex(0x7f8ca3));
	lv_style_set_bg_opa(&style_muted, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_muted, 0);
	lv_style_set_text_font(&style_muted, &lv_font_montserrat_14);

	lv_style_init(&style_big);
	lv_style_set_text_color(&style_big, lv_color_hex(0xffffff));
	lv_style_set_bg_opa(&style_big, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_big, 0);
	lv_style_set_text_font(&style_big, &lv_font_montserrat_14);

	lv_style_init(&style_panel);
	lv_style_set_bg_color(&style_panel, lv_color_hex(0x101722));
	lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
	lv_style_set_border_color(&style_panel, lv_color_hex(0x243247));
	lv_style_set_border_width(&style_panel, 1);
	lv_style_set_radius(&style_panel, 6);
	lv_style_set_pad_all(&style_panel, 0);

	lv_style_init(&style_panel_accent);
	lv_style_set_border_color(&style_panel_accent, lv_color_hex(0x39a7ff));
	lv_style_set_border_width(&style_panel_accent, 2);

	lv_style_init(&style_box);
	lv_style_set_bg_color(&style_box, lv_color_hex(0x05070b));
	lv_style_set_bg_opa(&style_box, LV_OPA_COVER);
	lv_style_set_border_color(&style_box, lv_color_hex(0xb9c7dc));
	lv_style_set_border_width(&style_box, 2);
	lv_style_set_radius(&style_box, 3);
	lv_style_set_pad_all(&style_box, 0);

	lv_style_init(&style_fill);
	lv_style_set_bg_color(&style_fill, lv_color_hex(0x32d583));
	lv_style_set_bg_opa(&style_fill, LV_OPA_COVER);
	lv_style_set_border_width(&style_fill, 0);
	lv_style_set_radius(&style_fill, 2);
	lv_style_set_pad_all(&style_fill, 0);

	lv_style_init(&style_icon);
	lv_style_set_bg_color(&style_icon, lv_color_hex(0x182333));
	lv_style_set_bg_opa(&style_icon, LV_OPA_COVER);
	lv_style_set_border_color(&style_icon, lv_color_hex(0x39a7ff));
	lv_style_set_border_width(&style_icon, 1);
	lv_style_set_radius(&style_icon, 14);
	lv_style_set_pad_all(&style_icon, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w,
			    lv_style_t *style)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_add_style(label, style, 0);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_width(label, w);
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
	return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *panel = lv_obj_create(parent);

	lv_obj_remove_style_all(panel);
	lv_obj_add_style(panel, &style_panel, 0);
	lv_obj_set_pos(panel, x, y);
	lv_obj_set_size(panel, w, h);
	return panel;
}

int app_display_init(void)
{
	lv_obj_t *screen;
	int err;

	screen_backlight_probe();

	if (!device_is_ready(display_dev)) {
		LOG_WRN("Display is not ready");
		return -ENODEV;
	}

	err = display_set_pixel_format(display_dev, PIXEL_FORMAT_RGB_565);
	if (err) {
		LOG_WRN("Set RGB565 pixel format failed: %d", err);
	}

	err = display_blanking_off(display_dev);
	if (err) {
		LOG_WRN("Display blanking off failed: %d", err);
	}

	init_styles();
	screen = lv_screen_active();
	lv_obj_remove_style_all(screen);
	lv_obj_add_style(screen, &style_screen, 0);
	lv_obj_set_size(screen, SCREEN_W, SCREEN_H);

	time_label = make_label(screen, 12, VISIBLE_Y + 4, 112, &style_big);
	date_label = make_label(screen, 12, VISIBLE_Y + 26, 118, &style_muted);

	link_panel = make_panel(screen, 182, VISIBLE_Y + 4, 126, 42);
	link_label = make_label(link_panel, 8, 12, 110, &style_text);
	lv_obj_set_style_text_align(link_label, LV_TEXT_ALIGN_RIGHT, 0);

	mode_panel = make_panel(screen, 12, VISIBLE_Y + 60, 136, 68);
	lv_obj_add_style(mode_panel, &style_panel_accent, 0);
	mode_label = make_label(mode_panel, 12, 10, 112, &style_big);
	lv_obj_set_style_text_align(mode_label, LV_TEXT_ALIGN_CENTER, 0);
	mode_status_label = make_label(mode_panel, 12, 40, 112, &style_muted);
	lv_obj_set_style_text_align(mode_status_label, LV_TEXT_ALIGN_CENTER, 0);

	battery_panel = make_panel(screen, 164, VISIBLE_Y + 60, 144, 68);
	battery_label = make_label(battery_panel, 10, 6, 122, &style_text);
	battery_box = lv_obj_create(battery_panel);
	lv_obj_remove_style_all(battery_box);
	lv_obj_add_style(battery_box, &style_box, 0);
	lv_obj_set_pos(battery_box, 10, 30);
	lv_obj_set_size(battery_box, 124, 18);

	battery_fill = lv_obj_create(battery_panel);
	lv_obj_remove_style_all(battery_fill);
	lv_obj_add_style(battery_fill, &style_fill, 0);
	lv_obj_set_pos(battery_fill, 14, 34);
	lv_obj_set_size(battery_fill, 1, 10);

	battery_tip = lv_obj_create(battery_panel);
	lv_obj_remove_style_all(battery_tip);
	lv_obj_add_style(battery_tip, &style_box, 0);
	lv_obj_set_pos(battery_tip, 134, 34);
	lv_obj_set_size(battery_tip, 4, 10);

	charge_label = make_label(battery_panel, 10, 50, 122, &style_muted);
	lv_obj_set_style_text_align(charge_label, LV_TEXT_ALIGN_RIGHT, 0);

	layer_panel = make_panel(screen, 12, VISIBLE_Y + 140, 296, 32);
	layer_label = make_label(layer_panel, 10, 6, 48, &style_big);

	status_label = make_label(layer_panel, 72, 6, 204, &style_muted);

	build_epoch_seconds = build_time_seconds();
	lv_timer_handler();
	display_ready = true;
	display_update_status(APP_MODE_BLE, -1, -1, false, true);
	return 0;
}

void display_update_status(enum app_mode mode, int battery_percent,
			   int battery_mv, bool connected, bool numlock)
{
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int fill_width;
	int64_t now;
	bool time_changed;
	bool mode_changed;
	bool battery_changed;
	bool numlock_changed;

	if (!display_ready) {
		return;
	}

	if (battery_percent > 100) {
		battery_percent = 100;
	} else if (battery_percent < 0) {
		battery_percent = -1;
	}
	if (battery_mv < 0) {
		battery_mv = -1;
	}

	now = build_epoch_seconds + (k_uptime_get() / 1000);
	epoch_to_date(now, &year, &month, &day, &hour, &minute, &second);
	time_changed = hour != last_display_hour ||
		       minute != last_display_minute ||
		       second != last_display_second ||
		       day != last_display_day;
	mode_changed = mode != last_display_mode ||
		       connected != last_display_connected;
	battery_changed = battery_percent != last_display_battery ||
			  battery_mv != last_display_battery_mv;
	numlock_changed = numlock != last_display_numlock;

	if (!time_changed && !mode_changed && !battery_changed &&
	    !numlock_changed) {
		return;
	}

	if (time_changed) {
		lv_label_set_text_fmt(time_label, "%02d:%02d:%02d",
				      hour, minute, second);
		lv_label_set_text_fmt(date_label, "%04d-%02d-%02d",
				      year, month, day);
		last_display_hour = hour;
		last_display_minute = minute;
		last_display_second = second;
		last_display_day = day;
	}

	if (mode_changed) {
		lv_label_set_text(mode_label, mode_name(mode));
		if (mode == APP_MODE_BLE) {
			lv_label_set_text(link_label,
					  connected ? "BT LINK" : "BT SCAN");
			lv_label_set_text(mode_status_label,
					  connected ? "CONNECTED" : "SEARCHING");
			lv_label_set_text(status_label,
					  connected ? "BLE ACTIVE" : "BLE IDLE");
		} else if (mode == APP_MODE_USB) {
			lv_label_set_text(link_label,
					  connected ? "USB LINK" : "USB WAIT");
			lv_label_set_text(mode_status_label,
					  connected ? "CONNECTED" : "WAITING");
			lv_label_set_text(status_label,
					  connected ? "USB HID" : "USB IDLE");
		} else {
			lv_label_set_text(link_label, "OFF");
			lv_label_set_text(mode_status_label, "SLEEP");
			lv_label_set_text(status_label, "POWER OFF");
		}
		last_display_mode = mode;
		last_display_connected = connected;
	}

	if (battery_changed || mode_changed) {
		if (battery_percent >= 0) {
			lv_label_set_text_fmt(battery_label, "BAT %d%%",
					      battery_percent);
			fill_width = (battery_percent * BATTERY_FILL_MAX_WIDTH) / 100;
			if (fill_width < 1 && battery_percent > 0) {
				fill_width = 1;
			}
		} else {
			lv_label_set_text(battery_label, "BAT ---");
			fill_width = 1;
		}
		lv_obj_set_size(battery_fill, fill_width, 10);
		if (battery_percent >= 60) {
			lv_obj_set_style_bg_color(battery_fill,
						  lv_color_hex(0x32d583), 0);
		} else if (battery_percent >= 25) {
			lv_obj_set_style_bg_color(battery_fill,
						  lv_color_hex(0xfdb022), 0);
		} else {
			lv_obj_set_style_bg_color(battery_fill,
						  lv_color_hex(0xf04438), 0);
		}

		if (battery_mv >= 0) {
			lv_label_set_text_fmt(charge_label, "%d.%02dV",
					      battery_mv / 1000,
					      (battery_mv % 1000) / 10);
		} else {
			lv_label_set_text(charge_label, "--.--V");
		}
		last_display_battery = battery_percent;
		last_display_battery_mv = battery_mv;
	}

	if (numlock_changed) {
		lv_label_set_text(layer_label, numlock ? "123" : "NAV");
		last_display_numlock = numlock;
	}
	lv_timer_handler();
}

void app_display_tick(void)
{
	if (display_ready) {
		lv_timer_handler();
	}
}

void app_display_set_idle(bool idle)
{
	if (display_idle == idle) {
		return;
	}

	display_idle = idle;
	screen_backlight_set(!idle);
}
