#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Receiver health watchdog. A low-priority task evaluates health_policy.h and
 * reboots the device when the meter data has been silent for too long or the
 * heap fell below the floor. Call health_task_note_rx() whenever a fresh SML
 * value arrives. */
esp_err_t health_task_start(void);

void health_task_note_rx(void);
