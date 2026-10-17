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
# Die Sysfs-Reads koennen kurz EBUSY liefern, waehrend cpufreq schreibt -> retry.
avail() {
	i=0
	while [ $i -lt 5 ]; do
		cat /sys/devices/system/cpu/cpufreq/policy$1/scaling_available_frequencies 2>/dev/null && return 0
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

for p in 0 2; do
	A=$(avail $p) || { echo "FEHLER: policy$p nicht lesbar"; exit 2; }
	case " $A " in *" $WANT "*) ;; *) echo "FEHLER: $WANT kHz gibt es an policy$p nicht"; exit 2;; esac
done

echo "$WANT" > /etc/default/cpu-freq-guard

# Armbians /usr/lib/armbian/armbian-hardware-optimization liest MIN_SPEED aus
# /etc/default/cpufrequtils und setzt scaling_min_freq damit NOCH NACH
# cpu-freq-guard.service (das Skript laeuft als Kind weiter). Ohne diese Zeile
# wird die Untergrenze beim Boot wieder auf 1.0 GHz gezogen.
if [ -w /etc/default/cpufrequtils ]; then
	sed -i "s/^MIN_SPEED=.*/MIN_SPEED=$WANT/" /etc/default/cpufrequtils
	grep -q '^MIN_SPEED=' /etc/default/cpufrequtils ||
		echo "MIN_SPEED=$WANT" >> /etc/default/cpufrequtils
	echo "cpufrequtils MIN_SPEED: $(sed -n 's/^MIN_SPEED=//p' /etc/default/cpufrequtils)"
fi

echo "dauerhafte Untergrenze: $WANT kHz"

# Sofort anwenden, ohne Reboot. Ein Cluster kann komplett offline sein
# (cpu-hotplug) -> dann ist sein cpufreq-Sysfs EBUSY und wird uebersprungen;
# beim naechsten Online-Nehmen greift MIN_SPEED/der Guard ohnehin wieder.
for p in 0 2; do
	echo "$WANT" > /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq 2>/dev/null || \
		echo "policy$p offline - Untergrenze wird beim Onlinen gesetzt"
done
for p in 0 2; do
	printf 'policy%s min=%s gov=%s cur=%s\n' "$p" \
		"$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq 2>/dev/null || echo offline)" \
		"$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_governor 2>/dev/null || echo offline)" \
		"$(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_cur_freq 2>/dev/null || echo offline)"
done
