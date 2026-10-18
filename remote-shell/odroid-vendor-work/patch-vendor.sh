#!/bin/sh
# patch-vendor.sh - port the Realtek rtl88x2bu vendor driver to kernel 7.x
#
# Three independent breakages on kernel 7.1.8:
#  1. Makefile builds all -I paths into EXTRA_CFLAGS, but kbuild >= 6.x only
#     reads ccflags-y. Fix: one `ccflags-y += $(EXTRA_CFLAGS)` at the END of
#     the KERNELRELEASE branch (the .mk includes fill EXTRA_CFLAGS first).
#  2. Timer API renamed: from_timer -> timer_container_of,
#     del_timer -> timer_delete, del_timer_sync -> timer_delete_sync.
#  3. halrf/halrf_psd.h include resolves once -I$(src)/hal/phydm is applied
#     (fixed by #1).
set -e
cd /usr/src/rtl88x2bu-git

[ -f Makefile.orig ] || cp Makefile Makefile.orig
[ -f include/osdep_service_linux.h.orig ] || cp include/osdep_service_linux.h include/osdep_service_linux.h.orig

echo "=== 1) Makefile: ccflags-y at END of kbuild branch ==="
cp Makefile.orig Makefile
# drop the earlier (too early) insertion if present
sed -i '/^ccflags-y += \$(EXTRA_CFLAGS)$/d' Makefile
# insert just before the top-level `else` that closes the KERNELRELEASE branch
awk '
  /^else$/ && !done { print "ccflags-y += $(EXTRA_CFLAGS)"; done=1 }
  { print }
' Makefile > Makefile.new && mv Makefile.new Makefile
grep -n 'ccflags-y' Makefile

echo "=== 2) timer API shim in osdep_service_linux.h ==="
cp include/osdep_service_linux.h.orig include/osdep_service_linux.h
# add compat defines right after the last system include block, guarded by kernel version
if ! grep -q 'ODROID_TIMER_COMPAT' include/osdep_service_linux.h; then
  awk '
    /#include <linux\/netlink.h>/ && !done {
      print;
      print "";
      print "/* ODROID_TIMER_COMPAT: kernel 7.x renamed the timer API */";
      print "#include <linux/timer.h>";
      print "#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0))";
      print "#ifndef from_timer";
      print "#define from_timer(var, callback_timer, timer_fieldname) \\";
      print "        timer_container_of(var, callback_timer, timer_fieldname)";
      print "#endif";
      print "#define del_timer(t) timer_delete(t)";
      print "#define del_timer_sync(t) timer_delete_sync(t)";
      print "#endif";
      done=1; next
    }
    { print }
  ' include/osdep_service_linux.h > include/osdep_service_linux.h.new && mv include/osdep_service_linux.h.new include/osdep_service_linux.h
fi
grep -n 'ODROID_TIMER_COMPAT' include/osdep_service_linux.h

echo "=== build ==="
make clean >/dev/null 2>&1 || true
make -j2 ARCH=arm64 KSRC=/lib/modules/$(uname -r)/build > /root/netbackup/vendor-build5.log 2>&1
echo "RC=$?"
echo "=== distinct errors ==="
grep -oE 'error: [^,;]*' /root/netbackup/vendor-build5.log | sed 's/[0-9]\+//g' | sort | uniq -c | sort -rn | head -15
echo "=== .ko produced? ==="
find . -name '*.ko' 2>/dev/null
