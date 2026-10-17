#!/bin/bash
# Odroid-Bootstrap: Kommando-Kanal-Agent auf die AKTUELLE Sandbox zeigen.
#
# Nach jedem Sandbox-Reset aendern sich Sandbox-ID (URL) und Token. Dieses
# Skript aktualisiert die systemd-Unit `odroid-cmd-agent.service` (Token +
# ExecStart-URL) und startet den Agenten neu.
#
# Aufruf AUF DEM ODROID (als root):
#   bash odroid-bootstrap.sh 'wss://work-2-<id>.prod-runtime.all-hands.dev/' '<token>'
#
# Es aendert NUR die beiden Zeilen Environment=SANDBOX_TERM_TOKEN= und
# ExecStart=; alles andere in der Unit bleibt unangetastet.
set -euo pipefail

URL="${1:?Aufruf: odroid-bootstrap.sh <wss-url> <token>}"
TOKEN="${2:?Aufruf: odroid-bootstrap.sh <wss-url> <token>}"

UNIT=/etc/systemd/system/odroid-cmd-agent.service
[ -f "$UNIT" ] || { echo "FEHLER: $UNIT nicht gefunden"; exit 1; }

cp "$UNIT" "$UNIT.bak-$(date +%Y%m%d-%H%M%S)"

sed -i -E "s|^(Environment=SANDBOX_TERM_TOKEN=).*|\1${TOKEN}|" "$UNIT"
sed -i -E "s|^(ExecStart=.*python3[[:space:]]+[^[:space:]]+)[[:space:]]+.*|\1 ${URL}|" "$UNIT"
chmod 600 "$UNIT"

systemctl daemon-reload
systemctl restart odroid-cmd-agent.service
sleep 2
systemctl --no-pager --lines=5 status odroid-cmd-agent.service || true
echo "---"
echo "Agent neu gestartet. In der Sandbox pruefen: tail -3 /tmp/cmdchannel/audit.log"
