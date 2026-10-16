#!/bin/bash
# nginx-TLS-Vhosts fuer den Odroid N2, damit SOWOHL die IP als auch Namen
# funktionieren:
#   * https://192.168.178.71 und https://odroidn2.local  -> lokale CA
#     (SAN enthaelt die IP + die Namen); CA muss einmalig auf dem Client
#     installiert sein.
#   * https://<wort-wort>.lancert.dev                     -> Let's Encrypt
#     (oeffentlich vertrauenswuerdig, kein Client-Setup).
# SNI/TLS waehlt anhand des Namens; die IP nutzt den default_server.
#
# Idempotent. Setzt voraus, dass die lokale CA (setup_https_nginx.sh) und/oder
# lancert (setup_lancert_tls.sh) bereits vorhanden sind.
set -u
SSL=/etc/nginx/ssl
IP="${LAN_IP:-192.168.178.71}"
LOCAL_NAME="odroidn2.local"
LE_HOST="${LANCERT_HOST:-}"
[ -z "$LE_HOST" ] && [ -f /root/.lancert/acmedns.env ] && . /root/.lancert/acmedns.env && LE_HOST="${LANCERT_HOST:-}"

if [ ! -f "$SSL/ca.crt" ] && [ -z "$LE_HOST" ]; then
  echo "FEHLER: weder lokale CA noch lancert-Zertifikat vorhanden." >&2
  exit 1
fi

PROXY='proxy_pass http://127.0.0.1:8123;
        proxy_http_version 1.1; proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade";
        proxy_read_timeout 600s; proxy_buffering off;'

# Block fuer IP/odroidn2.local mit lokaler CA (default_server -> greift bei IP).
LOCAL_BLOCK=""
if [ -f "$SSL/ca.crt" ]; then
  LOCAL_BLOCK="server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name ${IP} ${LOCAL_NAME};
    ssl_certificate     ${SSL}/odroidn2.crt;
    ssl_certificate_key ${SSL}/odroidn2.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    location / { ${PROXY} }
}"
fi

# Block fuer den lancert-Namen mit Let's-Encrypt-Zert (falls vorhanden).
LE_BLOCK=""
if [ -n "$LE_HOST" ] && [ -f "$SSL/lancert.crt" ]; then
  LE_BLOCK="server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name ${LE_HOST};
    ssl_certificate     ${SSL}/lancert.crt;
    ssl_certificate_key ${SSL}/lancert.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    location / { ${PROXY} }
}"
fi

# Port 80: ACME/Redirect + CA-Download.
cat > /etc/nginx/sites-available/homeassistant <<CONF
server {
    listen 80;
    listen [::]:80;
    server_name ${IP} ${LOCAL_NAME} ${LE_HOST};
    location = /ca.crt { alias ${SSL}/ca.crt; default_type application/x-x509-ca-cert; }
    location / { return 301 https://\$host\$request_uri; }
}
${LOCAL_BLOCK}
${LE_BLOCK}
CONF

ln -sf /etc/nginx/sites-available/homeassistant /etc/nginx/sites-enabled/homeassistant
rm -f /etc/nginx/sites-enabled/default
nginx -t || { echo "FEHLER: nginx-Config ungueltig." >&2; exit 1; }
systemctl reload nginx 2>/dev/null || systemctl restart nginx

sleep 1
echo "https IP            -> $(curl -sk -o /dev/null -w '%{http_code}' https://${IP}/ 2>/dev/null)"
echo "https odroidn2.local-> $(curl -sk -o /dev/null -w '%{http_code}' --resolve ${LOCAL_NAME}:443:127.0.0.1 https://${LOCAL_NAME}/ 2>/dev/null)"
[ -n "$LE_HOST" ] && echo "https ${LE_HOST} -> $(curl -sk -o /dev/null -w '%{http_code}' --resolve ${LE_HOST}:443:127.0.0.1 https://${LE_HOST}/ 2>/dev/null)"
echo "Zertifikat fuer IP liefert:"; echo | openssl s_client -connect 127.0.0.1:443 -servername ${IP} 2>/dev/null | openssl x509 -noout -subject -issuer 2>/dev/null
