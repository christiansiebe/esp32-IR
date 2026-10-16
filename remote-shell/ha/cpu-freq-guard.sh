#!/bin/sh
# Pins the CPU minimum frequency to a safe floor.
#
# The lowfreq device-tree overlay adds OPP points below 1 GHz. Until they have
# been validated they must never be selected automatically: cpufreq would start
# the policy at the lowest OPP, so a bad point could hang the boot before any
# userspace fixup runs. Two guards cover that:
#
#   * cpufreq.default_governor=performance (boot arg) keeps the early-boot
#     governor at maximum frequency.
#   * this service raises scaling_min_freq and restores the normal governor as
#     soon as sysinit is done.
#
# Floor is read from /etc/default/cpu-freq-guard (integer kHz, default 1000000).
# The governor is taken from /etc/default/cpufrequtils so this stays in step
# with Armbian's own hardware optimisation.
FLOOR=$(tr -dc 0-9 < /etc/default/cpu-freq-guard 2>/dev/null)
[ -n "$FLOOR" ] || FLOOR=1000000

GOV=$(sed -n 's/^GOVERNOR=\(.*\)$/\1/p' /etc/default/cpufrequtils 2>/dev/null)
[ -n "$GOV" ] || GOV=schedutil

for p in /sys/devices/system/cpu/cpufreq/policy*; do
	[ -w "$p/scaling_min_freq" ] && echo "$FLOOR" > "$p/scaling_min_freq" 2>/dev/null
	grep -qw "$GOV" "$p/scaling_available_governors" 2>/dev/null &&
		[ -w "$p/scaling_governor" ] && echo "$GOV" > "$p/scaling_governor" 2>/dev/null
done
