#!/usr/bin/env bash
# Install the reverse-channel agent on the Odroid and register it with the
# sandbox. Run ON THE ODROID as root:
#
#   sudo ./install_odroid_agent.sh
#
# It generates a fresh key locally, registers it with the sandbox once (the
# sandbox only accepts the first registration after the agent opened it), writes
# /etc/esp32-channel.env (root-only) and starts the systemd service. The key is
# never printed and never has to be copied by hand.
set -euo pipefail

CHANNEL_URL="${CHANNEL_URL:-https://work-2-icvijgkachqdohvq.prod-runtime.all-hands.dev}"
CHANNEL_USER="${CHANNEL_USER:-${SUDO_USER:-$(id -un)}}"
DEST=/opt/esp32-channel
ENV=/etc/esp32-channel.env

install -d "$DEST"
install -m 0755 odroid_agent.sh "$DEST/odroid_agent.sh"

if [ -s "$ENV" ]; then
    echo "vorhandene $ENV gefunden - Registrierung wird uebersprungen."
else
    KEY="$(head -c 32 /dev/urandom | base64 | tr -dc 'A-Za-z0-9_-')"
    printf 'CHANNEL_URL=%s\nCHANNEL_KEY=%s\n' "$CHANNEL_URL" "$KEY" > "$ENV"
    chmod 600 "$ENV"
    echo "registriere beim Sandbox ..."
    if curl -fsS -m 20 -H "X-Channel-Key: $KEY" "$CHANNEL_URL/register" >/dev/null; then
        echo "  Registrierung ok - Sandbox kennt den Key jetzt."
    else
        echo "  WARNUNG: Registrierung fehlgeschlagen (Sandbox offline?)."
        echo "  Erneut versuchen mit:  sudo ./install_odroid_agent.sh"
    fi
fi

cat > /etc/systemd/system/esp32-channel.service <<EOF
[Unit]
Description=ESP32 reverse command channel agent
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=$CHANNEL_USER
WorkingDirectory=$DEST
EnvironmentFile=$ENV
ExecStart=/bin/bash $DEST/odroid_agent.sh
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now esp32-channel.service
echo "installiert. Status:"
systemctl --no-pager --full status esp32-channel.service | head -12
