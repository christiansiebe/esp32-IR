#!/bin/bash
# .openhands/setup.sh — laeuft automatisch bei JEDEM OpenHands-Agent-Start
# (Cloud + lokal), siehe docs.openhands.dev "Repository Customization".
#
# Zweck: Diese Sandbox sofort Odroid-faehig machen, ohne manuelle Eingabe.
#   - Toolkit nach /workspace/remote-shell kopieren
#   - Token aus dem Cloud-Secret SANDBOX_TERM_TOKEN setzen
#   - Supervisor starten -> traegt die Sandbox ins GitHub-Register ein
#     -> der Odroid verbindet sich automatisch.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLKIT="$REPO_ROOT/tools/remote-shell"
DEST="${SANDBOX_TERM_DIR:-/workspace/remote-shell}"

if [ -x "$TOOLKIT/bootstrap.sh" ]; then
  bash "$TOOLKIT/bootstrap.sh"
  exit 0
fi

# Fallback: Toolkit direkt aus dem Repo ziehen (falls nicht mitgeklont).
mkdir -p "$DEST"
base="https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/tools/remote-shell"
for f in sandbox_term_server.py cmd_send.sh cmd_agent.py supervise.py \
         register_sandbox.py sync-memory.sh OTHER-CHAT.md bootstrap.sh; do
  curl -fsSL "$base/$f" -o "$DEST/$f" || true
done
[ -f "$DEST/bootstrap.sh" ] && bash "$DEST/bootstrap.sh" || true
