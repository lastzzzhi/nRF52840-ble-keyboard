#include "key_wakeup_pm.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "keyboard_matrix.h"

LOG_MODULE_REGISTER(key_wakeup_pm, LOG_LEVEL_INF);

#define ACTIVE_SCAN_INTERVAL K_MSEC(5)
#define IDLE_SCAN_INTERVAL K_MSEC(50)
#define OFF_INTERVAL K_MSEC(200)
/* Keep the keyboard fully active for 60 s before entering shallow idle. */
#define ACTIVE_HOLD_MS 60000

static atomic_t wake_requested;
static bool idle;
static int64_t active_until_ms;
static k_tid_t owner_thread;

void key_wakeup_pm_init(int64_t now)
{
	active_until_ms = now + ACTIVE_HOLD_MS;
	idle = false;
	owner_thread = k_current_get();
	atomic_clear(&wake_requested);
}

void key_wakeup_pm_wakeup_isr(void)
{
	atomic_set(&wake_requested, 1);
	if (owner_thread != NULL) {
		k_wakeup(owner_thread);
	}
}

void key_wakeup_pm_note_activity_reason(int64_t now, const char *reason)
{
	active_until_ms = now + ACTIVE_HOLD_MS;
	atomic_clear(&wake_requested);

	if (idle) {
		keyboard_matrix_wakeup_disarm();
		idle = false;
		LOG_INF("%s: active scan", reason);
	}
}

void key_wakeup_pm_note_activity(int64_t now)
{
	key_wakeup_pm_note_activity_reason(now, "Keyboard wakeup");
}

bool key_wakeup_pm_is_idle(void)
{
	return idle;
}

bool key_wakeup_pm_should_scan(enum app_mode mode)
{
	return (mode != APP_MODE_OFF) && !idle;
}

void key_wakeup_pm_update(enum app_mode mode, int64_t now)
{
	if (mode == APP_MODE_OFF) {
		if (idle) {
			keyboard_matrix_wakeup_disarm();
			idle = false;
		}
		return;
	}

	if (atomic_get(&wake_requested) != 0) {
		key_wakeup_pm_note_activity(now);
		return;
	}

	if (!idle && now >= active_until_ms) {
		int err = keyboard_matrix_wakeup_arm(key_wakeup_pm_wakeup_isr);

		if (err == 0) {
			idle = true;
			LOG_INF("Keyboard idle: matrix wakeup armed");
		} else {
			active_until_ms = now + ACTIVE_HOLD_MS;
		}
	}
}

k_timeout_t key_wakeup_pm_sleep_interval(enum app_mode mode, int64_t now)
{
	if (mode == APP_MODE_OFF) {
		return OFF_INTERVAL;
	}

	if (idle) {
		return IDLE_SCAN_INTERVAL;
	}

	if (now < active_until_ms) {
		return ACTIVE_SCAN_INTERVAL;
	}

	return IDLE_SCAN_INTERVAL;
}
