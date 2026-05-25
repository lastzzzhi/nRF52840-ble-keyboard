#ifndef KEYBOARD_MATRIX_H
#define KEYBOARD_MATRIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYBOARD_MATRIX_ROWS 6
#define KEYBOARD_MATRIX_COLS 4

struct keyboard_event {
	uint8_t row;
	uint8_t col;
	bool pressed;
};

typedef void (*keyboard_matrix_wakeup_cb_t)(void);

int keyboard_matrix_init(void);
int keyboard_matrix_scan(struct keyboard_event *events, size_t max_events);
int keyboard_matrix_wakeup_arm(keyboard_matrix_wakeup_cb_t cb);
void keyboard_matrix_wakeup_disarm(void);
bool keyboard_matrix_wakeup_pressed(void);

#endif
