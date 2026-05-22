#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "app_types.h"
#include "display.h"
#include "hid_transport.h"
#include "keyboard_matrix.h"
#include "keymap.h"
#include "power.h"
#include "rgb.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define CTRL_NODE DT_NODELABEL(board_controls)
#define SCAN_INTERVAL K_MSEC(5)
#define STATUS_INTERVAL_MS 1000

static const struct gpio_dt_spec mode_gpio =
	GPIO_DT_SPEC_GET(CTRL_NODE, mode_gpios);

struct encoder_line {
	const char *name;
	struct gpio_dt_spec gpio;
	struct gpio_callback cb;
};

struct encoder_pair {
	const char *name;
	uint8_t a;
	uint8_t b;
	int last_state;
	int8_t accumulator;
};

static struct hid_keyboard_report keyboard_report;
static uint8_t pressed_usage[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS];
static atomic_t encoder_pending_steps;
static struct k_work encoder_work;
static int active_encoder_pair = -1;

static struct encoder_line encoder_lines[] = {
	{
		.name = "P0.10",
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 10,
			.dt_flags = GPIO_PULL_UP | GPIO_ACTIVE_LOW,
		},
	},
	{
		.name = "P1.06",
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
			.pin = 6,
			.dt_flags = GPIO_PULL_UP | GPIO_ACTIVE_LOW,
		},
	},
	{
		.name = "P0.09",
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 9,
			.dt_flags = GPIO_PULL_UP | GPIO_ACTIVE_LOW,
		},
	},
};

static struct encoder_pair encoder_pairs[] = {
	{ .name = "P0.10/P1.06", .a = 0, .b = 1, .last_state = -1 },
	{ .name = "P0.09/P1.06", .a = 2, .b = 1, .last_state = -1 },
	{ .name = "P0.09/P0.10", .a = 2, .b = 0, .last_state = -1 },
};

static enum app_mode read_mode_switch(void)
{
	int val;

	if (!gpio_is_ready_dt(&mode_gpio)) {
		return APP_MODE_BLE;
	}

	val = gpio_pin_get_dt(&mode_gpio);
	return val > 0 ? APP_MODE_BLE : APP_MODE_USB;
}

static void clear_keyboard_report(void)
{
	memset(&keyboard_report, 0, sizeof(keyboard_report));
	memset(pressed_usage, 0, sizeof(pressed_usage));
	(void)hid_transport_send_keyboard(&keyboard_report);
}

static void add_usage(uint8_t usage)
{
	if (usage == 0) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(keyboard_report.keys); i++) {
		if (keyboard_report.keys[i] == usage) {
			return;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(keyboard_report.keys); i++) {
		if (keyboard_report.keys[i] == 0) {
			keyboard_report.keys[i] = usage;
			return;
		}
	}

	LOG_WRN("Keyboard rollover limit reached, dropping usage 0x%02x", usage);
}

static void remove_usage(uint8_t usage)
{
	if (usage == 0) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(keyboard_report.keys); i++) {
		if (keyboard_report.keys[i] == usage) {
			keyboard_report.keys[i] = 0;
		}
	}
}

static void process_key_event(const struct keyboard_event *event)
{
	uint8_t usage;

	if (event->row >= KEYBOARD_MATRIX_ROWS || event->col >= KEYBOARD_MATRIX_COLS) {
		return;
	}

	if (event->row == 0 && event->col == 3) {
		if (event->pressed) {
			LOG_INF("encoder switch mute");
			(void)hid_transport_send_consumer(HID_CONSUMER_MUTE);
		}
		return;
	}

	if (event->pressed) {
		usage = keymap_usage_for_event(event);
		pressed_usage[event->row][event->col] = usage;
		add_usage(usage);
		keymap_note_event(event);
	} else {
		usage = pressed_usage[event->row][event->col];
		pressed_usage[event->row][event->col] = 0;
		remove_usage(usage);
	}

	LOG_INF("key r%u c%u %s usage 0x%02x layer %s",
		event->row, event->col, event->pressed ? "down" : "up",
		usage, keymap_layer_name());
	(void)hid_transport_send_keyboard(&keyboard_report);
}

static void encoder_work_handler(struct k_work *work)
{
	int steps;

	ARG_UNUSED(work);

	steps = atomic_set(&encoder_pending_steps, 0);
	while (steps > 0) {
		(void)hid_transport_send_consumer(HID_CONSUMER_VOLUME_UP);
		steps--;
	}
	while (steps < 0) {
		(void)hid_transport_send_consumer(HID_CONSUMER_VOLUME_DOWN);
		steps++;
	}
}

static int encoder_line_state(uint8_t index)
{
	int value = gpio_pin_get_dt(&encoder_lines[index].gpio);

	return value > 0 ? 1 : 0;
}

static void encoder_sample_pair(struct encoder_pair *pair, size_t pair_index)
{
	static const int8_t delta_table[16] = {
		0, -1, 1, 0,
		1, 0, 0, -1,
		-1, 0, 0, 1,
		0, 1, -1, 0,
	};
	int state;
	int idx;
	int8_t delta;

	if (active_encoder_pair >= 0 && active_encoder_pair != (int)pair_index) {
		return;
	}

	if (pair->last_state < 0) {
		return;
	}

	state = (encoder_line_state(pair->a) << 1) | encoder_line_state(pair->b);
	if (state == pair->last_state) {
		return;
	}

	idx = (pair->last_state << 2) | state;
	pair->last_state = state;
	delta = delta_table[idx & 0x0f];
	if (delta == 0) {
		return;
	}

	pair->accumulator += delta;
	if (pair->accumulator >= 2) {
		pair->accumulator = 0;
		if (active_encoder_pair < 0) {
			active_encoder_pair = (int)pair_index;
			LOG_INF("Encoder pair locked to %s", pair->name);
		}
		atomic_inc(&encoder_pending_steps);
		(void)k_work_submit(&encoder_work);
	} else if (pair->accumulator <= -2) {
		pair->accumulator = 0;
		if (active_encoder_pair < 0) {
			active_encoder_pair = (int)pair_index;
			LOG_INF("Encoder pair locked to %s", pair->name);
		}
		atomic_dec(&encoder_pending_steps);
		(void)k_work_submit(&encoder_work);
	}
}

static void encoder_sample(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(encoder_pairs); i++) {
		encoder_sample_pair(&encoder_pairs[i], i);
	}
}

static void encoder_gpio_cb(const struct device *dev, struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	encoder_sample();
}

static int encoder_init(void)
{
	int err;

	k_work_init(&encoder_work, encoder_work_handler);

	for (size_t i = 0; i < ARRAY_SIZE(encoder_lines); i++) {
		struct encoder_line *line = &encoder_lines[i];

		if (!gpio_is_ready_dt(&line->gpio)) {
			LOG_WRN("Encoder GPIO %s is not ready", line->name);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&line->gpio, GPIO_INPUT);
		if (err) {
			return err;
		}

		gpio_init_callback(&line->cb, encoder_gpio_cb, BIT(line->gpio.pin));
		err = gpio_add_callback(line->gpio.port, &line->cb);
		if (err) {
			return err;
		}

		err = gpio_pin_interrupt_configure_dt(&line->gpio, GPIO_INT_EDGE_BOTH);
		if (err) {
			return err;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(encoder_pairs); i++) {
		struct encoder_pair *pair = &encoder_pairs[i];

		pair->last_state = (encoder_line_state(pair->a) << 1) |
				    encoder_line_state(pair->b);
		LOG_INF("Encoder candidate %s initial state %d",
			pair->name, pair->last_state);
	}

	return 0;
}

int main(void)
{
	struct keyboard_event events[8];
	int64_t next_status = 0;
	enum app_mode mode = APP_MODE_BLE;

	LOG_INF("AI BLE Keyboard starting");

	(void)power_init();
	(void)app_display_init();
	(void)rgb_init();
	keymap_init();

	if (gpio_is_ready_dt(&mode_gpio)) {
		(void)gpio_pin_configure_dt(&mode_gpio, GPIO_INPUT);
	}

	if (keyboard_matrix_init() != 0) {
		LOG_ERR("Keyboard matrix init failed");
	}

	(void)encoder_init();

	mode = read_mode_switch();

	if (hid_transport_init(mode) != 0) {
		LOG_ERR("HID transport init failed");
	}

	hid_transport_set_mode(mode);

	while (true) {
		enum app_mode new_mode = read_mode_switch();
		int event_count;

		if (new_mode != mode) {
			mode = new_mode;
			hid_transport_set_mode(mode);
			clear_keyboard_report();
			LOG_INF("Mode changed to %s", mode == APP_MODE_BLE ? "BLE" : "USB");
		}

		if (mode != APP_MODE_OFF) {
			event_count = keyboard_matrix_scan(events, ARRAY_SIZE(events));
			for (int i = 0; i < event_count; i++) {
				process_key_event(&events[i]);
			}
			encoder_sample();
		}
		app_display_tick();
		power_ip5306_keepalive_tick(mode != APP_MODE_OFF);

		if (k_uptime_get() >= next_status) {
			int battery = power_get_battery_percent();
			bool connected = hid_transport_connected();

			display_update_status(mode, battery, connected,
					      keymap_numlock_enabled());
			rgb_show_status(mode, connected, keymap_numlock_enabled());
			if (hid_transport_ble_ready()) {
				bt_bas_set_battery_level(battery >= 0 ? battery : 0);
			}
			next_status = k_uptime_get() + STATUS_INTERVAL_MS;
		}

		k_sleep(SCAN_INTERVAL);
	}

	return 0;
}
