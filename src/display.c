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
#define SCREEN_W 320
#define SCREEN_H 240
#define VISIBLE_Y 34
#define DISPLAY_IDLE_TICK_MS 1000
#define COLOR_BG 0x0b0f14
#define COLOR_CARD 0x111827
#define COLOR_BORDER 0x1f2937
#define COLOR_TEXT 0xeaf2ff
#define COLOR_MUTED 0x7c8a99
#define COLOR_USB 0x22c55e
#define COLOR_BLE 0x38bdf8
#define COLOR_LAYER 0xf59e0b
#define COLOR_LOW_BATTERY 0xef4444

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec screen_bl =
	GPIO_DT_SPEC_GET(CTRL_NODE, screen_bl_gpios);

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *mode_label;
static lv_obj_t *battery_label;
static lv_obj_t *layer_label;
static lv_obj_t *layer_value_label;
static lv_obj_t *mode_status_label;
static lv_obj_t *mode_panel;
static lv_obj_t *layer_panel;
static lv_obj_t *ble_label;
static lv_obj_t *battery_state_label;
static lv_style_t style_screen;
static lv_style_t style_text;
static lv_style_t style_muted;
static lv_style_t style_time;
static lv_style_t style_card_title;
static lv_style_t style_card_value;
static lv_style_t style_card_value_small;
static lv_style_t style_panel;
static lv_style_t style_panel_accent;
static lv_style_t style_layer_accent;
static bool display_ready;
static bool display_idle;
static int64_t next_display_idle_tick_ms;
static int64_t build_epoch_seconds;
static int64_t host_epoch_seconds = -1;
static int64_t host_epoch_uptime_seconds;
static int last_display_mode = -1;
static int last_display_battery = -999;
static int last_display_battery_mv = -999;
static int last_display_charge_state = -1;
static int last_display_hour = -1;
static int last_display_minute = -1;
static int last_display_day = -1;
static bool last_display_connected;
static bool last_display_numlock;

static lv_color_t mode_status_color(enum app_mode mode, bool connected,
				    bool numlock)
{
	if (!numlock) {
		return lv_color_hex(COLOR_LAYER);
	}

	switch (mode) {
	case APP_MODE_USB:
		return lv_color_hex(connected ? COLOR_USB : COLOR_LOW_BATTERY);
	case APP_MODE_BLE:
		return lv_color_hex(COLOR_BLE);
	case APP_MODE_OFF:
	default:
		return lv_color_hex(COLOR_MUTED);
	}
}

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

static void screen_power_set(bool on)
{
	int err;

	if (!display_ready) {
		screen_backlight_set(on);
		return;
	}

	if (on) {
		err = display_blanking_off(display_dev);
		if (err) {
			LOG_DBG("Display blanking off failed: %d", err);
		}
		screen_backlight_set(true);
	} else {
		screen_backlight_set(false);
		err = display_blanking_on(display_dev);
		if (err) {
			LOG_DBG("Display blanking on failed: %d", err);
		}
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

static const char *battery_prefix(enum power_charge_state charge_state,
				  enum app_mode mode, bool connected)
{
	switch (charge_state) {
	case POWER_CHARGE_CHARGING:
	case POWER_CHARGE_FULL:
		return "USB";
	case POWER_CHARGE_DISCHARGING:
		return (mode == APP_MODE_USB && connected) ? "USB" : "BAT";
	case POWER_CHARGE_UNKNOWN:
	default:
		return "---";
	}
}

static const char *battery_state_name(enum power_charge_state charge_state)
{
	switch (charge_state) {
	case POWER_CHARGE_DISCHARGING:
		return "Discharging";
	case POWER_CHARGE_CHARGING:
		return "Charging";
	case POWER_CHARGE_FULL:
		return "Full";
	case POWER_CHARGE_UNKNOWN:
	default:
		return "Unknown";
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
	lv_style_set_bg_color(&style_screen, lv_color_hex(COLOR_BG));
	lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
	lv_style_set_pad_all(&style_screen, 0);

	lv_style_init(&style_text);
	lv_style_set_text_color(&style_text, lv_color_hex(COLOR_TEXT));
	lv_style_set_bg_opa(&style_text, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_text, 0);
	lv_style_set_text_font(&style_text, &lv_font_montserrat_18);

	lv_style_init(&style_muted);
	lv_style_set_text_color(&style_muted, lv_color_hex(COLOR_MUTED));
	lv_style_set_bg_opa(&style_muted, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_muted, 0);
	lv_style_set_text_font(&style_muted, &lv_font_montserrat_14);

	lv_style_init(&style_time);
	lv_style_set_text_color(&style_time, lv_color_hex(COLOR_TEXT));
	lv_style_set_bg_opa(&style_time, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_time, 0);
	lv_style_set_text_font(&style_time, &lv_font_montserrat_18);

	lv_style_init(&style_card_title);
	lv_style_set_text_color(&style_card_title, lv_color_hex(COLOR_MUTED));
	lv_style_set_bg_opa(&style_card_title, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_card_title, 0);
	lv_style_set_text_font(&style_card_title, &lv_font_montserrat_18);

	lv_style_init(&style_card_value);
	lv_style_set_text_color(&style_card_value, lv_color_hex(COLOR_TEXT));
	lv_style_set_bg_opa(&style_card_value, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_card_value, 0);
	lv_style_set_text_font(&style_card_value, &lv_font_montserrat_24);

	lv_style_init(&style_card_value_small);
	lv_style_set_text_color(&style_card_value_small, lv_color_hex(COLOR_TEXT));
	lv_style_set_bg_opa(&style_card_value_small, LV_OPA_TRANSP);
	lv_style_set_pad_all(&style_card_value_small, 0);
	lv_style_set_text_font(&style_card_value_small, &lv_font_montserrat_24);

	lv_style_init(&style_panel);
	lv_style_set_bg_color(&style_panel, lv_color_hex(COLOR_CARD));
	lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
	lv_style_set_border_color(&style_panel, lv_color_hex(COLOR_BORDER));
	lv_style_set_border_width(&style_panel, 1);
	lv_style_set_radius(&style_panel, 6);
	lv_style_set_pad_all(&style_panel, 0);

	lv_style_init(&style_panel_accent);
	lv_style_set_border_color(&style_panel_accent, lv_color_hex(COLOR_USB));
	lv_style_set_border_width(&style_panel_accent, 2);

	lv_style_init(&style_layer_accent);
	lv_style_set_border_color(&style_layer_accent, lv_color_hex(COLOR_LAYER));
	lv_style_set_border_width(&style_layer_accent, 2);
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

	time_label = make_label(screen, 16, VISIBLE_Y + 1, 70, &style_time);
	date_label = make_label(screen, 16, VISIBLE_Y + 30, 122, &style_text);
	lv_obj_set_style_text_color(date_label, lv_color_hex(COLOR_TEXT), 0);
	battery_label = make_label(screen, 194, VISIBLE_Y + 4, 104, &style_time);
	lv_obj_set_style_text_align(battery_label, LV_TEXT_ALIGN_RIGHT, 0);
	battery_state_label = make_label(screen, 164, VISIBLE_Y + 32, 134,
					 &style_text);
	lv_obj_set_style_text_align(battery_state_label, LV_TEXT_ALIGN_RIGHT, 0);

	mode_panel = make_panel(screen, 16, VISIBLE_Y + 82, 176, 78);
	lv_obj_add_style(mode_panel, &style_panel_accent, 0);
	mode_label = make_label(mode_panel, 12, 10, 152, &style_card_title);
	mode_status_label = make_label(mode_panel, 12, 39, 152,
				       &style_card_value_small);

	layer_panel = make_panel(screen, 204, VISIBLE_Y + 82, 100, 78);
	lv_obj_add_style(layer_panel, &style_layer_accent, 0);
	layer_label = make_label(layer_panel, 12, 10, 76, &style_card_title);
	lv_label_set_text(layer_label, "LAYER");
	layer_value_label = make_label(layer_panel, 12, 37, 76,
				       &style_card_value);

	ble_label = make_label(screen, 16, VISIBLE_Y + 63, 180, &style_text);
	lv_obj_add_flag(ble_label, LV_OBJ_FLAG_HIDDEN);

	build_epoch_seconds = build_time_seconds();
	lv_timer_handler();
	display_ready = true;
	display_update_status(APP_MODE_BLE, -1, -1, POWER_CHARGE_UNKNOWN,
			      false, true);
	return 0;
}

void display_update_status(enum app_mode mode, int battery_percent,
			   int battery_mv, enum power_charge_state charge_state,
			   bool connected, bool numlock)
{
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
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

	if (host_epoch_seconds >= 0) {
		now = host_epoch_seconds +
		      ((k_uptime_get() / 1000) - host_epoch_uptime_seconds);
	} else {
		now = build_epoch_seconds + (k_uptime_get() / 1000);
	}
	epoch_to_date(now, &year, &month, &day, &hour, &minute, &second);
	time_changed = hour != last_display_hour ||
		       minute != last_display_minute ||
		       day != last_display_day;
	mode_changed = mode != last_display_mode ||
		       connected != last_display_connected;
	battery_changed = battery_percent != last_display_battery ||
			  battery_mv != last_display_battery_mv ||
			  charge_state != last_display_charge_state;
	numlock_changed = numlock != last_display_numlock;

	if (!time_changed && !mode_changed && !battery_changed &&
	    !numlock_changed) {
		return;
	}

	if (time_changed) {
		lv_label_set_text_fmt(time_label, "%02d:%02d", hour, minute);
		lv_label_set_text_fmt(date_label, "%04d-%02d-%02d",
				      year, month, day);
		lv_obj_set_style_text_color(time_label, lv_color_hex(COLOR_TEXT), 0);
		lv_obj_set_style_text_color(date_label, lv_color_hex(COLOR_TEXT), 0);
		last_display_hour = hour;
		last_display_minute = minute;
		last_display_day = day;
	}

	if (mode_changed) {
		lv_color_t color = mode_status_color(mode, connected, numlock);

		lv_label_set_text_fmt(mode_label, "%s MODE", mode_name(mode));
		if (mode == APP_MODE_BLE) {
			lv_label_set_text(mode_status_label,
					  connected ? "CONNECTED" : "SEARCHING");
			lv_label_set_text(ble_label, "");
		} else if (mode == APP_MODE_USB) {
			lv_label_set_text(mode_status_label,
					  connected ? "CONNECTED" : "WAITING");
			lv_label_set_text(ble_label, "");
		} else {
			lv_label_set_text(mode_status_label, "SLEEP");
			lv_label_set_text(ble_label, "");
		}
		lv_obj_set_style_border_color(mode_panel, color, 0);
		lv_obj_set_style_text_color(mode_label, color, 0);
		lv_obj_set_style_text_color(mode_status_label, color, 0);
		lv_label_set_text(layer_value_label, numlock ? "NUM" : "NAV");
		lv_obj_set_style_border_color(layer_panel, color, 0);
		lv_obj_set_style_text_color(layer_label, color, 0);
		lv_obj_set_style_text_color(layer_value_label, color, 0);
		lv_obj_set_style_text_color(ble_label,
					    mode == APP_MODE_BLE ?
					    lv_color_hex(COLOR_BLE) :
					    lv_color_hex(COLOR_MUTED), 0);
		last_display_mode = mode;
		last_display_connected = connected;
	}

	if (battery_changed || mode_changed) {
		if (battery_percent >= 0) {
			lv_label_set_text_fmt(battery_label, "%s %d%%",
					      battery_prefix(charge_state, mode,
							     connected),
					      battery_percent);
		} else {
			lv_label_set_text_fmt(battery_label, "%s ---",
					      battery_prefix(charge_state, mode,
							     connected));
		}
		lv_label_set_text(battery_state_label,
				  battery_state_name(charge_state));
		lv_obj_set_style_text_color(battery_label,
					    lv_color_hex(COLOR_TEXT), 0);
		lv_obj_set_style_text_color(battery_state_label,
					    lv_color_hex(COLOR_TEXT), 0);
		last_display_battery = battery_percent;
		last_display_battery_mv = battery_mv;
		last_display_charge_state = charge_state;
	}

	if (numlock_changed) {
		lv_color_t color = mode_status_color(mode, connected, numlock);

		lv_label_set_text(layer_value_label, numlock ? "NUM" : "NAV");
		lv_obj_set_style_border_color(mode_panel, color, 0);
		lv_obj_set_style_text_color(mode_label, color, 0);
		lv_obj_set_style_text_color(mode_status_label, color, 0);
		lv_obj_set_style_border_color(layer_panel, color, 0);
		lv_obj_set_style_text_color(layer_label, color, 0);
		lv_obj_set_style_text_color(layer_value_label, color, 0);
		last_display_numlock = numlock;
	}
	lv_timer_handler();
}

void display_set_host_time(uint32_t unix_time, int16_t timezone_offset_min,
			   uint8_t flags)
{
	ARG_UNUSED(flags);

	host_epoch_seconds = (int64_t)unix_time +
			     ((int64_t)timezone_offset_min * 60);
	host_epoch_uptime_seconds = k_uptime_get() / 1000;
	last_display_hour = -1;
	last_display_minute = -1;
	last_display_day = -1;
}

void app_display_tick(void)
{
	int64_t now;

	if (!display_ready) {
		return;
	}

	if (!display_idle) {
		lv_timer_handler();
		return;
	}

	now = k_uptime_get();
	if (now < next_display_idle_tick_ms) {
		return;
	}

	next_display_idle_tick_ms = now + DISPLAY_IDLE_TICK_MS;
	lv_timer_handler();
}

void app_display_set_idle(bool idle)
{
	if (display_idle == idle) {
		return;
	}

	display_idle = idle;
	LOG_INF("Display %s", idle ? "idle" : "resume");
	screen_power_set(!idle);
	if (display_ready) {
		next_display_idle_tick_ms = 0;
		lv_timer_handler();
	}
}
