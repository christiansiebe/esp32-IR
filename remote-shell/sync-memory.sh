#!/usr/bin/env bash
# sync-memory.sh — gemeinsames Gedaechtnis zwischen Sandbox und GitHub abgleichen.
#
#   bash sync-memory.sh pull    # GitHub -> /workspace/MEMORY.md (Live-Kopie)
#   bash sync-memory.sh push    # /workspace/MEMORY.md -> GitHub
#   bash sync-memory.sh loop    # alle N s pullen (Hintergrund; fuer supervise.py)
#   bash sync-memory.sh show    # Status/Info
#
# Quelle der Wahrheit: GitHub-Datei MEMORY_PATH im Repo (Branch BRANCH).
# Die Live-Kopie /workspace/MEMORY.md wird jedem Chat als User-Memory injiziert.
#
# KEINE Geheimnisse in der Datei — das Repo ist oeffentlich.
set -euo pipefail

OWNER="${MEM_OWNER:-christiansiebe}"
REPO="${MEM_REPO:-esp32-IR}"
BRANCH="${MEM_BRANCH:-cline/power-history-diag}"
MEMORY_PATH="${MEM_PATH:-remote-shell/memory/SHARED.md}"
LIVE="${MEM_LIVE:-/workspace/MEMORY.md}"
INTERVAL="${MEM_INTERVAL:-90}"

API="https://api.github.com"
READ_TOKEN="${GITHUB_TOKEN:-${GITHUB_WRITE_TOKEN:-}}"
WRITE_TOKEN="${GITHUB_WRITE_TOKEN:-${GITHUB_TOKEN:-}}"

do_pull() {
  local tmp; tmp="$(mktemp)"
  local -a auth=()
  [ -n "$READ_TOKEN" ] && auth=(-H "Authorization: Bearer $READ_TOKEN")
  # Oeffentliches Repo -> Pull funktioniert auch OHNE Token (Loop braucht kein Secret).
  curl -s -m20 "${auth[@]}" \
       -H "Accept: application/vnd.github.raw" \
       "$API/repos/$OWNER/$REPO/contents/$MEMORY_PATH?ref=$BRANCH" -o "$tmp" || true
  # Fehlerantwort (JSON/404) statt Markdown -> Live-Kopie NICHT ueberschreiben.
  if ! head -1 "$tmp" | grep -q '^# '; then
    echo "pull: GitHub-Datei nicht lesbar (existiert sie schon?) - unveraendert."
    rm -f "$tmp"; return 1
  fi
  if cmp -s "$tmp" "$LIVE"; then
    echo "pull: schon aktuell."
  else
    cp "$tmp" "$LIVE"
    echo "pull: aktualisiert -> $LIVE"
  fi
  rm -f "$tmp"
}

do_push() {
  [ -f "$LIVE" ] || { echo "push: $LIVE fehlt."; return 1; }
  curl -s -m20 -H "Authorization: Bearer $WRITE_TOKEN" \
       -H "Accept: application/vnd.github+json" \
       "$API/repos/$OWNER/$REPO/contents/$MEMORY_PATH?ref=$BRANCH" -o /tmp/_memget.json || true
  local sha
  sha="$(python3 -c "import json;print(json.load(open('/tmp/_memget.json')).get('sha',''))" 2>/dev/null || true)"
  python3 - "$LIVE" "$sha" "$BRANCH" > /tmp/_membody.json <<'PY'
import base64, json, sys
live, sha, branch = sys.argv[1], sys.argv[2], sys.argv[3]
content = base64.b64encode(open(live, 'rb').read()).decode()
d = {'message': 'memory: sync shared memory', 'content': content, 'branch': branch}
if sha:
    d['sha'] = sha
print(json.dumps(d))
PY
  curl -s -m25 -X PUT -H "Authorization: Bearer $WRITE_TOKEN" \
       -H "Accept: application/vnd.github+json" \
       "$API/repos/$OWNER/$REPO/contents/$MEMORY_PATH" --data @/tmp/_membody.json \
       -o /tmp/_mempush.json -w "push: HTTP %{http_code}\n" || true
  python3 -c "import json;d=json.load(open('/tmp/_mempush.json'));print('  commit:',((d.get('commit') or {}).get('sha') or d.get('message',''))[:10])" 2>/dev/null || true
}

do_loop() {
  while true; do do_pull || true; sleep "$INTERVAL"; done
}

case "${1:-show}" in
  pull) do_pull ;;
  push) do_push ;;
  loop) do_loop ;;
  show)
    echo "Repo:   $OWNER/$REPO @ $BRANCH"
    echo "GitHub: $MEMORY_PATH"
    echo "Live:   $LIVE  ($(wc -l < "$LIVE" 2>/dev/null || echo 0) Zeilen)"
    echo "Read-Token:  $([ -n "$READ_TOKEN" ] && echo ja || echo nein)"
    echo "Write-Token: $([ -n "$WRITE_TOKEN" ] && echo ja || echo nein)"
    ;;
  *) echo "Aufruf: sync-memory.sh {pull|push|loop|show}"; exit 2 ;;
esac
