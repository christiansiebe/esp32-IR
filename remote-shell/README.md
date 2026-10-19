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

## Cline Cloud: kein oeffentlicher Port -> Reverse-Tunnel (2026-10-19)

Eine Cline-Cloud-Sandbox hat **keinen** oeffentlichen HTTP-Eingang: es gibt kein
`RUNTIME_ID`, kein `work-N-<id>.prod-runtime.all-hands.dev` (alte URLs liefern
HTTP 404), und im Container lauscht nur der Cline-Hub (Port 8080/8081, Pfad
`/hub`) — nicht erreichbar von aussen. Der Odroid kann die Sandbox daher **nicht**
direkt anwaehlen; erreichbar wird sie nur ueber einen Tunnel, den die Sandbox
selbst **ausgehend** aufbaut.

```bash
bash /workspace/remote-shell/tunnel.sh start   # -> https://<name>.serveousercontent.com
bash /workspace/remote-shell/tunnel.sh status
python3 /workspace/remote-shell/supervise.py --status   # zeigt dieselbe Agent-URL
```

`tunnel.sh` haelt einen SSH-Reverse-Tunnel (`TUNNEL_HOST`, Default
`serveo.net`) auf Port `SANDBOX_TERM_PORT` (12001) und schreibt die URL nach
`run/public-url.txt`; `supervise.py`/`register_sandbox.py` lesen sie. Die URL
wechselt bei jedem Neustart des Tunnels.

**Protokoll unveraendert:** gleicher WS-Server, gleicher Pfad `/agent`, gleiche
Authentifizierung `?token=<SANDBOX_TERM_TOKEN>` (Vergleich per
`hmac.compare_digest`). Der vorhandene 43-Zeichen-Token kann unveraendert
weiterverwendet werden; der Server liest ihn **pro Verbindung** aus
`remote-shell/token` (0600), Rotation = Datei ersetzen, **kein Neustart**.
Token niemals committen (`.gitignore`) oder ausgeben.

Getestet (2026-10-19, durch den oeffentlichen Tunnel): authentifizierter
`/agent`-Connect + `hostname` -> Antwort `ins-...`; falscher Token ->
`closed 1008 unauthorized`; 43-Zeichen-Token + Rotation ohne Serverneustart ok.
`cloudflared tunnel --url ...` war hier **nicht** nutzbar (Tunnel registriert,
aber der Quick-Tunnel-Hostname lieferte NXDOMAIN).

**Sicherheit:** Der Relay terminiert TLS (Befehle sind dort im Klartext
sichtbar). Deshalb zusaetzlich im Agenten:

```bash
# nur erlaubte Befehle + Datei-Verben, eingesperrt auf ~/odroid-channel-test
python3 cmd_agent.py --allowlist odroid_allowlist.txt 'wss://<tunnel-host>/'
```

`@read test.txt` / `@write test.txt <base64>` laufen ueber denselben
`cmd`-Frame und werden von `cmd_policy.py` auf `$CMD_AGENT_FILE_ROOT`
(Default `~/odroid-channel-test`) eingesperrt — absolute Pfade, `..` und
Symlink-Ausbrueche werden abgelehnt.

Schritt-fuer-Schritt-Reparatur mit allen Odroid-Befehlen:
**`remote-shell/CLINE-ODROID-REPAIR.md`**.

## Betrieb auf dem Odroid (diese Session eingerichtet)
- **Taegliches inkrementelles Backup**: `backup-daily.timer` → `/root/backup-daily.sh`
  (rsync `--link-dest`, nur geaenderte Daten, 14 Tage, `/mnt/ssd/backups/rsync/`).
- **Desktop im Browser (noVNC)**: `novnc.service` (websockify `6080` → VNC `5901`),
  `xfce-vnc.service` (TigerVNC `:1`). Aufruf: `http://192.168.178.71:6080/vnc.html`.

Details/Historie: gemeinsames Gedaechtnis `remote-shell/memory/SHARED.md` (live: `/workspace/MEMORY.md`).
