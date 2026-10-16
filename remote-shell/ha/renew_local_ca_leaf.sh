#!/bin/bash
# Erneuert das lokale-CA-Serverzertifikat (odroidn2.crt) automatisch, BEVOR es
# ablaeuft. Die CA selbst (ca.crt/ca.key) bleibt unveraendert - sonst muessten
# Clients die CA neu installieren. Nur das Leaf-Zert (~2 Jahre) wird neu
# signiert; die SANs (IP + Namen) kommen aus diesem Skript.
#
# Aufruf per systemd-Timer (siehe setup_https_ip.sh / acme-renew).
set -eu

SSL=/etc/nginx/ssl
CRT=$SSL/odroidn2.crt
KEY=$SSL/odroidn2.key
CA_CRT=$SSL/ca.crt
CA_KEY=$SSL/ca.key
IP="${LAN_IP:-192.168.178.71}"
SAN="DNS:odroidn2.local,DNS:odroidn2,IP:${IP},IP:127.0.0.1"
RENEW_BEFORE_DAYS="${RENEW_BEFORE_DAYS:-30}"

[ -f "$CA_CRT" ] && [ -f "$CA_KEY" ] || { echo "Lokale CA fehlt."; exit 0; }

need=1
if [ -f "$CRT" ]; then
  end=$(openssl x509 -in "$CRT" -noout -enddate | cut -d= -f2)
  end_s=$(date -d "$end" +%s 2>/dev/null || echo 0)
  now=$(date +%s)
  [ $(( (end_s - now) / 86400 )) -lt "$RENEW_BEFORE_DAYS" ] && need=1 || need=0
fi

if [ "$need" = 0 ]; then
  echo "odroidn2.crt noch gueltig (> ${RENEW_BEFORE_DAYS} Tage) - nichts zu tun."
  exit 0
fi

openssl req -newkey rsa:2048 -nodes \
  -keyout "$KEY" -out "$SSL/odroidn2.csr" \
  -subj "/CN=odroidn2.local/O=Odroid N2"
openssl x509 -req -in "$SSL/odroidn2.csr" -CA "$CA_CRT" -CAkey "$CA_KEY" \
  -CAserial "$SSL/ca.srl" -CAcreateserial -out "$CRT" -days 730 \
  -extfile <(printf "subjectAltName=%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n" "$SAN")
chmod 600 "$KEY"
systemctl reload nginx 2>/dev/null || true
echo "odroidn2.crt neu signiert und nginx neu geladen."
