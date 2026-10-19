#!/usr/bin/env bash
# tunnel.sh — oeffentlicher WSS-Eingang fuer eine Cline-Cloud-Sandbox.
#
# Die Cline-Sandbox hat KEINEN oeffentlichen Port (kein RUNTIME_ID, kein
# work-2-<id>.prod-runtime-Host). Erreichbar wird sie nur ueber einen Tunnel,
# den die Sandbox selbst AUSGEHEND aufbaut. Dieses Skript haelt so einen
# SSH-Reverse-Tunnel (Default serveo.net) und schreibt die URL nach
# run/public-url.txt; supervise.py --status und register_sandbox.py lesen sie.
#
#   bash remote-shell/tunnel.sh start    # Tunnel starten, URL ausgeben
#   bash remote-shell/tunnel.sh status   # laeuft? + aktuelle URL
#   bash remote-shell/tunnel.sh url      # nur die URL
#   bash remote-shell/tunnel.sh stop
#
# Hinweis: Der Relay terminiert TLS; Befehle sind dort im Klartext sichtbar.
# Deshalb: Token nur per Query (bestehendes Protokoll) + Allowlist im Agenten.
set -euo pipefail

BASE="$(cd "$(dirname "$0")" && pwd)"
RUN="$BASE/run"
PORT="${SANDBOX_TERM_PORT:-12001}"
TUNNEL_HOST="${TUNNEL_HOST:-serveo.net}"
URLFILE="$RUN/public-url.txt"
LOGFILE="$RUN/tunnel.log"
PIDFILE="$RUN/tunnel.pid"
PATTERN='https://[a-z0-9-]+\.(serveousercontent\.com|lhr\.life|trycloudflare\.com)'

mkdir -p "$RUN"

alive() {
  local pid
  pid="$(cat "$PIDFILE" 2>/dev/null || true)"
  [ -n "$pid" ] || return 1
  grep -q '^ssh$' "/proc/$pid/comm" 2>/dev/null
}

http_get_ok() {
  [ -s "$URLFILE" ] || return 1
  curl -fsS -m 8 -o /dev/null "$(cat "$URLFILE")/" 2>/dev/null
}

start() {
  if alive; then
    echo "Tunnel laeuft bereits (PID $(cat "$PIDFILE")): $(cat "$URLFILE" 2>/dev/null)"
    return 0
  fi
  rm -f "$URLFILE"
  setsid ssh -o StrictHostKeyChecking=no \
             -o UserKnownHostsFile=/dev/null \
             -o ExitOnForwardFailure=yes \
             -o ServerAliveInterval=15 \
             -o ServerAliveCountMax=3 \
             -R "80:127.0.0.1:$PORT" "$TUNNEL_HOST" \
             >>"$LOGFILE" 2>&1 </dev/null &
  echo $! >"$PIDFILE"
  for _ in $(seq 1 30); do
    local url
    url="$(grep -aoE "$PATTERN" "$LOGFILE" 2>/dev/null | tail -1 || true)"
    if [ -n "$url" ]; then
      printf '%s' "$url" >"$URLFILE"
      echo "Tunnel: $url"
      return 0
    fi
    sleep 1
  done
  echo "Tunnel-URL nicht erschienen - siehe $LOGFILE" >&2
  return 1
}

case "${1:-status}" in
  start)  start ;;
  stop)   alive && kill "$(cat "$PIDFILE")" 2>/dev/null || true; rm -f "$PIDFILE" "$URLFILE"; echo "Tunnel gestoppt." ;;
  url)    cat "$URLFILE" 2>/dev/null || { echo "(keine URL)" >&2; exit 1; } ;;
  status)
    if alive; then
      echo "Tunnel laeuft (PID $(cat "$PIDFILE")): $(cat "$URLFILE" 2>/dev/null)"
      http_get_ok && echo "HTTP-Probe: ok" || echo "HTTP-Probe: fehlgeschlagen"
    else
      echo "Tunnel laeuft NICHT."; exit 1
    fi ;;
  *) echo "Aufruf: $0 start|stop|status|url" >&2; exit 2 ;;
esac
