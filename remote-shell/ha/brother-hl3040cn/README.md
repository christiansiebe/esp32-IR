# Brother HL-3040CN Drucker ueber den Odroid (qemu-i386-Container)

Der HL-3040CN haengt im LAN (`192.168.178.50`, Port 9100). Er soll ueber den
Odroid als CUPS-Druckserver fuer alle Rechner im Heimnetz dienen.

## Warum das nicht direkt geht

Der HL-3040CN ist ein **host-based (GDI)**-Drucker: Brother listet
`Emulation: N/A (Host-Based only)` — er versteht **weder PCL6 noch PostScript**.
Deshalb drucken generische Treiber (und `driverless` IPP/AirPrint) nur
**leere Seiten**: der Datenstrom erreicht den Drucker, aber er kann ihn nicht
interpretieren. `brlaser` kennt den 3040CN nicht (nur S/W-Modelle).

Der Brother-Linux-Treiber ist ein **i386/x86_64-Binary ohne Quellcode**
(Filter `brhl3040cnfilter`, LPR + CUPSwrapper deb, 2011/2013). Es gibt **kein
armhf/arm64** und ohne Quelle kein Rebuild — auf ARM also nicht "portierbar".

**Loesung:** den x86-Filter unter **qemu-User-Emulation** laufen lassen, in
einem **i386-CUPS-Container**. Der arm64-Odroid fuehrt i386-Binaries ueber
`binfmt_misc` + `qemu-i386-static` transparent aus; der Drucker bekommt genau
den Datenstrom, den er versteht.

## Dateien

| Datei | Zweck |
|---|---|
| `Dockerfile` | i386-CUPS-Image; laedt die Brother-debs zur Buildzeit (kein Blob im Repo) |
| `cupsd.conf` | CUPS im LAN erreichbar, keine Auth (reiner Druckserver) |
| `entrypoint.sh` | legt die Queue an (`socket://<IP>:9100`), startet cupsd im Vordergrund |
| `deploy_brother_cups.sh` | binfmt sicherstellen, Image bauen, Container starten |

## Ausrollen (auf dem Odroid)

```
cd /pfad/zu/remote-shell/ha/brother-hl3040cn
./deploy_brother_cups.sh                 # Standard-IP 192.168.178.50
./deploy_brother_cups.sh 192.168.178.50  # explizit
```

Das Skript:
1. installiert bei Bedarf `qemu-user-static` + `binfmt-support`,
2. testet die i386-Emulation (`docker run --platform linux/386 … uname -m` → `i686`),
3. baut das Image (laedt die Brother-debs von `download.brother.com`),
4. startet den Container `brother-cups` mit `--network=host --restart unless-stopped`.

Hinweis `--network=host`: Die Docker-Bridge des Odroid kann das LAN nicht
erreichen (die Host-FORWARD-Policy verwirft es, und die Box laeuft zusaetzlich
als AP). Mit Host-Netzwerk nutzt CUPS die Host-Route zum Drucker und lauscht
direkt auf Port 631.

## Benutzen

- CUPS-Weboberflaeche: `http://<odroid-ip>:631/printers/HL3040CN`
- Queue vom LAN: `lp -h <odroid-ip>:631 -d HL3040CN datei.pdf`
- Auf anderen Rechnern (Windows/macOS/Linux) die Queue per IPP/socket
  hinzufuegen: `ipp://<odroid-ip>:631/printers/HL3040CN`.

Der Drucker selbst ist unter `http://192.168.178.50/` erreichbar. Sein Web-UI
hat **keinen Login**, solange kein Passwort gesetzt wurde (Brothers Default
waere `access`). Die Ports `PCL_P1` / `POSTSCRIPT_P1` dort **nicht** benutzen —
sie sind bei diesem Modell Attrappen (s. o.).

## Wie die Filterkette laeuft

```
PDF --(poppler pdftops)--> PostScript
PostScript --brlpdwrapperhl3040cn--> filterhl3040cn
        -> psconvertij2 (ghostscript -> PPM)
        -> brhl3040cnfilter (i386, unter qemu) -> Brother-Datenstrom (PJL)
Brother-Datenstrom --socket://192.168.178.50:9100--> Drucker
```

Verifiziert (im Sandbox-Build, x86_64): eine PDF-Seite ergibt ~124 kB echten
Brother-Datenstrom (`@PJL JOB NAME="Print Job"` … `@PJL EOJ`, UEL-Ende) —
kein leeres Blatt.

## Randbedingungen

- **CUPS-Port 631 ist ungeschuetzt** (wie der eingebaute Druckerserver). Nur
  im Heimnetz betreiben; die Firewall des Odroid kann 631 auf `192.168.178.0/24`
  begrenzen.
- Der Container braucht beim ersten Build Internet (Brother-debs).
- i386 unter qemu ist langsamer als nativ, fuer gelegentliches Drucken aber
  unauffaellig (Rendern von ~1 Seite < 1 s).
