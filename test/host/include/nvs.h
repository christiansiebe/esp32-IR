/* Host-test shim for the NVS API used by wall_clock_store.c.
 *
 * Only the wall-clock host test compiles a *_store.c translation unit, so this
 * shim is not pulled in by the other tests. The backing store is provided by
 * the test itself (a small file-per-namespace/key store), so a forked "reboot"
 * child reads exactly what the parent wrote.
 */
#pragma once
#include "esp_shim.h"

typedef int nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
