#include "keyboard_matrix.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(keyboard_matrix, LOG_LEVEL_INF);

#define KBD_NODE DT_NODELABEL(kbd_matrix)
#define DEBOUNCE_SCANS 3

static const struct gpio_dt_spec rows[KEYBOARD_MATRIX_ROWS] = {
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 3),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 4),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, row_gpios, 5),
};

static const struct gpio_dt_spec cols[KEYBOARD_MATRIX_COLS] = {
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, col_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, col_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, col_gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(KBD_NODE, col_gpios, 3),
};

static bool debounced[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS];
static bool last_raw[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS];
static uint8_t stable_count[KEYBOARD_MATRIX_ROWS][KEYBOARD_MATRIX_COLS];

int keyboard_matrix_init(void)
{
	int err;

	for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
		if (!gpio_is_ready_dt(&rows[r])) {
			LOG_ERR("Row %u GPIO is not ready", r);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&rows[r], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("Failed to configure row %u: %d", r, err);
			return err;
		}
	}

	for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
		if (!gpio_is_ready_dt(&cols[c])) {
			LOG_ERR("Column %u GPIO is not ready", c);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&cols[c], GPIO_INPUT);
		if (err) {
			LOG_ERR("Failed to configure column %u: %d", c, err);
			return err;
		}
	}

	memset(debounced, 0, sizeof(debounced));
	memset(last_raw, 0, sizeof(last_raw));
	memset(stable_count, 0, sizeof(stable_count));
	return 0;
}

int keyboard_matrix_scan(struct keyboard_event *events, size_t max_events)
{
	size_t count = 0;

	for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
		(void)gpio_pin_set_dt(&rows[r], 0);
	}

	for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
		(void)gpio_pin_set_dt(&rows[r], 1);
		k_busy_wait(30);

		for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
			int val = gpio_pin_get_dt(&cols[c]);
			bool raw_pressed = (val > 0);

			if (raw_pressed == last_raw[r][c]) {
				if (stable_count[r][c] < DEBOUNCE_SCANS) {
					stable_count[r][c]++;
				}
			} else {
				last_raw[r][c] = raw_pressed;
				stable_count[r][c] = 0;
			}

			if ((stable_count[r][c] >= DEBOUNCE_SCANS) &&
			    (debounced[r][c] != raw_pressed)) {
				debounced[r][c] = raw_pressed;
				if ((events != NULL) && (count < max_events)) {
					events[count].row = r;
					events[count].col = c;
					events[count].pressed = raw_pressed;
					count++;
				}
			}
		}

		(void)gpio_pin_set_dt(&rows[r], 0);
	}

	return (int)count;
}
