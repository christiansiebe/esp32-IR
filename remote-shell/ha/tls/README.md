# Vertrauenswürdiges HTTPS für Home Assistant (Odroid N2)

Ein **selbstsigniertes** Zertifikat stuft jeder Browser als „nicht
zertifiziert" ein — das ist normal und kein Fehler. Für ein LAN-Gerät mit dem
privaten Namen `odroidn2.local` kann auch Let's Encrypt kein Zertifikat
ausstellen (dafür bräuchte man eine echte Domain + DNS-01). Der saubere Weg ist
eine **eigene kleine CA**: ist sie einmal auf den Geräten installiert, gilt die
Verbindung als vertrauenswürdig (kein Warnhinweis mehr).

## Was auf dem Odroid läuft

- nginx terminiert TLS auf **443** und leitet auf HA (`127.0.0.1:8123`) weiter;
  Port **80** leitet auf HTTPS um. HA selbst bleibt unverändert.
- `setup_https_nginx.sh` legt an (idempotent):
  - `/etc/nginx/ssl/ca.crt` + `ca.key` — die lokale Root-CA (10 Jahre)
  - `/etc/nginx/ssl/odroidn2.crt` + `.key` — Server-Zertifikat (2 Jahre),
    SANs `odroidn2.local`, `odroidn2`, `192.168.178.71`, `127.0.0.1`
- **Wichtig:** Die CA wird nur einmal erzeugt. Ein erneuter Lauf behält dieselbe
  CA, damit bereits eingerichtete Geräte weiter vertrauen.

## CA auf den Geräten installieren

Die Datei `odroidn2-ca.crt` (in diesem Ordner) ist das **öffentliche**
CA-Zertifikat — kein Geheimnis, darf verteilt werden.

Fingerabdruck (SHA-256, zum Abgleich):
```
0D:87:78:39:C6:31:3D:11:EF:8D:4A:CB:BC:DD:27:AC:88:0B:13:5A:51:02:9A:A7:A2:06:99:4B:34:6C:5C:EE
```

**Windows:** Doppelklick → *Zertifikat installieren* → *Lokaler Computer* →
*Alle Zertifikate in folgendem Speicher* → *Vertrauenswürdige
Stammzertifizierungsstellen*.

**macOS:** Datei öffnen (Schlüsselbund) → im Schlüsselbund „System" unter
*Systemzertifikate* ablegen → doppelklicken → *Immer vertrauen*.

**Linux (Debian/Ubuntu):**
```
sudo cp odroidn2-ca.crt /usr/local/share/ca-certificates/odroidn2-ca.crt
sudo update-ca-certificates
```
Firefox führt einen eigenen Speicher → dort zusätzlich unter
*Einstellungen → Datenschutz → Zertifikate → Zertifizierungsstellen importieren*.

**Android:** *Einstellungen → Sicherheit → Verschlüsselung → Zertifikat
installieren → CA-Zertifikat*. (Neuere Android-Versionen stufen Nutzer-CAs für
Apps als nicht vertrauenswürdig ein; im Browser funktioniert es.)

**iOS:** Zertifikat per AirDrop/Mail öffnen → *Profil installiert* →
*Einstellungen → Allgemein → Info → Zertifikatsvertrauenseinstellungen* →
für diese CA *Vertrauen* aktivieren.

Die CA herunterladen (auf dem Odroid erreichbar solange man dort Zugriff hat):
```
scp root@odroidn2.local:/etc/nginx/ssl/ca.crt ./odroidn2-ca.crt
```

## Herunterfahren / Rückbau

```
systemctl disable --now nginx
```
