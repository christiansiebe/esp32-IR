#!/bin/bash
# Installiert/aktualisiert die 2 Odroid-Worker (idempotent). Auf dem Odroid als root.
#   bash deploy_workers.sh
# Backup: bestehende Units/Configs werden nach /root/worker/backup-<TS>/ gesichert.
set -u
TS=$(date +%Y%m%d-%H%M%S)
WDIR=/root/worker
BK=$WDIR/backup-$TS
mkdir -p "$WDIR"/{queue,running,results} "$BK"

# 1. Backups
for u in /etc/systemd/system/odroid-worker@.service; do
  [ -f "$u" ] && cp -a "$u" "$BK/" && echo "backup: $u"
done
[ -d /root/worker/queue ] && cp -a /root/worker/queue "$BK/queue-before" 2>/dev/null
echo "backup-dir: $BK"

# 2. Unit installieren (Template)
cat > /etc/systemd/system/odroid-worker@.service <<'UNIT'
[Unit]
Description=Odroid OpenHands worker %i (local agent server, no cloud quota)
After=network-online.target openhands-agent.service
Wants=network-online.target
Requires=openhands-agent.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /root/worker/worker.py --name worker%i --dir /root/worker --timeout 3600
Restart=always
RestartSec=10
Environment=WORKER_OH_BIN=/usr/local/bin/oh
Nice=10

[Install]
WantedBy=multi-user.target
UNIT
chmod 644 /etc/systemd/system/odroid-worker@.service

# 3. Worker-Skript muss vorhanden sein (wird per Kanal mitgeliefert)
if [ ! -f "$WDIR/worker.py" ]; then
  echo "FEHLER: $WDIR/worker.py fehlt (zuerst hochladen)."
  exit 1
fi
chmod +x "$WDIR/worker.py"

# 4. Dienste aktivieren
systemctl daemon-reload
systemctl enable --now odroid-worker@1.service odroid-worker@2.service 2>&1 | tail -2

# 5. Status
echo "=== status ==="
for i in 1 2; do
  printf 'worker%s: enabled=%s active=%s\n' "$i" \
    "$(systemctl is-enabled odroid-worker@$i 2>&1)" \
    "$(systemctl is-active odroid-worker@$i 2>&1)"
done
echo "=== RAM ==="; free -m | head -2
