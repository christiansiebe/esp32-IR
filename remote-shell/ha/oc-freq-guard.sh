#!/bin/sh
# Caps the big-cluster maximum frequency at the stock value.
#
# Why this exists: oc.dts adds a 2472 MHz OPP above the stock 2400 MHz edge.
# cpufreq-dt sets a policy's maximum to the highest OPP it sees, so without a
# cap the board would boot straight into the overclock point - which runs
# UNDERVOLTED (the PWM regulator is already at 100 % duty at 2400 MHz). This
# service pins scaling_max_freq back to the stock maximum right after sysinit,
# so the OC point is only reachable by an explicit runtime raise.
#
# Cap is read from /etc/default/oc-freq-guard (integer kHz, default 2400000).
# A cluster whose hardware maximum is already below the cap is left alone.
CAP=$(tr -dc 0-9 < /etc/default/oc-freq-guard 2>/dev/null)
[ -n "$CAP" ] || CAP=2400000

# OC_FREQ_GUARD_SYS ist beim echten Boot leer; der Praefix dient nur den Tests.
SYS=${OC_FREQ_GUARD_SYS:-/sys/devices/system/cpu/cpufreq}

for p in "$SYS"/policy*; do
        [ -w "$p/scaling_max_freq" ] || continue
        hard=$(cat "$p/cpuinfo_max_freq" 2>/dev/null)
        target=$CAP
        if [ -n "$hard" ] && [ "$hard" -lt "$target" ]; then
                target=$hard
        fi
        # Nie unter die aktuelle Untergrenze deckeln (sonst EINVAL).
        min=$(cat "$p/scaling_min_freq" 2>/dev/null)
        if [ -n "$min" ] && [ "$target" -lt "$min" ]; then
                echo "$min" > "$p/scaling_max_freq" 2>/dev/null
                logger -t oc-freq-guard "Warnung: $p Untergrenze $min > Cap $target"
                continue
        fi
        echo "$target" > "$p/scaling_max_freq" 2>/dev/null || \
                logger -t oc-freq-guard "scaling_max_freq auf $p nicht setzbar (EBUSY?)"
done
