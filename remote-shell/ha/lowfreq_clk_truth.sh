#!/bin/sh
# Zeigt fuer jede Rate unter 1 GHz den echten Endtakt (cpu_clk_dyn) und die
# Mux-/Teilerwahl. cpufreq meldet scaling_cur_freq, das kann gerundet sein -
# cpu_clk_dyn ist der tatsaechliche CPU-Takt.
for c in 0 2; do
	p=/sys/devices/system/cpu/cpufreq/policy$c
	echo "=== policy$c ==="
	echo userspace > $p/scaling_governor
	echo 1000000 > $p/scaling_min_freq
	for f in $(cat $p/scaling_available_frequencies); do
		[ "$f" -lt 1000000 ] || continue
		echo "$f" > $p/scaling_min_freq
		echo "$f" > $p/scaling_setspeed
		sleep 0.15
		sel=$(cat /sys/kernel/debug/clk/cpu_clk_dyn0_sel/clk_rate 2>/dev/null)
		[ "$c" = 2 ] && sel=$(cat /sys/kernel/debug/clk/cpub_clk_dyn0_sel/clk_rate 2>/dev/null)
		dyn=$(cat /sys/kernel/debug/clk/cpu_clk_dyn/clk_rate 2>/dev/null)
		[ "$c" = 2 ] && dyn=$(cat /sys/kernel/debug/clk/cpub_clk_dyn/clk_rate 2>/dev/null)
		div=$(cat /sys/kernel/debug/clk/cpu_clk_dyn0_div/clk_rate 2>/dev/null)
		echo "soll=$f  cur=$(cat $p/scaling_cur_freq)  mux=$sel  dyn=$dyn  div0=$div"
	done
	echo 1000000 > $p/scaling_min_freq
	echo schedutil > $p/scaling_governor
done
