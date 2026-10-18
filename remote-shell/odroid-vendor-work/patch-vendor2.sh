#!/bin/sh
# patch-vendor2.sh - correct port of the Realtek rtl88x2bu vendor driver to kernel 7.x
set -e
cd /usr/src/rtl88x2bu-git

[ -f Makefile.orig ] || cp Makefile Makefile.orig
[ -f include/osdep_service_linux.h.orig ] || cp include/osdep_service_linux.h include/osdep_service_linux.h.orig

echo "=== Makefile: ccflags-y right after the KERNELRELEASE branch ==="
cp Makefile.orig Makefile
L=$(grep -n 'ifneq ($(KERNELRELEASE),)' Makefile | head -1 | cut -d: -f1)
sed -i "${L}a ccflags-y += \$(EXTRA_CFLAGS)" Makefile
grep -n 'ccflags-y' Makefile

echo "=== timer shim at TOP of osdep_service_linux.h (after linux/version.h) ==="
cp include/osdep_service_linux.h.orig include/osdep_service_linux.h
V=$(grep -n '#include <linux/version.h>' include/osdep_service_linux.h | head -1 | cut -d: -f1)
awk -v v="$V" '
  NR==v {
    print;
    print "";
    print "/* ODROID_TIMER_COMPAT: kernel 6.15+ renamed the timer API */";
    print "#include <linux/timer.h>";
    print "#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0))";
    print "#ifndef from_timer";
    print "#define from_timer(var, callback_timer, timer_fieldname) \\";
    print "        timer_container_of(var, callback_timer, timer_fieldname)";
    print "#endif";
    print "#define del_timer(t) timer_delete(t)";
    print "#define del_timer_sync(t) timer_delete_sync(t)";
    print "#endif";
    next
  }
  { print }
' include/osdep_service_linux.h > /tmp/osl.new && mv /tmp/osl.new include/osdep_service_linux.h
sed -n '18,32p' include/osdep_service_linux.h

echo "=== build ==="
make clean >/dev/null 2>&1 || true
make -j2 ARCH=arm64 KSRC=/lib/modules/$(uname -r)/build CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT= > /root/netbackup/vendor-build8.log 2>&1
echo "RC=$?"
echo "=== distinct errors ==="
grep -oE 'error: [^,;]*' /root/netbackup/vendor-build8.log | sed 's/[0-9]\+//g' | sort | uniq -c | sort -rn | head -15
echo "=== .ko ==="
find . -name '*.ko' 2>/dev/null
