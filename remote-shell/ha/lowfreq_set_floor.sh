#!/bin/sh
# Setzt die dauerhafte CPU-Untergrenze (kHz) und wendet sie sofort an.
#
# Die Untergrenze steht in /etc/default/cpu-freq-guard und wird von
# cpu-freq-guard.service beim Boot angewandt. Nur so kann schedutil die neuen
# OPP-Punkte unter 1 GHz ueberhaupt waehlen: scaling_min_freq klemmt sonst ab.
#
# Aufruf: lowfreq_set_floor.sh <kHz|auto>
#   auto = tiefste real erreichbare Rate beider Cluster
set -u

WANT=${1:-}
[ -n "$WANT" ] || { echo "Aufruf: $0 <kHz|auto>"; exit 2; }

if [ "$WANT" = auto ]; then
	WANT=$(cut -d' ' -f1 /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies)
fi

case "$WANT" in *[!0-9]*|'') echo "FEHLER: '$WANT' ist keine kHz-Zahl"; exit 2;; esac

# Nur Raten zulassen, die der Taktgeber exakt erzeugt (Teiler 2^a*5^b). Der
# Mux rundet sonst still ab und cpufreq meldet eine falsche Frequenz.
for p in 0 2; do
	grep -qw "$WANT" /sys/devices/system/cpu/cpufreq/policy$p/scaling_available_frequencies ||
		{ echo "FEHLER: $WANT kHz gibt es an policy$p nicht"; exit 2; }
done

echo "$WANT" > /etc/default/cpu-freq-guard
echo "dauerhafte Untergrenze: $WANT kHz"

# Sofort anwenden, ohne Reboot.
for p in 0 2; do
	echo "$WANT" > /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq
done
for p in 0 2; do
	echo "policy$p min=$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq) \
gov=$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_governor) \
cur=$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_cur_freq)"
done
