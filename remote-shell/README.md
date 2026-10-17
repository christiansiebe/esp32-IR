# remote-shell — Odroid von der Sandbox aus steuern

Persistenter, selbstheilender Kanal: **Odroid verbindet sich ausgehend (WSS)** zur Sandbox;
die Sandbox kann den Odroid **nicht** direkt anwählen. Deshalb muss der Sandbox-Server laufen,
bevor der Odroid-Agent sich verbinden kann.

## Schnellstart (Sandbox, nach jedem Sandbox-Reset)
```bash
python3 /workspace/remote-shell/supervise.py           # startet/repariert den Server (idempotent)
python3 /workspace/remote-shell/supervise.py --status  # laeuft? + benoetigte Agent-URL
bash    /workspace/remote-shell/install_autostart.sh   # optional: Hook in ~/.bashrc
```
`supervise.py` leitet die Proxy-URL aus `$RUNTIME_ID` ab:
`wss://work-2-<RUNTIME_ID>.prod-runtime.all-hands.dev/` (Port 12001).

## Befehl senden
```bash
bash /workspace/remote-shell/cmd_send.sh "uname -a"
bash /workspace/remote-shell/cmd_send.sh "systemctl status novnc.service --no-pager" 60
```
Der Odroid zeigt den Befehl und fuehrt ihn nach **lokaler Bestaetigung** aus.
Lange Laeufe: Timeout via `SANDBOX_TERM_CMD_TIMEOUT` (Default 300 s) — oder am Odroid
detached starten (`systemd-run`/`systemctl start`).

`cmd_send.sh` ist **selbstheilend**: laeuft der Server nicht (z.B. nach einem Reset),
startet es `supervise.py` automatisch und wartet auf den Reconnect des Odroid-Agenten (~5 s).

## Mehrere Chats / mehrere Sandboxes

Der Server liegt auf **Sandbox-Ebene** (`/workspace/remote-shell/`). Jeder weitere Chat in
**derselben Sandbox** kann daher sofort `cmd_send.sh` nutzen — kein Setup, kein Token-Handling.
Nur bei einem Chat in einer **anderen Sandbox** verbindet sich ein zweiter Agent dorthin.

**Zweite Sandbox hinzufuegen** (auf dem Odroid, kein Neustart des Kanaels noetig):
```bash
echo 'wss://work-2-<ANDERE_ID>.prod-runtime.all-hands.dev/' >> /etc/odroid-cmd-agent.urls
systemctl restart odroid-cmd-agent.service
```
Der Agent liest die Datei (eine URL pro Zeile, `#` = Kommentar) und haelt **alle** Ziele
gleichzeitig verbunden — je Ziel eigener Reconnect. Alternativ ohne Datei:
`Environment=SANDBOX_TERM_URLS=wss://a/ wss://b/` (leerzeichengetrennt) im Unit setzen.
Alle Ziele teilen sich `SANDBOX_TERM_TOKEN`.

Hinweis: `run_remote_command` im Server **broadcastet** an alle verbundenen Agenten — pro
Sandbox also **genau ein** Ziel eintragen (sonst fuehrt derselbe Befehl mehrfach aus).

### Tote Ziele kosten CPU (Backoff, 2026-10-17)

Das GitHub-Register (`memory/sandboxes.json`) und `$SANDBOX_TERM_URLS_FILE` sammeln **jede**
Sandbox, die es je gab. Eine beendete Sandbox antwortet auf `/agent` mit **HTTP 404**. Ohne
Backoff versuchte der Agent **jede tote URL alle 5 s** erneut — gemessen **9-14 % CPU dauerhaft**
plus hektisches Taktschreiben (`sugov`), was die Last auf ~0,7 hob und verhinderte, dass die
Kerne in den tiefen Takt (250 MHz) fielen. Zwei Aenderungen:

- `cmd_agent.next_delay()` — exponentieller Backoff je Ziel. **404** (Sandbox tot) bis **900 s**,
  jeder andere Fehler (eigener Server kurz weg) bis **60 s**; nach erfolgreichem Connect wieder 5 s.
  Wirkung: gemessen **0,03 % CPU** statt 9-14 %, keine 404-Wiederholungen mehr.
- `register_sandbox.prune_sandboxes()` — Eintraege, die laenger als **24 h** nicht aktualisiert
  wurden, fallen aus dem Register. So waechst die Liste nicht endlos.

Host-Test: `python3 remote-shell/test_channel_backoff.py` (15 Checks, ohne Netz).

**Wichtig:** Zusaetzliche tote URLs gar nicht erst eintragen. Ein verwaister Eintrag in
`/etc/odroid-cmd-agent.urls` oder im Register kostet nur Verbindungsversuche.

### Nie am Kanal der Produktions-WLAN-Karte drehen (2026-10-17)

Der Odroid hat **eine** WLAN-Karte (`rtw88_8822bu`, `wlxdc627949ba79`) und ist damit im
5-GHz-Heimnetz (Kanal 36). Ein Monitor-Interface laesst sich zwar anlegen (`iw list` zeigt
`* monitor`), aber die Karte ist **einkanalig**: sie folgt dem Kanal des Managed-Interfaces.
Ein Versuch, das Monitor-Interface auf einen **anderen** Kanal zu stellen, liefert
`Device or resource busy` — und das Anlegen/Hochnehmen kann die Managed-Verbindung
**abreissen**. Genau das hat das Board einmal aus dem WLAN geworfen (Agent weg, nur per
Neustart zurueck).

Regel: Monitor-Mode **nur** auf einer **zweiten** WLAN-Karte, oder auf demselben Kanal wie
die bestehende Verbindung. Fuer ESP-NOW (Kanal 1) heisst das: entweder zusaetzlichen
USB-WLAN-Adapter stecken, oder die Verbindung auf das 2,4-GHz-Netz legen, das ebenfalls auf
Kanal 1 sendet.

## Dateien
- `supervise.py` — Supervisor (flock-geschuetzt, startet Server bei Absturz neu, `--status`/`--stop`)
- `sandbox_term_server.py` — WS-Server: PTY-Terminal + `/agent`-Befehlskanal
- `cmd_send.sh` — Befehle ueber die Queue senden
- `cmd_agent.py` — Odroid-Seite (URL aus `$SANDBOX_TERM_URL` oder Argument)
- `token` (0600) — shared secret; wird **pro Verbindung** gelesen → Rotation = Datei ersetzen
- `install_autostart.sh` — Autostart-Hook fuer `~/.bashrc`
- `run/` — `server.log`, `supervisor.pid`, `server.pid`, `supervisor.lock`

## Odroid-Seite (reboot-fest)
`/etc/systemd/system/odroid-cmd-agent.service` — `Restart=always`, Token inline.
Nach Sandbox-Wechsel **nur** die URL in `ExecStart` anpassen (oder `EnvironmentFile` nutzen):
```bash
sed -i -E 's|wss://work-[0-9]+-[a-z0-9]+\.prod-runtime|wss://work-2-NEUERID.prod-runtime|' \
  /etc/systemd/system/odroid-cmd-agent.service
systemctl daemon-reload && systemctl restart odroid-cmd-agent.service
```

## Betrieb auf dem Odroid (diese Session eingerichtet)
- **Taegliches inkrementelles Backup**: `backup-daily.timer` → `/root/backup-daily.sh`
  (rsync `--link-dest`, nur geaenderte Daten, 14 Tage, `/mnt/ssd/backups/rsync/`).
- **Desktop im Browser (noVNC)**: `novnc.service` (websockify `6080` → VNC `5901`),
  `xfce-vnc.service` (TigerVNC `:1`). Aufruf: `http://192.168.178.71:6080/vnc.html`
  (bzw. `http://odroidn2.local:6080/vnc.html`).
  **Wichtig:** `/vnc.html` nicht weglassen. `:6080/` allein zeigt die
  noVNC-**Verzeichnisliste** von `/usr/share/novnc/` - das sieht wie ein
  Datei-Explorer aus, ist aber nicht der Desktop.

Details/Historie: gemeinsames Gedaechtnis `remote-shell/memory/SHARED.md` (live: `/workspace/MEMORY.md`).
