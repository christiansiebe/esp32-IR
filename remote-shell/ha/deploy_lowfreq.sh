#!/bin/sh
# Installs the lowfreq device-tree overlay and its safety net on the Odroid N2(+).
#
# Safety net (why a bad low OPP point cannot cause a boot loop):
#   * cpufreq.default_governor=performance -> the early-boot governor runs the
#     CPU at maximum frequency, before any userspace floor exists.
#   * cpu-freq-guard.service pins scaling_min_freq to 1.0 GHz right after
#     sysinit, so the new points are only reachable by an explicit runtime
#     probe; a reset restores the floor.
#
# Everything here is reversed by revert_lowfreq.sh.
set -u

DTB=/boot/dtb/amlogic/meson-g12b-odroid-n2-plus.dtb
DTBO=/boot/overlay-user/lowfreq.dtbo

echo '== 1. Overlay kompilieren =='
dtc -@ -I dts -O dtb -o /root/lowfreq.dtbo /root/lowfreq.dts || { echo 'FEHLER: dtc'; exit 1; }

echo '== 2. gegen das laufende DTB validieren =='
fdtoverlay -i "$DTB" -o /tmp/merged.dtb /root/lowfreq.dtbo || { echo 'FEHLER: fdtoverlay'; exit 1; }

echo '== 3. neue OPPs im gemergten DTB pruefen =='
dtc -I dtb -O dts /tmp/merged.dtb > /tmp/merged.dts 2>/dev/null
for f in 666666666 500000000 250000000 200000000 125000000 100000000 62500000 50000000 40000000 31250000 25000000 20000000 15625000 24000000; do
	# Anker " {" noetig: "opp-100000000" ist Praefix von "opp-1000000000".
	n=$(grep -c "opp-$f {" /tmp/merged.dts)
	[ "$n" = "2" ] || { echo "FEHLER: opp-$f $n mal (erwartet 2)"; exit 1; }
done
echo 'alle neuen OPPs vorhanden (je 2)'

echo '== 4. Overlay installieren =='
mkdir -p /boot/overlay-user
cp /root/lowfreq.dtbo "$DTBO"
ls -l "$DTBO"

echo '== 5. Sicherheitsnetz: frueher Boot-Governor =='
grep -q '^user_overlays=' /boot/armbianEnv.txt || echo 'user_overlays=lowfreq' >> /boot/armbianEnv.txt
if ! grep -q 'cpufreq.default_governor=performance' /boot/armbianEnv.txt; then
	sed -i 's/^extraargs=\(.*\)$/extraargs=\1 cpufreq.default_governor=performance/' /boot/armbianEnv.txt
fi
grep -nE 'user_overlays|extraargs' /boot/armbianEnv.txt

echo '== 6. Sicherheitsnetz: Floor-Dienst =='
install -m 0755 /root/cpu-freq-guard.sh /usr/local/sbin/cpu-freq-guard.sh
echo 1000000 > /etc/default/cpu-freq-guard
install -m 0644 /root/cpu-freq-guard.service /etc/systemd/system/cpu-freq-guard.service
systemctl daemon-reload
systemctl enable cpu-freq-guard.service
systemctl start cpu-freq-guard.service
systemctl is-active cpu-freq-guard.service

echo '== fertig - Reboot noetig =='
