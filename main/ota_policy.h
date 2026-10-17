/* Pure OTA safety policy, kept out of main.c so it can be host-tested.
 *
 * Two independent guards protect the measurement history during an update:
 *   1. the update slot must not overlap the SPIFFS "storage" partition, and
 *   2. a pulled image URL must use https (a cleartext image could be swapped
 *      on the wire and brick the device).
 * Both are pure functions with no IDF dependency, so the host test drives the
 * exact code that runs on the device. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* https-only check for a pull-OTA URL. Anything else (http, ftp, a bare host)
 * is rejected. Case-sensitive: URL schemes are lower-case by convention and
 * esp_http_client expects a lower-case scheme. */
bool ota_url_is_trusted_policy(const char *url);

/* True when the two half-open byte ranges [a_start, a_start+a_size) and
 * [b_start, b_start+b_size) do not overlap. Handles the zero-size case. */
bool ota_ranges_disjoint(uint32_t a_start, uint32_t a_size,
                         uint32_t b_start, uint32_t b_size);

/* True when it is safe to write the update image described by
 * [target_start, target_start+target_size) without touching the measurement
 * partition [store_start, store_start+store_size). An absent measurement
 * partition (store_size == 0) is always safe. */
bool ota_target_is_safe_range(uint32_t target_start, uint32_t target_size,
                              uint32_t store_start, uint32_t store_size);
