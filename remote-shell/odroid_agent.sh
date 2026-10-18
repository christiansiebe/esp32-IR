#!/usr/bin/env bash
# Device side of the reverse command channel: run this ON THE ODROID (or any
# LAN host that can reach the ESP32 and the internet). It polls the sandbox's
# public URL, runs each queued command locally and posts the output back.
#
#   CHANNEL_KEY=<key> ./odroid_agent.sh            # poll forever
#   CHANNEL_KEY=<key> ./odroid_agent.sh --once     # one poll (for cron/systemd)
#
# Needs: curl. No other dependency.
set -u
CHANNEL_URL="${CHANNEL_URL:-https://work-2-icvijgkachqdohvq.prod-runtime.all-hands.dev}"
CHANNEL_KEY="${CHANNEL_KEY:?CHANNEL_KEY ist nicht gesetzt}"
CMD_TIMEOUT="${CMD_TIMEOUT:-240}"    # harte Obergrenze pro Befehl (Sekunden)
POLL_TIMEOUT="${POLL_TIMEOUT:-20}"   # /next
POST_TIMEOUT="${POST_TIMEOUT:-30}"   # /result
ONCE=0
[ "${1:-}" = "--once" ] && ONCE=1

log() { echo "[$(date -Is)] $*"; }

poll() {
    local resp id cmd out rc
    resp="$(curl -sS -m "$POLL_TIMEOUT" -H "X-Channel-Key: $CHANNEL_KEY" \
                 "$CHANNEL_URL/next" 2>/dev/null)" || return 0
    [ -z "$resp" ] && return 0
    id="$(printf '%s\n' "$resp" | sed -n '1s/^ID=//p')"
    cmd="$(printf '%s\n' "$resp" | sed '1d')"
    [ -z "$id" ] && return 0
    log "ausfuehren $id: $cmd"
    # Harte Obergrenze: ein Befehl, der haengt (z. B. ein curl ohne eigenes -m),
    # darf den Poll-Loop nicht blockieren. -k schickt SIGKILL nach, falls der
    # Befehl das TERM ignoriert.
    out="$(timeout -k 10 "$CMD_TIMEOUT" bash -lc "$cmd" 2>&1)"; rc=$?
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
        out="${out}
[channel] Befehl nach ${CMD_TIMEOUT}s abgebrochen (timeout) - Loop laeuft weiter"
        log "Befehl $id nach ${CMD_TIMEOUT}s abgebrochen"
    fi
    curl -sS -m "$POST_TIMEOUT" -X POST -H "X-Channel-Key: $CHANNEL_KEY" \
         --data-binary "$out" "$CHANNEL_URL/result?id=$id" >/dev/null 2>&1 \
        || log "Ergebnis $id konnte nicht gesendet werden (Sandbox offline?)"
    log "Ergebnis gesendet ($(printf '%s' "$out" | wc -c) Bytes, rc=$rc)"
}

if [ "$ONCE" = "1" ]; then
    poll
    exit 0
fi
log "Kanal-Agent laeuft gegen $CHANNEL_URL (Ctrl-C beendet, Befehl-Timeout ${CMD_TIMEOUT}s)"
# Selbstheilung: der Poll-Loop darf nie enden, egal was in poll() passiert.
while true; do
    poll || log "poll() meldete einen Fehler - Loop laeuft weiter"
    sleep 5
done
