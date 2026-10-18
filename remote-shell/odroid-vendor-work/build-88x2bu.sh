#!/bin/sh
# build-88x2bu.sh - build the ported RTL88x2BU vendor driver for this kernel.
#
# Two things make this work on the Odroid N2 (Armbian bookworm, kernel 7.1.8):
#   1. The kernel is built with GCC 14.2.0-19, which bookworm does not ship.
#      Build inside debian:trixie-slim, which has exactly that toolchain.
#      A GCC-12 module builds but panics the kernel at insmod (ftrace validation).
#   2. strip --strip-debug the .ko (77 MB unstripped -> 6.5 MB, loads clean).
set -e
SRC=${1:-/usr/src/rtl88x2bu-git}
KREL=$(uname -r)
docker run --rm \
  -v "$SRC":/src \
  -v "/usr/src/linux-headers-$KREL:/lib/modules/$KREL/build" \
  -w /src debian:trixie-slim sh -c '
    set -e
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq gcc make binutils bc libssl-dev libelf-dev flex bison kmod >/dev/null 2>&1
    make clean >/dev/null 2>&1 || true
    make -j4 ARCH=arm64 KSRC=/lib/modules/'"$KREL"'/build
    strip --strip-debug 88x2bu.ko
    echo "built: $(ls -la 88x2bu.ko)"
    gcc --version | head -1
  '
