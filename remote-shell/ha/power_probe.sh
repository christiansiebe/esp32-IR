#!/bin/sh
# Misst, ob ein niedrigerer Takt bei GLEICHER Dauerlast weniger Verlustleistung
# erzeugt. Es gibt keinen Stromsensor am Board (kein INA2xx), gemessen wird
# deshalb die CPU-Temperatur im Gleichgewicht als Proxy.
#
# Wichtig fuer die Aussagekraft: verglichen wird gleiche Last (zwei Busy-Loops
# auf cpu0+cpu1) bei verschiedenem Takt. Bei gleicher Arbeit waere die ENERGIE
# pro Operation konstant (C*V^2) - nur die Leistung in Watt sinkt. Erst die
# feste Dauerlast macht den Unterschied als Temperatur sichtbar.
#
# Alle Loops laufen auf dem little-Cluster (cpu0,1); dessen Takt wird
# festgenagelt. Der big-Cluster bleibt unbelastet, damit nur ein Cluster in die
# Messung eingeht.
#
# Aufruf: power_probe.sh [einlauf_sekunden]
set -u

RUN=${1:-75}
LOG=/root/power_probe.log
p=/sys/devices/system/cpu/cpufreq/policy0

# Hotplug anhalten: sonst aendert sich die Kernzahl zwischen den Punkten.
systemctl stop cpu-hotplug.service 2>/dev/null
LOADS=""
cleanup() {
	[ -n "$LOADS" ] && kill $LOADS 2>/dev/null
	systemctl start cpu-hotplug.service 2>/dev/null
	echo schedutil > $p/scaling_governor 2>/dev/null
	echo 1000000 > $p/scaling_min_freq 2>/dev/null
}
trap 'cleanup' EXIT INT TERM

# Zwei satte Loops auf dem little-Cluster -> konstante Nachfrage.
i=0
while [ $i -lt 2 ]; do
	taskset -c 0,1 sh -c 'while :; do :; done' &
	LOADS="$LOADS $!"
	i=$((i + 1))
done
sleep 2

echo "=== Leistungs-Proxy: 2 Busy-Loops auf cpu0,1, Einlauf ${RUN}s ===" | tee "$LOG"
printf '%-10s %-8s %-9s %-9s\n' Takt_Little Temp_C real_MHz load1 | tee -a "$LOG"

for f in 1000000 500000 250000 100000 24000; do
	echo userspace > $p/scaling_governor
	echo "$f" > $p/scaling_min_freq
	echo "$f" > $p/scaling_setspeed
	sleep "$RUN"
	t=$(cat /sys/class/thermal/thermal_zone0/temp)
	real=$(cat /sys/kernel/debug/clk/cpu_clk_dyn/clk_rate)
	l1=$(cut -d' ' -f1 /proc/loadavg)
	printf '%-10s %-8s %-9s %-9s\n' "$((f / 1000))MHz" "$((t / 1000))" \
		"$((real / 1000000))" "$l1" | tee -a "$LOG"
done

echo '--- Ende ---' | tee -a "$LOG"
