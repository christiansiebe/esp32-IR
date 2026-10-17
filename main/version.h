#pragma once

/* Firmware version string for identification on the wire and in the UI.
 *
 * The build system (root CMakeLists.txt) captures `git describe --tags --always
 * --dirty` at configure time and writes it into `main/build_version.h` as
 * `FIRMWARE_VERSION`. `fw_version()` falls back to the compile-time
 * `PROJECT_VER` when that header is absent (host builds). The result is exposed
 * as a JSON field `version` in every diagnostic payload and shown on the
 * dashboard, so a flashed device can be identified without guessing. */

const char *fw_version(void);
