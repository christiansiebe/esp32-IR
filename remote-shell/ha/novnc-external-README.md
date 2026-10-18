# XFCE-Desktop von außerhalb des LAN (Odroid N2+ über den Sandbox-Proxy)

Der Odroid N2+ hat einen echten XFCE-Desktop, der im LAN über noVNC im Browser
erreichbar ist (`http://odroidn2.local:6080/vnc.html`, siehe `novnc-README.md`).
Dieses Dokument beschreibt, wie man denselben Desktop **von außerhalb des LAN**
erreichbar macht — über den bereits vorhandenen Reverse-Tunnel und den
öffentlichen Sandbox-Proxy auf Port `12000` — und bewertet die Sicherheit.

> **Kurzfassung:** Die Verkettung ist technisch einfach, macht aber aus einem
> LAN-geschützten Desktop einen **öffentlich erreichbaren root-Desktop**. In der
> aktuellen Konfiguration (kein VNC-Passwort, `-SecurityTypes None`) darf sie
> **so nicht** in Betrieb gehen. Erforderlich sind mindestens ein VNC-Passwort
> (`-SecurityTypes VncAuth`) **und** eine zusätzliche Authentifizierung vor dem
> Proxy. Details unten.

## 1. Die Kette

```
  Browser (beliebiges Netz)
      │  HTTPS  (Let's Encrypt am Proxy)
      ▼
  https://work-1-<RUNTIME_ID>.prod-runtime.all-hands.dev/   ← öffentlich
      │  (Runtime-Proxy leitet auf den Container-Port 12000)
      ▼
  Sandbox: websockify  0.0.0.0:12000  ──►  127.0.0.1:5901   (lokaler Tunnel-Endpunkt)
      │  Reverse-Tunnel (SSH, verschlüsselt)
      ▼
  Odroid: 127.0.0.1:5901  =  TigerVNC :1  ──►  XFCE (als root)
```

Bestandteile:

1. **Sandbox-Proxy.** Die Runtime veröffentlicht Container-Port `12000` unter
   `https://work-1-<RUNTIME_ID>.prod-runtime.all-hands.dev/` (TLS mit
   Let's-Encrypt-Zertifikat). Dieselbe Mechanik trägt schon den OTA-Upload
   (`AGENTS.md`, Abschnitt „OTA from the sandbox") und den Kommando-Kanal auf
   Port `12001` (`work-2-…`). Der Proxy **authentifiziert nicht** — er reicht
   alles durch, was auf dem Port lauscht.
2. **websockify in der Sandbox.** `start_novnc_bridge.sh` startet in *dieser*
   Sandbox ein `websockify --web=<noVNC> 0.0.0.0:12000 127.0.0.1:5901`. Es
   übersetzt den noVNC-WebSocket zurück nach VNC und liefert die noVNC-Seite aus.
3. **Reverse-Tunnel.** Er stellt den lokalen Endpunkt `127.0.0.1:5901` in der
   Sandbox bereit und leitet ihn auf den Odroid weiter (TigerVNC `:1`). Der
   Tunnel wird **nicht** von diesem Skript aufgebaut; er ist Voraussetzung. Die
   bestehende Hilfe dafür ist `tools/remote-shell/reverse_ssh.sh` (SSH-Tunnel).
   Fehlt der Endpunkt, bricht `start_novnc_bridge.sh` mit klarer Meldung ab.
4. **TigerVNC / XFCE auf dem Odroid.** `xfce-vnc.service` betreibt TigerVNC auf
   `:1`, `-localhost yes` (nur `127.0.0.1:5901`) und aktuell
   `-SecurityTypes None` (kein Passwort).

## 2. Warum das ein öffentlich erreichbarer root-Desktop ist

Drei Punkte zusammen ergeben die volle Exposition:

* **Der Proxy ist öffentlich und ohne Auth.** Wer die URL kennt, erreicht
  `websockify` auf Port `12000`. Es gibt keine Zugangsbeschränkung am Proxy.
  Die `RUNTIME_ID` ist kein Geheimnis — sie steht in Logs, Docs und Chats
  (Obscurity ist kein Schutz).
* **Der LAN-Guard schützt diesen Weg nicht.** `vnc-lan-guard` filtert die
  *eingehenden* Verbindungen des Odroid auf Port `6080` nach Quell-IP. Der
  Tunnel endet aber auf `127.0.0.1:5901` (Loopback). Loopback-Verkehr passiert
  die `INPUT`-Kette nicht und wird vom Guard **gar nicht gesehen**. Der
  IP-Filter ist also wirkungslos, sobald ein Tunnel direkt auf den VNC-Port
  zeigt.
* **Es gibt kein VNC-Passwort.** `-SecurityTypes None` heißt: Nach dem
  TCP-Handshake wird der Desktop ohne jede Anmeldung freigegeben. Der
  VNC-Handshake selbst bietet nicht einmal eine Challenge an, die man knacken
  müsste.

Ergebnis: Ein einziger HTTP(S)-Aufruf der öffentlichen URL genügt, um eine
**Shell als root** auf dem Board zu bekommen — Dateien lesen/schreiben,
`systemctl`, alles. Das ist die entscheidende Aussage dieses Dokuments.

## 3. Sicherheitsbewertung

### 3.1 Braucht der externe Zugriff ein VNC-Passwort?

**Ja — nicht verhandelbar.** Sobald der Desktop über den öffentlichen Proxy
erreichbar ist, ist `-SecurityTypes None` gleichbedeutend mit „root ohne
Passwort im Internet". In `xfce-vnc.service` muss daher wieder
`-SecurityTypes VncAuth` stehen, mit einem gesetzten VNC-Passwort
(`vncpasswd -f > /root/.vnc/passwd`, `chmod 600`). Das Passwort darf — wie
schon beim LAN-Setup — **nie** ins Repo; es liegt nur auf dem Board
(`/root/.vnc/`, 0600).

Hinweis: `VncAuth` ist bewusst schwach (8 Zeichen, DES-basiertes
Challenge-Response, kein Lockout). Es ist die *unterste* Schicht und schützt
das VNC-Protokoll unabhängig vom Transport — aber es ist **allein nicht
ausreichend** für einen internet-exponierten Desktop.

### 3.2 Braucht es zusätzlich Auth am Proxy?

**Ja, empfohlen — als zweite, unabhängige Schicht (Defense in Depth).**
Gründe:

* **Angriffsfläche verkleinern.** Ohne Transport-Auth kann jeder Scanner die
  noVNC-Instanz und die angebotenen VNC-Security-Typen sehen und direkt gegen
  `VncAuth` brute-forcen. Eine Auth vor dem WebSocket-Upgrade blendet das aus.
* **Unabhängiger Faktor.** Fällt eine Schicht (z. B. schwaches VNC-Passwort,
  oder ein Fehler im VNC-Pfad), bleibt die andere. Zwei verschiedene
  Passwörter an zwei Stellen sind deutlich besser als eines.
* **Kein Lockout/Logging am Proxy.** Der Runtime-Proxy hat weder Rate-Limit
  noch Fail2ban. Ein vorgeschalteter, authentifizierender Dienst ist die
  einfachste Stelle, um überhaupt eine Hürde einzubauen.

Umsetzungsoptionen (Board-/Host-seitig, nicht Teil dieses Skripts):

* `websockify` mit `--auth-plugin=BasicHTTPAuth --auth-source=<datei>` und
  `--web-auth` (HTTP-Basic auf Web-Root *und* WebSocket). Verfügbarkeit der
  Optionen hängt von der websockify-Version ab — am Board verifizieren.
* Oder ein kleiner authentifizierender Reverse-Proxy (nginx/Caddy) vor
  `websockify`, der `Authorization` prüft und erst dann auf `127.0.0.1:5901`
  weiterleitet. Robuster und besser protokollierbar.

### 3.3 Empfehlung

1. **Wenn externer Zugriff nicht zwingend nötig ist: nicht exponieren.**
   Für einen root-Desktop ist der beste Transport **SSH**, nicht noVNC im
   öffentlichen Netz. Der SSH-Weg existiert bereits (`reverse_ssh.sh`). Für die
   GUI: SSH-Local-Forward (`ssh -L 5901:127.0.0.1:5901 …`) plus lokaler
   VNC-Client, oder ein VPN (WireGuard/Tailscale). Das vermeidet die
   öffentliche Angriffsfläche komplett.
2. **Wenn der Proxy-Weg gewünscht ist:** `VncAuth` **und** Proxy-Auth
   (3.1 + 3.2) gemeinsam einsetzen, ein starkes, eigenes VNC-Passwort wählen,
   und den Bridge-Prozess nur bei Bedarf starten (nicht dauerhaft laufen
   lassen). Die Bridge stirbt mit dem Sandbox-Reset — das ist ein Feature.
3. **Immer:** Den LAN-Guard (`vnc-lan-guard`) unverändert lassen. Er schützt den
   LAN-Weg weiterhin; er ist nur für den Tunnel-Weg nicht zuständig.

## 4. Restrisiken (auch mit Passwort + Proxy-Auth)

* **Brute Force / Credential Stuffing** gegen VNC- und Proxy-Auth. Kein
  Lockout, kein Rate-Limit am Proxy. Starke, zufällige Passwörter sind Pflicht.
* **VncAuth ist schwach** (8 Zeichen, DES, kein Replay-Schutz über Sitzungen).
  Wer den Handshake mitschneiden kann, hat theoretisch Angriffsfläche — im
  TLS/SSH-Transport aber praktisch eingebettet.
* **URL-/ID-Leak.** Die `RUNTIME_ID` ist kein Geheimnis. Der Schutz muss aus
  Passwörtern kommen, nie aus der Unbekanntheit der URL.
* **Kein Zugriffs-Audit.** Weder Proxy noch websockify protokollieren
  erfolgreiche Anmeldungen sinnvoll. Ein Vorfall bliebe unbemerkt.
* **Lebensdauer/Verfügbarkeit.** Der Bridge-Prozess ist an die Sandbox
  gebunden; er ist kein Reboot-fester Dienst. Ein abgestürzter Tunnel lässt den
  Desktop von außen unerreichbar (kein Sicherheits-, aber ein Betriebsrisiko).
* **Ein gekapertes Sandbox-Konto/Token** kann die Bridge erneut starten. Der
  Zugriff auf die Sandbox ist damit gleichwertig zum Desktop-Zugriff.
* **Fehlkonfiguration.** Wird die Bridge einmal mit `-SecurityTypes None`
  gestartet (Board noch ohne Passwort), ist das Fenster offen, bis jemand es
  bemerkt. Deshalb: Passwort **zuerst**, Bridge **danach**.

## 5. Betrieb: `start_novnc_bridge.sh`

Reine Sandbox-/Host-Arbeit, **kein Board-Zugriff**. Die reine Logik (Ports,
öffentliche URL, websockify-Argumente) liegt in `novnc_bridge.py` und ist ohne
Board/Netz getestet (`test_novnc_bridge.py`).

```bash
# Voraussetzung: Tunnel steht (lokaler Endpunkt 127.0.0.1:5901).
sh remote-shell/ha/start_novnc_bridge.sh --dry-run   # zeigen, was passieren würde
sh remote-shell/ha/start_novnc_bridge.sh             # starten (idempotent)
sh remote-shell/ha/start_novnc_bridge.sh --status    # läuft der Prozess?
sh remote-shell/ha/start_novnc_bridge.sh --stop      # stoppen (PIDfile)
sh remote-shell/ha/start_novnc_bridge.sh --help
```

Eigenschaften:

* **Idempotent** — läuft schon eine Bridge (PIDfile), passiert nichts.
* **`--dry-run`** — zeigt argv, Ports, öffentliche URL und ob der Tunnel steht;
  bricht nicht ab.
* **Sauberer Fehler ohne Tunnel** — fehlt der lokale Endpunkt, endet das
  Skript mit Klartext-Hinweis (inkl. Beispiel-Tunnel) und Exit-Code 1.
* **Port-Kollision** — ist Port `12000` schon belegt, bricht es ab, statt einen
  fremden Dienst zu verdrängen.

Optionen (`--listen`, `--upstream`, `--webroot`, `--websockify`, `--runtime-id`,
`--worker`, `--domain`, `--page`) sind auch als Umgebungsvariable nutzbar
(`BRIDGE_*`); `--worker 2` verschiebt auf Port `12001`.

## 6. Tunnel (Voraussetzung, nicht Teil dieses Skripts)

Der Tunnel wird auf dem Odroid aufgebaut und macht dessen `127.0.0.1:5901` in
der Sandbox als `127.0.0.1:5901` erreichbar. Die Form ist ein SSH-Forward
(`tools/remote-shell/reverse_ssh.sh`), z. B.:

```bash
# auf dem Board, so dass die Sandbox lokal 5901 sieht:
ssh -N -R 127.0.0.1:5901:127.0.0.1:5901 <sandbox-ziel>
```

Richtung und Endpunkt hängen davon ab, wie der bestehende Kanal terminiert;
entscheidend ist allein, dass in der Sandbox ein lokaler Endpunkt existiert, der
auf TigerVNC `:1` zeigt (Default `127.0.0.1:5901`). Der Bridge-Aufruf bleibt
davon unabhängig (`--upstream`).

## 7. Prüfen

```bash
# Läuft die Bridge? Hört sie auf 12000?
sh remote-shell/ha/start_novnc_bridge.sh --status
python3 remote-shell/ha/novnc_bridge.py --check-listen --listen 0.0.0.0:12000

# Öffentliche URL (nur sinnvoll, wenn der Proxy die Sandbox gerade bedient):
python3 remote-shell/ha/novnc_bridge.py --print-url --runtime-id <RUNTIME_ID>

# Logik-Tests (kein Board, kein Netz):
python3 remote-shell/ha/test_novnc_bridge.py
```

## 8. Nicht verwechseln

* Port `12000` (work-1) = dieser externe Desktop-Weg **oder** der OTA-Upload.
  Beide nutzen denselben Port; sie schließen sich gegenseitig aus.
* Port `12001` (work-2) = Kommando-Kanal / WS-Terminal.
* Port `6080` am Odroid = noVNC **im LAN** (durch `vnc-lan-guard` geschützt).
* `:6080/` ohne `/vnc.html` = noVNC-Verzeichnisliste (kein Desktop).

## 9. Abgrenzung

Dieses Dokument und die Skripte fassen **keine** Board-Dateien an. Unberührt
bleiben insbesondere `novnc.service`, `xfce-vnc.service`, `deploy_novnc.sh`,
`vnc_lan_guard.py`, `vnc-lan-guard.*` und `novnc-README.md`. Die Änderung an
`xfce-vnc.service` (`-SecurityTypes VncAuth`) sowie das VNC-Passwort werden am
Board vom zuständigen Agenten/Parent gesetzt — hier steht nur die Begründung.
