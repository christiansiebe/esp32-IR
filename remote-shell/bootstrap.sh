#!/usr/bin/env bash
# bootstrap.sh — macht diese Sandbox sofort Odroid-faehig (idempotent).
#
# Wird automatisch von .openhands/setup.sh bei jedem Agent-Start aufgerufen.
# Kopiert den Kanal sandbox-weit nach /workspace/remote-shell, setzt den
# geteilten Token (Cloud-Secret SANDBOX_TERM_TOKEN) und startet den Supervisor,
# der sich selbst im GitHub-Register eintraegt (-> Odroid findet die Sandbox).
#
# Defaults (per Env ueberschreibbar):
#   MEM_OWNER=christiansiebe MEM_REPO=esp32-IR MEM_BRANCH=main
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="${SANDBOX_TERM_DIR:-/workspace/remote-shell}"
TOKEN_FILE="$DEST/token"

export MEM_OWNER="${MEM_OWNER:-christiansiebe}"
export MEM_REPO="${MEM_REPO:-esp32-IR}"
export MEM_BRANCH="${MEM_BRANCH:-main}"
export SANDBOX_TERM_REGISTRY="${SANDBOX_TERM_REGISTRY:-https://raw.githubusercontent.com/$MEM_OWNER/$MEM_REPO/$MEM_BRANCH/remote-shell/memory/sandboxes.json}"

echo "bootstrap: $SRC -> $DEST"
mkdir -p "$DEST"

# Kanal-Dateien kopieren (nur die noetigen, flach). SRC==DEST -> nichts tun.
if [ "$SRC" != "$DEST" ]; then
  for f in sandbox_term_server.py cmd_send.sh cmd_agent.py supervise.py \
           register_sandbox.py sync-memory.sh OTHER-CHAT.md; do
    [ -f "$SRC/$f" ] && cp -a "$SRC/$f" "$DEST/$f"
  done
fi

# Geteilter Token aus dem Cloud-Secret in die Datei schreiben (Rotation moeglich:
# Datei einfach ueberschreiben; der Server liest sie pro Verbindung).
if [ -n "${SANDBOX_TERM_TOKEN:-}" ]; then
  printf '%s' "$SANDBOX_TERM_TOKEN" > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "bootstrap: Token gesetzt (${#SANDBOX_TERM_TOKEN} Zeichen)"
else
  echo "bootstrap: WARNUNG - SANDBOX_TERM_TOKEN fehlt (Cloud-Secret?)."
fi

# Geteilte Memory-Datei, falls noch nicht da (Inhalt kommt per sync-memory.sh).
if [ ! -f /workspace/MEMORY.md ]; then
  printf '# Shared Memory — OpenHands chats\n\nwird von sync-memory.sh gefuellt.\n' > /workspace/MEMORY.md
  bash "$DEST/sync-memory.sh" pull || true
fi

# Supervisor starten (idempotent, selbstheilend) + Registrierung.
python3 "$DEST/supervise.py" || true
echo "bootstrap: fertig. Kanal: bash $DEST/cmd_send.sh \"<befehl>\""
