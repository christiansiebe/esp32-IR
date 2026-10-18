#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Receiver health watchdog (receiver role only).
 *
 * A low-priority task evaluates health_policy.h every few seconds and reboots
 * the device when the meter data has been silent for too long or the heap fell
 * below the floor. It is the counterpart of web_guard (which watches the
 * dashboard): web_guard covers "the UI is dead", health_task covers "the UI is
 * up but the measurement data never arrives", which the confirm/rollback and
 * liveness paths do not notice.
 *
 * The failure is escalated through boot_confirm_fail(): while the running image
 * is still unconfirmed that rolls back to the previous OTA slot, otherwise it
 * is a plain reboot.
 *
 * The worker subscribes to the task watchdog (task_wdt_guard), so a wedged
 * health loop also reboots instead of silently disappearing.
 *
 * Call health_task_note_rx() whenever a fresh SML value arrives (any OBIS).
 */
esp_err_t health_task_start(void);

void health_task_note_rx(void);
