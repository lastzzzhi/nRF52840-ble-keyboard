#ifndef KEY_WAKEUP_PM_H
#define KEY_WAKEUP_PM_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "app_types.h"

void key_wakeup_pm_init(int64_t now);
void key_wakeup_pm_note_activity(int64_t now);
void key_wakeup_pm_note_activity_reason(int64_t now, const char *reason);
void key_wakeup_pm_wakeup_isr(void);
bool key_wakeup_pm_is_idle(void);
bool key_wakeup_pm_should_scan(enum app_mode mode);
void key_wakeup_pm_update(enum app_mode mode, int64_t now);
k_timeout_t key_wakeup_pm_sleep_interval(enum app_mode mode, int64_t now);

#endif
