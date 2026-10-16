#!/bin/bash
# Additive TLS termination for Home Assistant on Armbian (Odroid N2).
#
# HA keeps running plain on 127.0.0.1:8123; this only ADDS an nginx proxy on
# 443 (TLS) plus an 80 -> 443 redirect. Existing http://<ip>:8123 keeps working.
#
# TLS uses a small local CA instead of a self-signed leaf, so the "untrusted"
# warning goes away as soon as the CA (ca.crt) is installed on a device. A
# public CA (Let's Encrypt) cannot issue for the private name odroidn2.local
# without DNS-01, so a local CA is the correct choice for a LAN device.
export DEBIAN_FRONTEND=noninteractive

SSL=/etc/nginx/ssl
CRT=$SSL/odroidn2.crt
KEY=$SSL/odroidn2.key
CA_CRT=$SSL/ca.crt
CA_KEY=$SSL/ca.key
SAN="DNS:odroidn2.local,DNS:odroidn2,IP:192.168.178.71,IP:127.0.0.1"

if ! command -v nginx >/dev/null 2>&1; then
  apt-get update -qq && apt-get install -y -qq nginx
fi
mkdir -p "$SSL"

# 1) Local CA (10 years). Only created once; re-running keeps the same CA so
#    devices that already trust it keep working.
if [ ! -f "$CA_CRT" ]; then
  openssl req -x509 -newkey rsa:3072 -nodes -days 3650 \
    -keyout "$CA_KEY" -out "$CA_CRT" \
    -subj "/CN=Odroid N2 Local CA/O=Odroid N2" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
  chmod 600 "$CA_KEY"
fi

# 2) Server key + CSR, then sign with the CA (2 years) with the LAN SANs.
openssl req -newkey rsa:2048 -nodes \
  -keyout "$KEY" -out "$SSL/odroidn2.csr" \
  -subj "/CN=odroidn2.local/O=Odroid N2"
openssl x509 -req -in "$SSL/odroidn2.csr" -CA "$CA_CRT" -CAkey "$CA_KEY" \
  -CAcreateserial -out "$CRT" -days 730 \
  -extfile <(printf "subjectAltName=%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n" "$SAN")
chmod 600 "$KEY"

cat > /etc/nginx/sites-available/homeassistant <<'CONF'
server {
    listen 80;
    listen [::]:80;
    server_name _;
    location = /ca.crt {
        alias /etc/nginx/ssl/ca.crt;
        default_type application/x-x509-ca-cert;
    }
    location / {
        return 301 https://$host$request_uri;
    }
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
echo "Chain verify (leaf against our CA):"
openssl verify -CAfile "$CA_CRT" "$CRT" 2>&1 || true
echo "Server cert SANs:"; openssl x509 -in "$CRT" -noout -ext subjectAltName
echo "CA fingerprint (install ca.crt on devices to trust it):"
openssl x509 -in "$CA_CRT" -noout -fingerprint -sha256
echo
echo "ca.crt liegt unter: $CA_CRT  -> auf Clients installieren"
