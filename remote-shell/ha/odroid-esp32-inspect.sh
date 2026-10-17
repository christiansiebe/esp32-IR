#!/bin/bash
# ESP32-Receiver: read-only Inspektion ALLER Seiten, Menues, Felder und Werte.
#
# Laeuft AUF DEM ODROID (der im Heimnetz steht und das ESP32 erreicht):
#   bash odroid-esp32-inspect.sh [IP]     # Default 192.168.178.70
#
# Nur lesend: es werden ausschliesslich GET-Endpunkte abgefragt. Damit laesst
# sich pruefen, ob die Werte so sind wie besprochen (SML, Historie, Sender-Diag,
# Sync-Lag, Uhrzeit, Crash-Journal ...).
set -u

IP="${1:-192.168.178.70}"
BASE="http://$IP"
C() { curl -sS -m 8 "$@"; }
pp() { python3 -m json.tool 2>/dev/null || cat; }
hr() { echo; echo "===== $* ====="; }

hr "0. Erreichbarkeit"
C -o /dev/null -w "  $BASE/ -> HTTP %{http_code}  (%{time_total}s)\n" "$BASE/" \
  || echo "  NICHT erreichbar (falsche IP / anderes Netz?)"

hr "1. /api (Basis-Info)"
C "$BASE/api" | pp

hr "2. /api/sml (Live-Werte)"
C "$BASE/api/sml" | pp

hr "3. /api/dashboard"
C "$BASE/api/dashboard" | pp

hr "4. /api/power-samples"
C "$BASE/api/power-samples" | pp

hr "5. /api/power-history (Kopf)"
C "$BASE/api/power-history" | head -c 1200 | pp

hr "6. /api/rssi (Sender->Empfaenger)"
C "$BASE/api/rssi" | pp

for s in counters crash time_sync history cmd; do
  hr "7. sender-diag section=$s"
  C "$BASE/api/sender-diag?section=$s" | pp
done

hr "7b. sender-diag section=events (Seite 0)"
C "$BASE/api/sender-diag?section=events&page=0" | pp

hr "8. /api/history-pull (Sender-Abgleich / Lag)"
C "$BASE/api/history-pull" | pp

hr "8b. /api/sender (aggregierte Sender-Statusseite)"
C "$BASE/api/sender" | head -c 1500 | pp

hr "9. Seiten: HTTP-Status + Titel"
for p in / /diag /sender /config /console /update /log; do
  code=$(C -o /tmp/_esp_page.html -w '%{http_code}' "$BASE$p")
  title=$(grep -o -m1 '<title>[^<]*</title>' /tmp/_esp_page.html 2>/dev/null | sed 's/<[^>]*>//g')
  printf '  %-9s HTTP %-4s title="%s"\n' "$p" "$code" "$title"
done

hr "10. Formularfelder / Menues (name= / id=)"
for p in /config /update /console; do
  echo "  -- $p --"
  C "$BASE$p" | grep -oE '<(input|select|button|textarea)[^>]*(name|id)="[^"]*"' \
    | sed 's/^/    /' | head -30
done

hr "11. Startseite: Links / Menuepunkte"
C "$BASE/" | grep -oE '<a [^>]*href="[^"]*"' | sed 's/^/    /' | head -30

hr "12. Vorkommen der besprochenen Kennzahlen (Kurzcheck)"
CTR=$(C "$BASE/api/sender-diag?section=counters")
HP=$(C "$BASE/api/history-pull")
for key in cpu_freq_mhz cpu_freq_max_mhz cpu_freq_min_mhz light_sleep_enabled \
           sleep_state uptime_sleep_ms build_profile window_ms transport_losses \
           transport_reorders gap_loss gap_mid parser_resyncs time_syncs \
           time_requests record_count free_heap; do
  hit=$(printf '%s' "$CTR" | grep -o "\"$key\":[^,}]*" | head -1)
  [ -n "$hit" ] && printf '    %s\n' "$hit"
done
for key in caught_up lag_seconds merged_total pages_total errors sessions \
           last_round_ok last_duration_ms; do
  hit=$(printf '%s' "$HP" | grep -o "\"$key\":[^,}]*" | head -1)
  [ -n "$hit" ] && printf '    %s\n' "$hit"
done

echo
echo "=== FERTIG (read-only) ==="
