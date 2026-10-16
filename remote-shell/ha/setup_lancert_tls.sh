#!/bin/bash
# Echtes Let's-Encrypt-Zertifikat fuer die private LAN-IP - ohne Registrierung,
# ohne Konto, ohne Zertifikat-Download auf den Clients.
#
# Wie das ohne eigene Domain geht: lancert.dev (MIT, github.com/lucor/lancert)
# ist ein oeffentlicher DNS-Dienst, der einen Namen wie <wort-wort>.lancert.dev
# auf eine RFC-1918-Adresse (z. B. 192.168.178.71) zeigen laesst und den
# DNS-01-Nachweis (TXT `_acme-challenge.<name>`) per acme-dns-API bereitstellt.
# Ein POST auf /register/<ip> legt das an und liefert die Zugangsdaten EINMALIG.
#
# Ergebnis: echtes LE-Zertifikat -> Browser zeigt ein Schloss, nichts zu
# installieren. Aus dem Internet ist die private IP nicht erreichbar.
#
# Zugangsdaten (lancert-Passwort) liegen nur lokal unter
# /root/.lancert/acmedns.env (0600) - NICHT im Repo.
#
# Idempotent: existiert die env-Datei, wird sie wiederverwendet (keine neue
# Registrierung).
set -u
export DEBIAN_FRONTEND=noninteractive

LAN_IP="${LAN_IP:-192.168.178.71}"
SSL=/etc/nginx/ssl
ACME_HOME=/root/.acme.sh
ACME=$ACME_HOME/acme.sh
ENV_DIR=/root/.lancert
ENV_FILE=$ENV_DIR/acmedns.env
LOCAL_NAME="odroidn2.local"          # lokale CA bleibt parallel nutzbar

# --- 1) lancert-Registrierung (einmalig) ------------------------------------
mkdir -p "$ENV_DIR" && chmod 700 "$ENV_DIR"
if [ -f "$ENV_FILE" ]; then
  . "$ENV_FILE"
  echo "lancert: vorhandene Registrierung wiederverwendet ($LANCERT_HOST)."
else
  resp=$(curl -s --fail-with-body --max-time 40 -X POST "https://lancert.dev/register/${LAN_IP}") || {
    echo "FEHLER: lancert.dev-Registrierung fehlgeschlagen." >&2; exit 1; }
  python3 - "$resp" "$ENV_FILE" <<'PY' || exit 1
import json, sys
d = json.loads(sys.argv[1])
mapping = {
    "hostname": "LANCERT_HOST",
    "username": "ACMEDNS_USERNAME",
    "password": "ACMEDNS_PASSWORD",
    "subdomain": "ACMEDNS_SUBDOMAIN",
}
assert all(k in d for k in mapping), "unerwartete lancert-Antwort"
with open(sys.argv[2], "w") as f:
    f.write("ACMEDNS_BASE_URL=https://lancert.dev\n")
    for k, var in mapping.items():
        f.write("%s=%s\n" % (var, d[k]))
PY
  chmod 600 "$ENV_FILE"
  . "$ENV_FILE"
  echo "lancert: neu registriert -> $LANCERT_HOST  (-> $LAN_IP)"
fi

# --- 2) DNS-A-Record pruefen ------------------------------------------------
got=$(curl -s --max-time 15 -H 'accept: application/dns-json' \
  "https://cloudflare-dns.com/dns-query?name=${LANCERT_HOST}&type=A" \
  | python3 -c "import sys,json;d=json.load(sys.stdin);print((d.get('Answer') or [{}])[0].get('data',''))")
echo "DNS ${LANCERT_HOST} -> ${got:-<leer>} (erwartet ${LAN_IP})"

# --- 3) acme.sh installieren ------------------------------------------------
# Immer mit HOME=/root, sonst installiert der Installer nach /.acme.sh.
if [ ! -x "$ACME" ] && [ -x /.acme.sh/acme.sh ]; then
  mkdir -p /root/.acme.sh && cp -a /.acme.sh/. /root/.acme.sh/
fi
if [ ! -x "$ACME" ]; then
  HOME=/root curl -s https://get.acme.sh | HOME=/root sh -s email=admin@${LANCERT_HOST}
fi
[ -x "$ACME" ] || { echo "FEHLER: acme.sh fehlt ($ACME)." >&2; exit 1; }

# --- 4) Zertifikat per DNS-01 (acme-dns-API von lancert) --------------------
# Die env-Datei MUSS exportiert werden: acme.sh ist ein eigener Prozess und
# sieht sonst ACMEDNS_* nicht (dann fragt sein Provider interaktiv nach).
set -a
. "$ENV_FILE"
set +a
export ACMEDNS_BASE_URL="${ACMEDNS_BASE_URL:-https://lancert.dev}"
export HOME=/root
"$ACME" --issue --dns dns_acmedns --server letsencrypt --keylength ec-256 \
  -d "$LANCERT_HOST" --dnssleep 15
rc=$?
[ $rc -ne 0 ] && echo "HINWEIS: --issue meldete Fehler (bei noch gueltigem Zert normal)."

"$ACME" --install-cert -d "$LANCERT_HOST" --ecc \
  --key-file       "$SSL/lancert.key" \
  --fullchain-file "$SSL/lancert.crt" \
  --reloadcmd      "systemctl reload nginx"
[ -f "$SSL/lancert.crt" ] || { echo "FEHLER: Zertifikat nicht installiert." >&2; exit 1; }
chmod 600 "$SSL/lancert.key"

# --- 5) nginx-Vhost ---------------------------------------------------------
if [ -f "$SSL/ca.crt" ]; then
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
else
  LOCAL_BLOCK=""
fi

cat > /etc/nginx/sites-available/homeassistant <<CONF
server {
    listen 80;
    listen [::]:80;
    server_name ${LANCERT_HOST} ${LOCAL_NAME};
    location = /ca.crt { alias ${SSL}/ca.crt; default_type application/x-x509-ca-cert; }
    location / { return 301 https://\$host\$request_uri; }
}
server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name ${LANCERT_HOST};
    ssl_certificate     ${SSL}/lancert.crt;
    ssl_certificate_key ${SSL}/lancert.key;
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
nginx -t || { echo "FEHLER: nginx-Config ungueltig." >&2; exit 1; }
systemctl enable nginx >/dev/null 2>&1
systemctl reload nginx 2>/dev/null || systemctl restart nginx

# --- 6) Erneuerung ----------------------------------------------------------
cat > /etc/systemd/system/acme-renew.service <<UNIT
[Unit]
Description=acme.sh renewal (lancert/DNS-01)
[Service]
Type=oneshot
EnvironmentFile=${ENV_FILE}
ExecStart=${ACME} --cron --home ${ACME_HOME}
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
echo "HTTP  (${LANCERT_HOST}) -> $(curl -s -o /dev/null -w '%{http_code}' --resolve ${LANCERT_HOST}:80:127.0.0.1 http://${LANCERT_HOST}/ 2>/dev/null)"
echo "HTTPS (${LANCERT_HOST}) -> $(curl -sk -o /dev/null -w '%{http_code}' --resolve ${LANCERT_HOST}:443:127.0.0.1 https://${LANCERT_HOST}/ 2>/dev/null)"
echo "Zertifikat:"; openssl x509 -in "$SSL/lancert.crt" -noout -issuer -subject -dates
echo
echo "FERTIG. Im LAN erreichbar unter:  https://${LANCERT_HOST}"
echo "FRITZ!Box: '${LANCERT_HOST}' ggf. vom DNS-Rebind-Schutz ausnehmen."
