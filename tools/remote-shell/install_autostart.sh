#!/bin/bash
# Installiert einen ~/.bashrc-Hook, der den Kanal-Server (supervise.py)
# bei Bedarf automatisch startet. Die Sandbox hat kein systemd/cron, daher
# ist ein Shell-Hook der einfachste Autostart.
#
# Aufruf (in der Sandbox):
#   bash tools/remote-shell/install_autostart.sh
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SUP="$HERE/supervise.py"
MARK="# remote-shell autostart"

if grep -qF "$MARK" "$HOME/.bashrc" 2>/dev/null; then
  echo "Autostart-Hook ist schon in $HOME/.bashrc"
  exit 0
fi

cat >> "$HOME/.bashrc" <<EOF

$MARK
if ! python3 "$SUP" --status 2>/dev/null | grep -q 'supervisor: laeuft'; then
  nohup python3 "$SUP" > /dev/null 2>&1 &
  disown
fi
EOF

echo "Autostart-Hook zu $HOME/.bashrc hinzugefuegt:"
echo "  $SUP"
