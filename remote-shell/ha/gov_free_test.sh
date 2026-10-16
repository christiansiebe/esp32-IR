#!/bin/sh
# Laesst den Governor den Takt selbst waehlen, mit gesenkter Untergrenze.
#
# Ablauf: Untergrenze beider Cluster auf die tiefste Rate setzen, schedutil
# freigeben, dann ueber die Laufzeit Takt, Temperatur, Last und Taktfehler
# protokollieren. Am Ende IMMER zurueck auf 1.0 GHz - auch bei Abbruch.
#
# Echter Takt: little -> cpu_clk_dyn, big -> cpub_clk_dyn (debugfs). Nur dort
# steht die tatsaechlich gefahrene Rate; scaling_cur_freq meldet die Wunschrate.
#
# Sicherheitsnetz: ein Hang wird vom Hardware-Watchdog rebootet, und
# cpu-freq-guard.service stellt beim Boot wieder 1.0 GHz her. Der Test ist
# deshalb selbst dann folgenlos, wenn er schiefgeht.
#
# Aufruf: gov_free_test.sh [laufzeit_sekunden] [idle|load]
set -u

DUR=${1:-180}
MODE=${2:-idle}
LOG=/root/gov_free_test.log

restore() {
	for p in 0 2; do
		echo schedutil > /sys/devices/system/cpu/cpufreq/policy$p/scaling_governor 2>/dev/null
		echo 1000000 > /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq 2>/dev/null
	done
}
trap 'restore' EXIT INT TERM

# Erste Rate der Liste ist die tiefste (aufsteigend sortiert).
lowest() { cut -d' ' -f1 /sys/devices/system/cpu/cpufreq/policy$1/scaling_available_frequencies; }

echo "=== Governor frei, Untergrenze = tiefste Rate (Modus $MODE, ${DUR}s) ===" | tee "$LOG"
for p in 0 2; do
	L=$(lowest $p)
	echo "policy$p Untergrenze -> $L kHz" | tee -a "$LOG"
	echo "$L" > /sys/devices/system/cpu/cpufreq/policy$p/scaling_min_freq
	echo schedutil > /sys/devices/system/cpu/cpufreq/policy$p/scaling_governor
done
echo "gesetzt: $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq) / $(cat /sys/devices/system/cpu/cpufreq/policy2/scaling_min_freq)" | tee -a "$LOG"

[ "$MODE" = load ] && {
	echo '--- Last: 4 Busy-Loops ---' | tee -a "$LOG"
	LOADS=""
	i=0
	while [ $i -lt 4 ]; do
		( while :; do :; done ) &
		LOADS="$LOADS $!"
		i=$((i + 1))
	done
	# Beim Verlassen IMMER aufraeumen, sonst laufen die Loops weiter.
	trap 'restore; kill $LOADS 2>/dev/null' EXIT INT TERM
}

i=0
while [ $i -lt "$DUR" ]; do
	L=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq)
	B=$(cat /sys/devices/system/cpu/cpufreq/policy2/scaling_cur_freq)
	LR=$(cat /sys/kernel/debug/clk/cpu_clk_dyn/clk_rate)
	BR=$(cat /sys/kernel/debug/clk/cpub_clk_dyn/clk_rate)
	T=$(cat /sys/class/thermal/thermal_zone0/temp)
	L1=$(cut -d' ' -f1 /proc/loadavg)
	echo "$(date +%H:%M:%S) little: will=$((L / 1000))kHz real=$((LR / 1000000))MHz | big: will=$((B / 1000))kHz real=$((BR / 1000000))MHz | temp=$((T / 1000))C load=$L1" | tee -a "$LOG"
	sleep 5
	i=$((i + 5))
done

echo '--- Verteilung (time_in_state) ---' | tee -a "$LOG"
for p in 0 2; do
	echo "policy$p:" | tee -a "$LOG"
	cat /sys/devices/system/cpu/cpufreq/policy$p/stats/time_in_state 2>/dev/null | tee -a "$LOG"
done
echo '--- zurueck auf 1.0 GHz ---' | tee -a "$LOG"
