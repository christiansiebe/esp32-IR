#!/bin/sh
# power-compare - vergleicht Hotplug-Zustaende bei gleicher Last ueber
# Temperatur + Takt (Proxy; das Board hat keinen Stromsensor).
#
# Aufruf (auf dem Board, root): sh power-compare.sh
# Ergebnis: /tmp/power-compare.log
#
# Warum nicht "jobs -p": in dash (dem nicht-interaktiven sh) ist das leer und
# `wait` blockiert dann ewig - die Busy-Loops laufen weiter. Daher werden die
# PIDs explizit gemerkt.
set -u

LOG=/tmp/power-compare.log
CPU=/sys/devices/system/cpu
TZ0=/sys/class/thermal/thermal_zone0/temp
LOADPIDS=""

temp() { awk -v t="$(cat "$TZ0" 2>/dev/null)" 'BEGIN{printf "%.1f", t/1000}'; }

spawn_load() {
    i=0
    while [ "$i" -lt "$1" ]; do
        sh -c 'while :; do :; done' &
        LOADPIDS="$LOADPIDS $!"
        i=$((i + 1))
    done
}
kill_load() {
    for p in $LOADPIDS; do kill -9 "$p" 2>/dev/null; done
    LOADPIDS=""
    wait 2>/dev/null
}

# Misst ueber $2 Sekunden: Mittel little-/big-Takt, online-Kerne, Temp.
measure() {
    i=0; suml=0; sumb=0; sumon=0; n=0
    end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        lf=$(cat "$CPU/cpufreq/policy0/scaling_cur_freq" 2>/dev/null || echo 0)
        bf=$(cat "$CPU/cpufreq/policy2/scaling_cur_freq" 2>/dev/null || echo 0)
        on=$(( $(tr -cd ',' < "$CPU/online" | wc -c) + 1 ))
        suml=$((suml + lf)); sumb=$((sumb + bf)); sumon=$((sumon + on)); n=$((n + 1))
        sleep 1
    done
    [ "$n" -eq 0 ] && n=1
    printf '%s  little_avg=%sMHz  big_avg=%sMHz  online_avg=%s  temp=%sC\n' \
        "$1" "$((suml / n / 1000))" "$((sumb / n / 1000))" \
        "$((sumon / n))" "$(temp)" | tee -a "$LOG"
}

systemctl stop cpu-hotplug.service 2>/dev/null
sleep 2
echo "=== Last: 4 Busy-Threads, je Zustand 60s einschwingen + 90s messen ===" | tee -a "$LOG"

# --- Zustand B: 6 Kerne online, dieselbe Last ---
for c in 1 2 3 4 5; do echo 1 > "$CPU/cpu$c/online" 2>/dev/null; done
sleep 2
spawn_load 4
sleep 60
measure "B: 6 Kerne online, 4 Lastthreads" 90
kill_load
sleep 5

# --- Zustand A: 1 Kern online, dieselbe Last (Vergleich) ---
for c in 1 2 3 4 5; do echo 0 > "$CPU/cpu$c/online" 2>/dev/null; done
sleep 2
spawn_load 4
sleep 60
measure "A: 1 Kern online, 4 Lastthreads" 90
kill_load
sleep 3

for c in 1 2 3 4 5; do echo 1 > "$CPU/cpu$c/online" 2>/dev/null; done
systemctl start cpu-hotplug.service 2>/dev/null
echo "=== fertig, Hotplug-Dienst wieder aktiv ===" | tee -a "$LOG"
