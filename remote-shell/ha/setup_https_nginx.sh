#!/bin/bash
# Additive TLS termination for Home Assistant on Armbian (Odroid N2).
# HA keeps running plain on 127.0.0.1:8123; this only ADDS an nginx proxy on
# 443 (TLS) plus an 80 -> 443 redirect. Existing http://<ip>:8123 keeps working.
export DEBIAN_FRONTEND=noninteractive

if ! command -v nginx >/dev/null 2>&1; then
  apt-get update -qq && apt-get install -y -qq nginx
fi

mkdir -p /etc/nginx/ssl
if [ ! -f /etc/nginx/ssl/odroidn2.crt ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout /etc/nginx/ssl/odroidn2.key \
    -out /etc/nginx/ssl/odroidn2.crt \
    -subj "/CN=odroidn2.local" \
    -addext "subjectAltName=DNS:odroidn2.local,DNS:odroidn2,IP:192.168.178.71,IP:127.0.0.1"
  chmod 600 /etc/nginx/ssl/odroidn2.key
fi

cat > /etc/nginx/sites-available/homeassistant <<'CONF'
server {
    listen 80;
    listen [::]:80;
    server_name _;
    return 301 https://$host$request_uri;
}
server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name _;
    ssl_certificate     /etc/nginx/ssl/odroidn2.crt;
    ssl_certificate_key /etc/nginx/ssl/odroidn2.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    location / {
        proxy_pass http://127.0.0.1:8123;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 600s;
        proxy_buffering off;
    }
}
CONF

ln -sf /etc/nginx/sites-available/homeassistant /etc/nginx/sites-enabled/homeassistant
rm -f /etc/nginx/sites-enabled/default

nginx -t || exit 1
systemctl enable nginx >/dev/null 2>&1
systemctl restart nginx

sleep 1
echo "HTTP  -> $(curl -s -o /dev/null -w '%{http_code}' -L http://127.0.0.1/ 2>/dev/null)"
echo "HTTPS -> $(curl -sk -o /dev/null -w '%{http_code}' https://127.0.0.1/ 2>/dev/null)"
echo "cert SANs:"; openssl x509 -in /etc/nginx/ssl/odroidn2.crt -noout -ext subjectAltName 2>/dev/null
