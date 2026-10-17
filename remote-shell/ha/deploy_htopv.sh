#!/bin/sh
# Installiert die htopv-Werkzeuge dauerhaft auf dem Odroid.
#
# Was es macht:
#   - /usr/local/bin/cpu-volt-watch   (Panel: Takt + Rails + Temperaturen)
#   - /usr/local/bin/cpu-volt-line    (Einzeiler fuer die tmux-Statuszeile)
#   - /usr/local/bin/htopv            (htop im tmux mit Spannungs-Statuszeile)
#   - /etc/htopv.tmux.conf            (tmux-Konfig fuer htopv)
#   - htoprc: show_cpu_frequency=1    (per-Kern-MHz in htop)
#
# Voraussetzungen: htop und tmux installiert (apt install htop tmux).
# Aufruf: deploy_htopv.sh [--no-htoprc]
set -eu

HERE=$(dirname "$(readlink -f "$0")")
NO_HTOPRC=0
[ "${1:-}" = "--no-htoprc" ] && NO_HTOPRC=1

for f in cpu-volt-watch cpu-volt-line htopv htopv.tmux.conf; do
	[ -f "$HERE/$f" ] || { echo "FEHLER: $HERE/$f fehlt"; exit 1; }
done

command -v tmux >/dev/null 2>&1 || { echo "FEHLER: tmux fehlt (apt install tmux)"; exit 1; }
command -v htop >/dev/null 2>&1 || { echo "FEHLER: htop fehlt (apt install htop)"; exit 1; }

echo '== 1. Skripte installieren =='
install -m 0755 "$HERE/cpu-volt-watch" /usr/local/bin/cpu-volt-watch
install -m 0755 "$HERE/cpu-volt-line"  /usr/local/bin/cpu-volt-line
install -m 0755 "$HERE/htopv"          /usr/local/bin/htopv
install -m 0644 "$HERE/htopv.tmux.conf" /etc/htopv.tmux.conf

if [ "$NO_HTOPRC" -eq 0 ]; then
	echo '== 2. htoprc: show_cpu_frequency=1 =='
	rc="${HOME:-/root}/.config/htop/htoprc"
	mkdir -p "$(dirname "$rc")"
	if [ -f "$rc" ]; then
		if grep -q '^show_cpu_frequency=' "$rc"; then
			sed -i 's/^show_cpu_frequency=.*/show_cpu_frequency=1/' "$rc"
		else
			echo 'show_cpu_frequency=1' >> "$rc"
		fi
	else
		echo 'show_cpu_frequency=1' > "$rc"
	fi
fi

sync

echo '== 3. Zustand =='
/usr/local/bin/cpu-volt-line; echo
echo '== fertig - Aufruf: htopv =='
