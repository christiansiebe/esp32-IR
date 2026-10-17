#include "version.h"

#if defined(__has_include)
#  if __has_include("build_version.h")
#    include "build_version.h"
#  endif
#endif

#ifndef FIRMWARE_VERSION
#  ifdef PROJECT_VER
#    define FIRMWARE_VERSION PROJECT_VER
#  else
#    define FIRMWARE_VERSION "unknown"
#  endif
#endif

const char *fw_version(void)
{
    return FIRMWARE_VERSION;
}
