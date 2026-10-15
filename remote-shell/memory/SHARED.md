# Shared Memory — OpenHands chats (esp32-IR / Odroid / Home Assistant)

Diese Datei ist das **gemeinsame, persistente Gedaechtnis** fuer OpenHands-Agent-Chats,
die an diesem Projekt arbeiten. Sie uebersteht Sandbox-Resets und ist ueber Sandboxes
hinweg teilbar.

> ⚠️ Dieses Repo ist **oeffentlich** — hier NIEMALS Tokens/Keys/Passwoerter ablegen.
> Geheimnisse bleiben in ihren eigenen Dateien (`/workspace/remote-shell/token`,
> systemd-`Environment=`, HA-`.storage`).

## Benutzung (fuer JEDEN Chat)
- **Live-Kopie** (persistent, sandbox-weit, wird jedem Chat automatisch als User-Memory
  injiziert): `/workspace/MEMORY.md`
  (`~/.openhands/memory/MEMORY.md` ist ein Symlink darauf).
- **Aktualisieren von GitHub holen:**  `bash /workspace/remote-shell/sync-memory.sh pull`
- **Eigene Aenderungen hochladen:**   `bash /workspace/remote-shell/sync-memory.sh push`
- **Repo-Datei:** `remote-shell/memory/SHARED.md` (Branch `cline/power-history-diag`).

## Fernsteuerung: Odroid (Armbian + Home Assistant)
- Der Odroid wird ueber einen **ausgehenden** WSS-Kanal gesteuert (kein eingehender Port).
- Jeder Chat in derselben Sandbox kann sofort:
  `bash /workspace/remote-shell/cmd_send.sh "<befehl>"`
  (selbstheilend; startet den Server bei Bedarf selbst).
- Architektur/Details: `/workspace/remote-shell/README.md` und `remote-shell/AGENTS.md`.
- Erreichbarkeit: Odroid nur per IPv6 (DS-Lite); die Sandbox hat kein IPv6 -> direkter
  Zugriff unmoeglich, nur der ausgehende Weg funktioniert.

## Konventionen
- Keine Geheimnisse in dieser Datei (oeffentliches Repo).
- Kurze, dauerhafte Fakten hier; ausfuehrliche Historie in datierten Logs.
- Persistenter, geteilter Speicher der Sandbox: `/workspace` (eigener Mount).

<!-- Chats: unten mit Eurem Namen/Datum anhaengen, kurz halten. -->

## Eintraege
- 2026-10-15 — Kanal + geteiltes Gedaechtnis eingerichtet (remote-shell, sync-memory.sh).

## Mehrere Sandboxes + Cloud-API-Zeiger (2026-10-15)
- Jede Sandbox traegt sich selbst in `remote-shell/memory/sandboxes.json` ein (Branch `main`).
  Der Odroid liest das Register und verbindet sich zu ALLEN -> kein Eintrag pro Sandbox.
- Zeiger fuer jeden Chat: `remote-shell/AGENTS.md` + `.openhands/setup.sh` im Repo (Branch `main`
  UND `cline/power-history-diag`). Zusaetzlich Cloud-API-Tag
  `memory=shared-memory:remote-shell/memory/SHARED.md` an allen Conversations.
- **Wichtig:** Cloud-Secrets (`SANDBOX_TERM_TOKEN`, `GITHUB_WRITE_TOKEN`) erreichen nur
  Sandboxes, die NACH dem Anlegen des Secrets gestartet wurden. Aeltere Sandboxes haben
  leere Env-Vars -> dort Token/Bootstrap per Cloud-API setzen:
  `python3 remote-shell/remote_exec.py <conversation_id> "<cmd>"`.
- Server liest den Token PRO VERBINDUNG; Datei `/workspace/remote-shell/token` hat Vorrang
  vor der Umgebung -> Token live rotierbar.
- Token-Rotation: Odroid-Token in `/etc/systemd/system/odroid-cmd-agent.service`
  (`Environment=SANDBOX_TERM_TOKEN=...`) ist die Wahrheit; Cloud-Secret darauf angleichen.

## Verifiziert (2026-10-15)
- Odroid verbindet automatisch zu BEIDEN Sandboxes (`fkhhrtgachhziqps`, `zvqpzxrtfjcxbcgw`).
- E2E: Befehl aus der Fremd-Sandbox heraus -> Odroid antwortet (`E2E-OK`, `odroidn2`).
- Zeiger gesetzt: `remote-shell/AGENTS.md`, `.openhands/setup.sh`, Cloud-Tag an 12 Conversations.

## Repo-Layout (2026-10-15)
- Alles liegt gesammelt unter **`remote-shell/`** (Toolkit, `memory/`, `AGENTS.md`),
  damit der ESP-IDF-Firmware-Baum (`main/`, `components/`, `tools/`, `CMakeLists.txt`)
  unberuehrt bleibt. Im Root liegt nur der zwingende Hook `.openhands/setup.sh`.
- Odroid-Register-URL jetzt: `.../main/remote-shell/memory/sandboxes.json`
  (per systemd-Drop-in `odroid-cmd-agent.service.d/registry.conf`).
- Alte Pfade (`tools/remote-shell/*`, Root-`AGENTS.md`, `memory/*`) sind geloescht.
- `remote-shell/publish.py publish|delete` pflegt/entfernt die Repo-Dateien.
