#!/usr/bin/env bash
# tunnel_watch.sh — haelt den Tunnel am Leben und veroeffentlicht die URL.
#
#   bash remote-shell/tunnel_watch.sh start|stop|status
#
# Ablauf (alle 20 s):
#   1. Tunnel tot/abgelaufen? -> tunnel.sh start (neue URL moeglich)
#   2. URL geaendert? -> publish_url.py (GitHub-Registry, Branch channel-registry)
# Der Odroid liest die Registry alle 60 s -> er verbindet sich selbst neu.
set -uo pipefail
BASE="$(cd "$(dirname "$0")" && pwd)"
RUN="$BASE/run"
PIDFILE="$RUN/tunnel-watch.pid"
LOGFILE="$RUN/tunnel-watch.log"
PUBLISHED="$RUN/published-url.txt"

mkdir -p "$RUN"

alive() {
  local pid; pid="$(cat "$PIDFILE" 2>/dev/null || true)"
  [ -n "$pid" ] || return 1
  grep -qx 'bash\|tunnel_watch.sh' "/proc/$pid/comm" 2>/dev/null || \
    grep -q 'tunnel_watch' "/proc/$pid/cmdline" 2>/dev/null
}

watch() {
  while true; do
    if ! bash "$BASE/tunnel.sh" status >/dev/null 2>&1; then
      echo "$(date -u +%FT%TZ) tunnel down -> restart" >>"$LOGFILE"
      bash "$BASE/tunnel.sh" start >>"$LOGFILE" 2>&1
    fi
    if [ -s "$RUN/public-url.txt" ]; then
      local url now
      url="$(cat "$RUN/public-url.txt")"
      now="$(cat "$PUBLISHED" 2>/dev/null || true)"
      if [ "$url" != "$now" ]; then
        if python3 "$BASE/publish_url.py" "$url" >>"$LOGFILE" 2>&1; then
          printf '%s' "$url" >"$PUBLISHED"
        fi
      fi
    fi
    sleep 20
  done
}

case "${1:-status}" in
  start)
    if alive; then echo "Watcher laeuft (PID $(cat "$PIDFILE"))"; exit 0; fi
    setsid bash "$0" __loop >>"$LOGFILE" 2>&1 </dev/null &
    echo $! >"$PIDFILE"
    sleep 1
    echo "Watcher gestartet (PID $(cat "$PIDFILE"))" ;;
  stop)
    alive && kill "$(cat "$PIDFILE")" 2>/dev/null || true
    rm -f "$PIDFILE"; echo "Watcher gestoppt." ;;
  status)
    if alive; then
      echo "Watcher laeuft (PID $(cat "$PIDFILE"))"
      echo "URL: $(cat "$RUN/public-url.txt" 2>/dev/null || echo '-')"
      echo "publiziert: $(cat "$PUBLISHED" 2>/dev/null || echo '-')"
    else
      echo "Watcher laeuft NICHT."; exit 1
    fi ;;
  __loop) watch ;;
  *) echo "Aufruf: $0 start|stop|status" >&2; exit 2 ;;
esac
