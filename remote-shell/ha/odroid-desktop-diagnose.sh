#!/bin/bash
# Odroid-Diagnose (read-only): Ist-Zustand fuer "Desktop im Browser".
#
# Sammelt alles, was zur Entscheidung noetig ist:
#  - laeuft XFCE/TigerVNC (xfce-vnc.service, Port 5901)?
#  - gibt es schon novnc.service / websockify / Port 6080?
#  - welche Web-Dienste laufen auf 80/8080/6080 (Kandidat "File Explorer")?
#
# Aufruf AUF DEM ODROID (als root):  bash odroid-desktop-diagnose.sh
set -u

echo "===== 1. Desktop-Dienste ====="
systemctl is-enabled xfce-vnc.service 2>&1 | sed 's/^/xfce-vnc enabled: /'
systemctl is-active  xfce-vnc.service 2>&1 | sed 's/^/xfce-vnc active : /'
systemctl is-enabled novnc.service 2>&1 | sed 's/^/novnc    enabled: /'
systemctl is-active  novnc.service 2>&1 | sed 's/^/novnc    active : /'

echo
echo "===== 2. Pakete ====="
dpkg -l novnc python3-websockify 2>/dev/null | grep -E '^ii' || echo "(novnc/websockify nicht installiert)"
command -v websockify || echo "(websockify nicht im PATH)"
[ -f /usr/share/novnc/vnc.html ] && echo "webroot: /usr/share/novnc (vnc.html vorhanden)" \
  || echo "webroot: /usr/share/novnc FEHLT"

echo
echo "===== 3. Lauschende Ports (Web/VNC) ====="
ss -ltnp 2>/dev/null | grep -E ':(80|443|5901|6080|6081|8080|8081|8000|3000|9000)\b' || echo "(keine der Ports offen)"

echo
echo "===== 4. Kandidaten fuer 'File Explorer' (Web-Dateiverwaltung) ====="
for name in filebrowser cockpit webmin casaos nginx apache2 lighttpd samba smbd phpmyadmin \
            transmission openmediavault; do
  state=$(systemctl is-active "$name" 2>/dev/null)
  [ -n "$state" ] && [ "$state" != "inactive" ] && [ "$state" != "unknown" ] \
    && echo "  $name: $state"
done
echo "  -- Einheiten mit 'file' im Namen --"
systemctl list-units --type=service --all --no-legend 2>/dev/null | grep -iE 'file|explor' || echo "  (keine)"

echo
echo "===== 5. Wer hoert auf 80/8080? (Prozessnamen) ====="
ss -ltnp 2>/dev/null | grep -E ':(80|8080)\b' | sed -E 's/.*users:\(\("([^"]+)".*/  -> \1/'

echo
echo "===== 6. VNC-Prozesse ====="
ps -eo pid,cmd | grep -E 'Xtigervnc|vncserver|websockify' | grep -v grep || echo "(keine)"

echo
echo "===== 7. HTTP-Probe (localhost) ====="
for p in 80 8080 6080; do
  code=$(curl -s -o /dev/null -m 3 -w '%{http_code}' "http://127.0.0.1:$p/" 2>/dev/null)
  echo "  port $p -> HTTP ${code:-keine Antwort}"
done

echo
echo "===== 8. Zusammenfassung ====="
echo "Wenn 'novnc active: inactive' und Port 6080 zu ist -> novnc.service einrichten."
echo "Der 'File Explorer' ist der Dienst, der auf 80/8080 lauscht (Abschnitt 5)."
