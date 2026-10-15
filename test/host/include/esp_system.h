/* Host-test shim: the slice of esp_system.h that boot_confirm.c uses. The
 * reboot call is a no-op here; the boot-confirm test records it via its own
 * fakes so the "rollback path does not return" assumption is not required. */
#pragma once
#include "esp_shim.h"

void esp_restart(void);
