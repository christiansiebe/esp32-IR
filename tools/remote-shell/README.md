# Remote-Zugriff auf die Sandbox (Token-Auth)

Ziel: Von deinem **Odroid (Armbian)** aus auf die **Sandbox** zugreifen — mit
Token-Authentifizierung, ohne offenen Port bei dir.

## Ausgangslage (in der Sandbox getestet)
- **Ausgehend** ist frei (Internet, SSH-Client) ✅
- **Eingehend** nur über den **Runtime-Proxy** (2 öffentliche URLs, Ports
  12000/12001) ✅ — **WebSockets werden durchgereicht** (verifiziert) ✅
- Rohes `ssh -p 12000` durch den Proxy geht **nicht** (Proxy terminiert TLS/HTTP).

Deshalb zwei Wege. Beide brauchen nur, dass die Sandbox **ausgehend** darf —
das kann sie.

---

## Option A — Shell im Terminal des Odroid (empfohlen, kein offener Port)

Ablauf:
1. In der **Sandbox** läuft `sandbox_term_server.py` (WS→PTY, Token-geschützt).
2. Auf dem **Odroid** läuft `sandbox_term_client.py`, verbindet sich **ausgehend**
   per WSS zur Proxy-URL und gibt die Sandbox-Shell im lokalen Terminal aus.
   → Du tippst auf dem Odroid, die Befehle laufen in der Sandbox.

```bash
# --- Sandbox ---
export SANDBOX_TERM_TOKEN="$(python3 -c 'import secrets;print(secrets.token_urlsafe(32))')"
export SANDBOX_TERM_PORT=12001
python3 tools/remote-shell/sandbox_term_server.py

# --- Odroid ---
python3 -m pip install websockets
export SANDBOX_TERM_TOKEN='<dasselbe Token>'
python3 sandbox_term_client.py 'wss://work-2-<id>.prod-runtime.all-hands.dev/'
```

Sicherheit: ohne/mit falschem Token → `close 1008 unauthorized` (getestet).
Es lauscht auf `0.0.0.0:12001`; öffentlich nur über den Proxy.

---

## Option B — SSH-Reverse-Tunnel (dein `ssh`-Client, wie gewohnt)

Setzt voraus, dass die **Sandbox den Odroid ausgehend erreicht** (`ssh` auf
Host/IP:Port). Dann baut die Sandbox einen Reverse-Tunnel: die **Odroid-SSH-Shell
bleibt deine Konsole**, zusätzlich wird die Sandbox-Shell auf
`127.0.0.1:2222` des Odroid freigelegt.

```bash
# --- Odroid: /etc/ssh/sshd_config ---
GatewayPorts no          # Standard; nur localhost-Bindung genuegt
# ggf. AddReverseForward (OpenSSH >= 9.8) fuer RemoteForward

# --- Sandbox ---
export SANDBOX_SSH_HOST=<odroid-ip-oder-host>
export SANDBOX_SSH_USER=<dein-user>
export SANDBOX_SSH_KEY=~/.ssh/id_ed25519      # Key auf dem Odroid autorisieren
export SANDBOX_SSH_TOKEN=<tunnel-token>
tools/remote-shell/reverse_ssh.sh -R          # nur Tunnel
tools/remote-shell/reverse_ssh.sh             # interaktive Odroid-Shell
```

Danach auf dem Odroid: `python3 sandbox_term_client.py ws://127.0.0.1:2222/`
(mit demselben Token) — oder direkt die Sandbox-Shell nutzen.

> Hinweis: Auf einem `sshd` mit `AllowTcpForwarding no` oder einem
> `Match`-Block, der `RemoteForward` verbietet, schlägt `-R` fehl. Dann
> Option A nehmen.

---

## Option C — Kommando-Kanal: Agent führt auf dem Odroid aus, DU bestätigst

Ziel: Der Agent kann Befehle **auf dem Odroid** ausführen (z. B. btrfs prüfen),
aber **nur nach deiner Bestätigung** am Odroid-Terminal.

```
Agent (Sandbox)  --Befehl-->  Server  --WS-->  cmd_agent.py (Odroid)
                                                   |
                                            Anzeige + [y]/[a]/[N]
                                                   |
Agent (Sandbox)  <--Ergebnis--  Server  <--WS---  Ausführung
```

**Auf dem Odroid** (eigenes Terminal, als root):
```bash
export SANDBOX_TERM_TOKEN='<token>'
python3 cmd_agent.py 'wss://work-2-<id>.prod-runtime.all-hands.dev/'
```
Optionen am Agenten:
- **`y`** = einmal ausführen · **`a`** = immer (bis Ctrl-C) · **`N`** = ablehnen
- **Ctrl-C** schaltet die Bestätigungspflicht um (Pause-Modus)

**Auf der Sandbox** (hier) senden:
```bash
tools/remote-shell/cmd_send.sh "btrfs subvolume list /" 60
```

Regeln:
- Bestätigungspflicht ist **Standard** und **serverseitig nicht abschaltbar**.
- Jede Ausführung/Ablehnung wird in `$CMD_BASE/audit.log` protokolliert.
- Ergebnis in `$CMD_BASE/results/<id>.out` / `.json`.

> Hinweis: `a` (immer) ist bequem, aber dann läuft alles ohne Rückfrage — für
> heikle Befehle besser einzeln bestätigen. `rm -rf` o. Ä. gehört **nicht** in
> den Kanal.

---

## Welche Option?
- **Nur die Sandbox bedienen** → **A**.
- **Deine Odroid-Konsole behalten und zusätzlich die Sandbox** → **B**.
- **Soll der Agent auf dem Odroid Befehle ausführen** → das ist die Umkehrung:
  die Sandbox verbindet sich als **Client** (`ssh user@odroid`) — dafür braucht
  der Odroid einen erreichbaren `sshd` (Tailscale/LAN/Portfreigabe).

## Sicherheit
- **Immer** ein starkes Token (`secrets.token_urlsafe(32)`); nie ohne Token starten
  (der Server verweigert den Start, wenn `SANDBOX_TERM_TOKEN` fehlt).
- Token nur per Umgebungsvariable/Datei übergeben, **nie committen**.
- Nach Gebrauch den Server beenden — er läuft sonst öffentlich (wenn auch geschützt).
- Empfehlung: Odroid per **WireGuard/Tailscale** ins gleiche Netz holen; dann ist
  alles nur im privaten Netz erreichbar und der öffentliche Proxy entfällt.

---

## Selbstheilung / Verbindung nie verlieren

Ziel: Der Kontroll-Chat soll die Verbindung zum Odroid **nicht verlieren** und
sie **selbststaendig wiederherstellen**.

**Odroid-Seite (reboot-fest):**
- systemd-Dienst `odroid-cmd-agent.service` (`Restart=always`, `RestartSec=5`,
  `enabled`), enthaelt Token + `CMD_AGENT_AUTO=1` (unbeaufsichtigter Betrieb).
- Faellt der Agent aus, verbindet er sich in 5 s neu; nach Reboot startet er
  automatisch, sobald Netz da ist (`After=network-online.target`).
- Achtung: `pkill -f cmd_agent.py` aus dem Kanal heraus toetet die eigene Shell
  (der Befehlstext enthaelt den Suchstring) -> Agent per `kill <pid>` beenden.

**Sandbox-Seite:**
- `sandbox_term_server.py` laeuft als Daemon; die Datei-Queue
  (`/tmp/cmdchannel`) puffert Befehle, bis der Agent wieder da ist.
- Nach einem Sandbox-Reset: Server mit **demselben Token** neu starten
  (`SANDBOX_TERM_TOKEN=... python3 tools/remote-shell/sandbox_term_server.py`);
  der Odroid-Agent verbindet sich dann von selbst wieder.

**Diagnose bei Verbindungsverlust:**
- `tail -5 /tmp/cmdchannel/audit.log` (letzte connect/disconnect/timeouts)
- `pgrep -af sandbox_term_server.py`

