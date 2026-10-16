#!/bin/bash
# Echtes Let's-Encrypt-Zertifikat fuer einen DuckDNS-Namen, validiert per DNS-01.
#
# Ziel: HTTPS ohne Warnhinweis und OHNE dass Clients ein Zertifikat installieren
# muessen - aber auch OHNE dass der Dienst aus dem Internet erreichbar ist.
#
# Trick: Der DuckDNS-A-Record zeigt auf die PRIVATE LAN-IP (z. B. 192.168.178.71).
# Im LAN loest der Name damit auf den Odroid auf; die Aussenwelt erreicht eine
# private Adresse nicht. Let's Encrypt prueft bei DNS-01 ausserdem nur den
# TXT-Eintrag `_acme-challenge.<name>`, nicht die Erreichbarkeit des Servers.
#
# Idempotent: mehrfaches Ausfuehren ist unschaedlich (erneuert nur das Zert).
#
# Aufruf (Token wird NICHT geloggt, nur ueber die Umgebung gelesen):
#   DUCKDNS_DOMAIN=meinname.duckdns.org DUCKDNS_TOKEN=xxxx bash setup_duckdns_tls.sh
#
# Optional: LAN_IP ueberschreiben (Default 192.168.178.71).
set -u
export DEBIAN_FRONTEND=noninteractive

DOMAIN="${DUCKDNS_DOMAIN:-}"
TOKEN="${DUCKDNS_TOKEN:-}"
SSL=/etc/nginx/ssl
ACME_HOME=/root/.acme.sh
ACME=$ACME_HOME/acme.sh
LOCAL_NAME="odroidn2.local"          # behaelt das Zertifikat der lokalen CA
LAN_IP="${LAN_IP:-192.168.178.71}"

if [ -z "$DOMAIN" ] || [ -z "$TOKEN" ]; then
  echo "FEHLER: DUCKDNS_DOMAIN und DUCKDNS_TOKEN muessen gesetzt sein." >&2
  exit 1
fi
case "$DOMAIN" in
  *.duckdns.org) : ;;
  *) echo "FEHLER: DUCKDNS_DOMAIN muss auf .duckdns.org enden." >&2; exit 1 ;;
esac
SUB="${DOMAIN%.duckdns.org}"

# --- 1) A-Record auf die private LAN-IP setzen ------------------------------
resp=$(curl -s "https://www.duckdns.org/update?domains=${SUB}&token=${TOKEN}&ip=${LAN_IP}")
echo "DuckDNS-Update fuer ${DOMAIN} -> ${LAN_IP}: ${resp}"
if [ "$resp" != "OK" ]; then
  echo "FEHLER: DuckDNS lehnte das Update ab (Token/Name pruefen)." >&2
  exit 1
fi

# --- 2) acme.sh installieren (falls noetig) ---------------------------------
if [ ! -x "$ACME" ]; then
  curl -s https://get.acme.sh | sh -s email=admin@${DOMAIN}
fi
if [ ! -x "$ACME" ]; then
  echo "FEHLER: acme.sh konnte nicht installiert werden." >&2
  exit 1
fi

# --- 3) Zertifikat per DNS-01 ausstellen ------------------------------------
export DuckDNS_Token="$TOKEN"
# Erstversuch mit Standard-DNS-Wartezeit; schlaegt er fehl, laenger warten.
"$ACME" --issue --dns dns_duckdns -d "$DOMAIN" --keylength ec-256 --dnssleep 30 \
  || "$ACME" --issue --dns dns_duckdns -d "$DOMAIN" --keylength ec-256 --dnssleep 120
rc=$?
if [ $rc -ne 0 ]; then
  echo "HINWEIS: --issue meldete einen Fehler (bei vorhandenem, noch gueltigem" >&2
  echo "Zertifikat ist das normal). Weiter mit der Installation." >&2
fi

# --- 4) Zertifikat an nginx-Pfade installieren + Reload-Hook ----------------
"$ACME" --install-cert -d "$DOMAIN" --ecc \
  --key-file       "$SSL/duckdns.key" \
  --fullchain-file "$SSL/duckdns.crt" \
  --reloadcmd      "systemctl reload nginx"
if [ ! -f "$SSL/duckdns.crt" ]; then
  echo "FEHLER: Zertifikat wurde nicht installiert." >&2
  exit 1
fi
chmod 600 "$SSL/duckdns.key"

# --- 5) nginx-Vhost: DuckDNS (LE) + odroidn2.local (lokale CA) parallel -----
# SNI waehlt das passende Zertifikat; beide Namen bleiben nutzbar.
mkdir -p "$SSL"
if [ ! -f "$SSL/ca.crt" ]; then
  # Fallback, falls die lokale CA fehlt: nur der LE-Block wird geschrieben.
  LOCAL_BLOCK=""
else
  LOCAL_BLOCK="server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name ${LOCAL_NAME};
    ssl_certificate     ${SSL}/odroidn2.crt;
    ssl_certificate_key ${SSL}/odroidn2.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    location / { proxy_pass http://127.0.0.1:8123;
        proxy_http_version 1.1; proxy_set_header Host \$host;
        proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \"upgrade\";
        proxy_read_timeout 600s; proxy_buffering off; }
}"
fi

cat > /etc/nginx/sites-available/homeassistant <<CONF
server {
    listen 80;
    listen [::]:80;
    server_name ${DOMAIN};
    location = /ca.crt { alias ${SSL}/ca.crt; default_type application/x-x509-ca-cert; }
    location / { return 301 https://\$host\$request_uri; }
}
server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name ${DOMAIN};
    ssl_certificate     ${SSL}/duckdns.crt;
    ssl_certificate_key ${SSL}/duckdns.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    location / { proxy_pass http://127.0.0.1:8123;
        proxy_http_version 1.1; proxy_set_header Host \$host;
        proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \"upgrade\";
        proxy_read_timeout 600s; proxy_buffering off; }
}
${LOCAL_BLOCK}
CONF

ln -sf /etc/nginx/sites-available/homeassistant /etc/nginx/sites-enabled/homeassistant
rm -f /etc/nginx/sites-enabled/default

nginx -t || { echo "FEHLER: nginx-Konfiguration ungueltig." >&2; exit 1; }
systemctl enable nginx >/dev/null 2>&1
systemctl reload nginx 2>/dev/null || systemctl restart nginx

# --- 6) Erneuerung sicherstellen --------------------------------------------
# acme.sh legt beim Installieren einen Cronjob an; hier zusaetzlich ein
# systemd-Timer als Belt-and-Braces fuer Systeme mit deaktiviertem Cron.
cat > /etc/systemd/system/acme-renew.service <<'UNIT'
[Unit]
Description=acme.sh renewal
[Service]
Type=oneshot
ExecStart=/root/.acme.sh/acme.sh --cron --home /root/.acme.sh
UNIT
cat > /etc/systemd/system/acme-renew.timer <<'UNIT'
[Unit]
Description=taegliche acme.sh Erneuerung
[Timer]
OnCalendar=daily
Persistent=true
[Install]
WantedBy=timers.target
UNIT
systemctl daemon-reload
systemctl enable --now acme-renew.timer >/dev/null 2>&1

# --- 7) Kontrolle -----------------------------------------------------------
sleep 1
echo "HTTP  (${DOMAIN}) -> $(curl -s -o /dev/null -w '%{http_code}' -L --resolve ${DOMAIN}:80:127.0.0.1 http://${DOMAIN}/ 2>/dev/null)"
echo "HTTPS (${DOMAIN}) -> $(curl -sk -o /dev/null -w '%{http_code}' --resolve ${DOMAIN}:443:127.0.0.1 https://${DOMAIN}/ 2>/dev/null)"
echo "Aussteller:"; openssl x509 -in "$SSL/duckdns.crt" -noout -issuer -subject -dates
echo "SANs:"; openssl x509 -in "$SSL/duckdns.crt" -noout -ext subjectAltName
echo
echo "FERTIG. Im LAN erreichbar unter:  https://${DOMAIN}"
echo "Hinweis: In der FRITZ!Box ggf. ${DOMAIN} vom DNS-Rebind-Schutz ausnehmen"
echo "(Heimnetz > Netzwerk > Netzwerkeinstellungen > Ausnahmen)."
