#!/bin/sh
# Installs the ramoops device-tree overlay so a kernel panic survives a reset.
#
# Why: the Amlogic SoC reports no reset reason (watchdog bootstatus is always 0,
# pstore was empty) and journald does not flush the kernel log before a panic
# freeze. ramoops keeps the panic dmesg in a reserved RAM area that survives a
# warm reset; afterwards /sys/fs/pstore holds it and boot_journal.py reads it.
#
# Safety: boot.cmd restores the original DT if `fdt apply` fails, so a malformed
# overlay cannot brick the board. The address 0x50000000 was checked against the
# live memblock map (free window 0x40000000..0xdfd2dfff) - see gen_ramoops_dts.py.
#
# Reversed by revert_ramoops.sh.
set -eu

HERE=$(dirname "$(readlink -f "$0")")
DTB=/boot/dtb/amlogic/meson-g12b-odroid-n2-plus.dtb
DTBO=/boot/overlay-user/ramoops.dtbo

echo '== 1. Overlay erzeugen + kompilieren =='
python3 "$HERE/gen_ramoops_dts.py" --out /root/ramoops.dts --compile

echo '== 2. gegen das laufende DTB validieren =='
fdtoverlay -i "$DTB" -o /tmp/ramoops-merged.dtb /root/ramoops.dtbo \
  || { echo 'FEHLER: fdtoverlay'; exit 1; }

echo '== 3. ramoops-Knoten im gemergten DTB pruefen =='
dtc -I dtb -O dts /tmp/ramoops-merged.dtb > /tmp/ramoops-merged.dts 2>/dev/null
grep -q 'ramoops@50000000' /tmp/ramoops-merged.dts \
  || { echo 'FEHLER: ramoops-Knoten fehlt'; exit 1; }
grep -A5 'ramoops@50000000' /tmp/ramoops-merged.dts

echo '== 4. Overlay installieren =='
mkdir -p /boot/overlay-user
cp /root/ramoops.dtbo "$DTBO"
ls -l "$DTBO"

echo '== 5. user_overlays ergaenzen (lowfreq behalten!) =='
if grep -q '^user_overlays=' /boot/armbianEnv.txt; then
  if ! grep -qE '^user_overlays=.*\bramoops\b' /boot/armbianEnv.txt; then
    sed -i 's/^user_overlays=\(.*\)$/user_overlays=\1 ramoops/' /boot/armbianEnv.txt
  fi
else
  echo 'user_overlays=ramoops' >> /boot/armbianEnv.txt
fi
grep -n '^user_overlays=' /boot/armbianEnv.txt

echo '== 6. Modul-Load absichern (Autoload ueber OF-Alias reicht meist) =='
echo ramoops > /etc/modules-load.d/ramoops.conf

# / ist ext4 mit commit=120: ohne sync kann ein Reset kurz nach dem Deploy die
# Schreibzugriffe verschlucken. Nach jedem Deploy flushen.
sync

echo '== fertig - Reboot noetig =='
