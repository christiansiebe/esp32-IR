#!/usr/bin/env bash
# Installiert den IDF-Monitor-Web auf dem Odroid (aarch64/Debian 12).
# Firmware bleibt unveraendert; das Werkzeug ist reines Host-Tooling.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
venv="${IDF_MONITOR_VENV:-/opt/idf-monitor}"
port="${IDF_MONITOR_WEB_PORT:-7681}"

echo "== 1/4  esp-idf-monitor in eigenes Venv ($venv) =="
if command -v python3 -m venv >/dev/null 2>&1; then :; else
    sudo apt-get update -y && sudo apt-get install -y python3-venv python3-pip
fi
sudo python3 -m venv "$venv" 2>/dev/null || python3 -m venv "$venv"
"$venv/bin/pip" install --upgrade pip
"$venv/bin/pip" install "esp-idf-monitor"

echo "== 2/4  Token erzeugen (nur Host, nie ins oeffentliche Repo) =="
token_dir="${IDF_MONITOR_STATE:-$HOME/.esp-idf-monitor-web}"
mkdir -p "$token_dir"
if [[ ! -s "$token_dir/token" ]]; then
    python3 -c "import secrets;print(secrets.token_urlsafe(32))" > "$token_dir/token"
    chmod 600 "$token_dir/token"
    echo "   neuer Token: $token_dir/token"
else
    echo "   vorhandener Token bleibt: $token_dir/token"
fi

echo "== 3/4  systemd-Unit =="
unit=/etc/systemd/system/idf-monitor-web.service
sudo tee "$unit" >/dev/null <<EOF
[Unit]
Description=Serieller IDF-Monitor im Browser (Odroid)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER
Environment=IDF_MONITOR_WEB_PORT=$port
Environment=IDF_MONITOR_WEB_TOKEN_FILE=$token_dir/token
Environment=IDF_MONITOR_BIN=$venv/bin/esp-idf-monitor
ExecStart=/usr/bin/python3 $here/idf_monitor_web.py --port auto --baud 115200
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now idf-monitor-web.service

echo "== 4/4  Status =="
sleep 1
systemctl --no-pager --lines=5 status idf-monitor-web.service || true
tok="$(cat "$token_dir/token")"
echo
echo "Browser: http://$(hostname -I | awk '{print $1}'):$port/?token=$tok"
echo "(Der Token steht nur hier lokal; nicht committen.)"
