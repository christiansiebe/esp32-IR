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
- **Repo-Datei:** `memory/SHARED.md` (Branch `cline/power-history-diag`).

## Fernsteuerung: Odroid (Armbian + Home Assistant)
- Der Odroid wird ueber einen **ausgehenden** WSS-Kanal gesteuert (kein eingehender Port).
- Jeder Chat in derselben Sandbox kann sofort:
  `bash /workspace/remote-shell/cmd_send.sh "<befehl>"`
  (selbstheilend; startet den Server bei Bedarf selbst).
- Architektur/Details: `/workspace/remote-shell/README.md` und `AGENTS.md` im Projekt-Root.
- Erreichbarkeit: Odroid nur per IPv6 (DS-Lite); die Sandbox hat kein IPv6 -> direkter
  Zugriff unmoeglich, nur der ausgehende Weg funktioniert.

## Konventionen
- Keine Geheimnisse in dieser Datei (oeffentliches Repo).
- Kurze, dauerhafte Fakten hier; ausfuehrliche Historie in datierten Logs.
- Persistenter, geteilter Speicher der Sandbox: `/workspace` (eigener Mount).

<!-- Chats: unten mit Eurem Namen/Datum anhaengen, kurz halten. -->

## Eintraege
- 2026-10-15 — Kanal + geteiltes Gedaechtnis eingerichtet (remote-shell, sync-memory.sh).
