#!/usr/bin/env bash
# Installiert/aktualisiert den Autostart-Hook fuer den Sandbox-Kommando-Server.
# Idempotent: kann beliebig oft laufen. Setzt einen markierten Block in ~/.bashrc.
#
#   bash /workspace/remote-shell/install_autostart.sh
#
# Hinweis: ~/.bashrc ist im Sandbox-Container (Overlay) NICHT dauerhaft; /workspace
# schon. Nach einem Sandbox-Reset dieses Skript erneut aufrufen (oder den Hook ueber
# die Repo-/AGENTS.md-Regel beim ersten Befehl ausfuehren lassen).
set -euo pipefail

BASE="/workspace/remote-shell"
RC="$HOME/.bashrc"
BEGIN="# >>> remote-shell autostart >>>"
END="# <<< remote-shell autostart <<<"

chmod +x "$BASE/supervise.py" "$BASE/cmd_send.sh" 2>/dev/null || true
[ -f "$BASE/token" ] && chmod 600 "$BASE/token" || true

touch "$RC"
# alten Block entfernen (idempotent)
if grep -qF "$BEGIN" "$RC"; then
  sed -i "\|$BEGIN|,\|$END|d" "$RC"
fi

cat >> "$RC" <<EOF
$BEGIN
# Startet den Sandbox-Kommando-Server selbstheilend (einmal pro Shell).
if [ -z "\${REMOTE_SHELL_AUTOSTART:-}" ] && [ -f "$BASE/supervise.py" ]; then
  export REMOTE_SHELL_AUTOSTART=1
  python3 "$BASE/supervise.py" >/dev/null 2>&1 || true
fi
$END
EOF

echo "Autostart-Hook in $RC installiert."
python3 "$BASE/supervise.py" --status || true
