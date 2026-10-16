# Vertrauenswürdiges HTTPS für Home Assistant (Odroid N2)

Ziel: `https://<name>` **ohne Browser-Warnung** und **ohne** dass Clients etwas
installieren müssen — aber auch **ohne** den Dienst ins Internet zu stellen.

Ein Zertifikat braucht keinen öffentlich erreichbaren Server; entscheidend ist,
dass der **Name öffentlich auflösbar** ist und die CA den Besitz per DNS-01
prüfen kann (TXT-Eintrag `_acme-challenge.<name>`). Der A-Record darf dabei auf
eine **private IP** zeigen — im LAN wird HA erreicht, aus dem Internet nicht.

Es gibt zwei eingerichtete Wege:

## Empfohlen: lancert.dev (keine Registrierung, kein Konto)

`setup_lancert_tls.sh` (idempotent) nutzt **[lancert.dev](https://lancert.dev)**
(MIT, github.com/lucor/lancert). Ein POST auf `/register/<ip>` legt automatisch
einen Namen wie `<wort-wort>.lancert.dev` an, der auf die private IP zeigt, und
gibt einmalig DNS-01-Zugangsdaten (acme-dns-API) aus. acme.sh holt damit ein
echtes Let's-Encrypt-Zertifikat.

```bash
# auf dem Odroid
LAN_IP=192.168.178.71 bash setup_lancert_tls.sh
```

- Ergebnis: z. B. `beaver-plain.lancert.dev` -> `192.168.178.71`,
  Zertifikat von Let's Encrypt, **systemweit vertrauenswürdig**.
- Zugangsdaten liegen in `/root/.lancert/acmedns.env` (0600), **nicht im Repo**.
- Erneuerung: `acme-renew.timer` (täglich, `acme.sh --cron`).
- ⚠️ **Sicherheitsmodell:** Die lancert-Zugangsdaten autorisieren nur den
  TXT-/DNS-01-Nachweis für den einen Namen — nicht mehr. Trotzdem privat halten.
  Der A-Record auf eine RFC-1918-Adresse bleibt im LAN.

## Alternativ: eigene lokale CA (`setup_https_nginx.sh`)

Falls der Name nicht öffentlich auflösbar sein soll (z. B. `odroidn2.local`),
stellt keine öffentliche CA aus. Dann erzeugt `setup_https_nginx.sh` eine
**lokale Root-CA** und signiert damit das Server-Zertifikat. Einmalig muss
`ca.crt` auf den Geräten als vertrauenswürdig installiert werden.

Fingerabdruck der lokalen CA (SHA-256):
```
0D:87:78:39:C6:31:3D:11:EF:8D:4A:CB:BC:DD:27:AC:88:0B:13:5A:51:02:9A:A7:A2:06:99:4B:34:6C:5C:EE
```

**Windows:** Doppelklick → *Zertifikat installieren* → *Lokaler Computer* →
*Alle Zertifikate in folgendem Speicher* → *Vertrauenswürdige
Stammzertifizierungsstellen*.
**macOS:** in den Schlüsselbund „System" → *Systemzertifikate* → *Immer vertrauen*.
**Linux:** `sudo cp odroidn2-ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`
**Firefox:** eigener Speicher → *Einstellungen → Datenschutz → Zertifikate → importieren*.

## Beide Namen parallel

nginx bedient SNI: Der **lancert**-Name (Let's Encrypt, `default_server`) und
`odroidn2.local` (lokale CA) funktionieren gleichzeitig. Die CA ist zusätzlich
unter `http://<host>/ca.crt` abrufbar (über HTTP, ohne Henne-Ei-Problem).

## FRITZ!Box

Falls ein Name im LAN nicht auflöst (DNS-Rebind-Schutz blockiert öffentliche
Namen auf private IPs): *Heimnetz → Netzwerk → Netzwerkeinstellungen → Ausnahmen*
den Namen hinzufügen.

## Rückbau

```bash
systemctl disable --now acme-renew.timer
rm -f /etc/nginx/sites-enabled/homeassistant
```

## IP-Adresse behalten (`https://192.168.178.71`)

Oeffentliche CAs (Let's Encrypt) stellen fuer **private** IPs keine Zertifikate
aus (RFC-1918 wird gefiltert, IP-Zerts nur fuer oeffentliche IPs). Ein Schloss
auf der nackten IP gibt es daher nur mit der **lokalen CA** - deren Server-Zert
hat die IP bereits als SAN. `setup_https_ip.sh` legt die nginx-Bloecke so an:

- `https://192.168.178.71` + `https://odroidn2.local` -> lokale CA
  (`default_server`, greift bei IP), SAN = DNS odroidn2.local/odroidn2 + IPs.
- `https://<wort-wort>.lancert.dev` -> Let's Encrypt (kein Client-Setup).

Voraussetzung fuer die IP ohne Warnung: **die lokale CA einmalig auf dem Geraet
installieren** (siehe oben). Ohne das zeigt der Browser weiterhin eine Warnung -
das liegt am Client, nicht am Server.
