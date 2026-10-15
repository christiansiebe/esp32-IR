# Anleitung: Odroid aus diesem Chat steuern (kopierbar)

> Gilt für **jeden Chat in dieser Sandbox**. Kein Token/Setup nötig.

## 1) Kanal starten (einmalig, selbstheilend, idempotent)

```bash
python3 /workspace/remote-shell/supervise.py
```

## 2) Befehle an den Odroid senden

```bash
bash /workspace/remote-shell/cmd_send.sh "<befehl>"
```

Beispiele:

```bash
bash /workspace/remote-shell/cmd_send.sh "uname -a; hostname"
bash /workspace/remote-shell/cmd_send.sh "systemctl is-active homeassistant"
bash /workspace/remote-shell/cmd_send.sh "docker ps --format '{{.Names}}'" 60
```

## 3) Geteiltes Gedächtnis (alle Chats)

```bash
cat /workspace/MEMORY.md                                  # lesen (wird auch auto-injiziert)
bash /workspace/remote-shell/sync-memory.sh pull          # von GitHub holen
bash /workspace/remote-shell/sync-memory.sh push          # eigene Notizen hochladen
```

## Wichtig

- Der Odroid verbindet sich **ausgehend** zur Sandbox — der Server (Schritt 1) muss laufen.
- `cmd_send.sh` startet den Server bei Bedarf selbst; Schritt 1 ist nur die Absicherung.
- Der Odroid bestätigt jeden Befehl lokal (Auto-Ja ist im systemd-Service aktiv).
- **Andere Sandbox:** kein Eintrag auf dem Odroid nötig. Jede Sandbox trägt sich beim
  Start selbst in `remote-shell/memory/sandboxes.json` ein; der Odroid liest dieses
  Register automatisch und verbindet sich zu allen. Nur Schritt 1 dort ausführen
  (oder `remote-shell/bootstrap.sh`, das erledigt alles).

