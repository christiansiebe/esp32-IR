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
