#!/bin/bash
# .openhands/setup.sh — laeuft automatisch bei JEDEM OpenHands-Agent-Start
# (Cloud + lokal), siehe docs.openhands.dev "Repository Customization".
#
# Dies ist die EINZIGE Datei, die im Repo-Root liegen muss (der OpenHands-Hook
# sucht genau hier). Alles andere liegt gesammelt unter `remote-shell/`, damit
# der ESP-IDF-Firmware-Baum (main/, components/, tools/) unberuehrt bleibt.
#
# Zweck: Diese Sandbox sofort Odroid-faehig machen, ohne manuelle Eingabe.
#   - Toolkit aus `remote-shell/` nach /workspace/remote-shell kopieren
#   - Token aus dem Cloud-Secret SANDBOX_TERM_TOKEN setzen
#   - Supervisor starten -> traegt die Sandbox ins GitHub-Register ein
#     -> der Odroid verbindet sich automatisch.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLKIT="$REPO_ROOT/remote-shell"
DEST="${SANDBOX_TERM_DIR:-/workspace/remote-shell}"
RAW="https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/remote-shell"

if [ -x "$TOOLKIT/bootstrap.sh" ]; then
  bash "$TOOLKIT/bootstrap.sh"
  exit 0
fi

# Fallback: Toolkit direkt aus dem Repo ziehen (falls nicht mitgeklont).
mkdir -p "$DEST"
for f in sandbox_term_server.py cmd_send.sh cmd_agent.py supervise.py \
         register_sandbox.py sync-memory.sh OTHER-CHAT.md bootstrap.sh; do
  curl -fsSL "$RAW/$f" -o "$DEST/$f" || true
done
[ -f "$DEST/bootstrap.sh" ] && bash "$DEST/bootstrap.sh" || true
