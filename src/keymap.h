#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "keyboard_matrix.h"

void keymap_init(void);
uint8_t keymap_usage_for_event(const struct keyboard_event *event);
void keymap_note_event(const struct keyboard_event *event);
bool keymap_numlock_enabled(void);
const char *keymap_layer_name(void);

#endif
