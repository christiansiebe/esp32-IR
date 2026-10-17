/* The firmware version must always resolve to a non-empty string, so the
 * dashboard and the diagnostics always have something to identify the build.
 * The value comes from build_version.h (CMake) or PROJECT_VER; on a host build
 * without either, it falls back to "unknown" rather than an empty string. */
#include <stdio.h>
#include <string.h>

#include "version.h"

int main(void)
{
    const char *v = fw_version();
    if (v == NULL || v[0] == '\0') {
        printf("FAIL: fw_version() returned an empty string\n");
        return 1;
    }
    printf("OK: fw_version() = '%s'\n", v);
    return 0;
}
