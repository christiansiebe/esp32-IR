#!/bin/sh
# fix-sta-powersave.sh - apply powersave ON to the STA VIF after NM connects.
# rc.local runs before NM is up, and Armbian's global wifi.powersave=2 can win,
# so a post-NM oneshot service is the reliable place.
set -e

cat > /usr/local/sbin/odroid-sta-powersave.sh <<'EOF'
#!/bin/sh
# STA VIF (the one NM has connected) -> powersave on. Retry a few times: NM may
# still be connecting when this runs.
i=0
while [ "$i" -lt 20 ]; do
    STA=$(nmcli -t -f DEVICE,STATE dev status 2>/dev/null | awk -F: '/^wlx/ && $2=="connected"{print $1; exit}')
    if [ -n "$STA" ]; then
        /usr/sbin/iw dev "$STA" set power_save on 2>/dev/null && exit 0
    fi
    i=$((i+1)); sleep 3
done
exit 0
EOF
chmod 755 /usr/local/sbin/odroid-sta-powersave.sh

cat > /etc/systemd/system/odroid-sta-powersave.service <<'EOF'
[Unit]
Description=Enable WLAN powersave on the STA VIF (after NetworkManager)
After=NetworkManager.service network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/odroid-sta-powersave.sh

[Install]
WantedBy=multi-user.target
EOF

# NM re-applies on every connect, so also hook the profile via a dispatcher
mkdir -p /etc/NetworkManager/dispatcher.d
cat > /etc/NetworkManager/dispatcher.d/90-odroid-sta-powersave <<'EOF'
#!/bin/sh
# NM dispatcher: when a wlx STA comes up, force powersave on (Armbian's global
# wifi.powersave=2 would otherwise turn it off).
[ "$2" = "up" ] || exit 0
case "$1" in wlx*) /usr/sbin/iw dev "$1" set power_save on 2>/dev/null ;; esac
exit 0
EOF
chmod 755 /etc/NetworkManager/dispatcher.d/90-odroid-sta-powersave

systemctl daemon-reload
systemctl enable odroid-sta-powersave.service 2>&1 | tail -1
echo "installed"
