# AGENTS.md — gemeinsames Gedaechtnis & Odroid-Kanal

> Alles zu diesem Thema liegt gesammelt unter **`remote-shell/`**, damit der
> ESP-IDF-Firmware-Baum (`main/`, `components/`, `tools/`, `CMakeLists.txt`)
> unberuehrt bleibt. Im Repo-Root liegt nur der zwingende `.openhands/setup.sh`.

Dieses Repository ist das **gemeinsame, persistente Gedaechtnis** aller
OpenHands-Chats an diesem Projekt. Jeder Chat soll es lesen und ergaenzen.

## 1. Gemeinsames Gedaechtnis (lesen + schreiben)
- Kanonische Datei (GitHub):
  https://github.com/christiansiebe/esp32-IR/blob/main/remote-shell/memory/SHARED.md
- Live-Kopie in jeder Sandbox: `/workspace/MEMORY.md`
  (Symlink: `~/.openhands/memory/MEMORY.md` -> `/workspace/MEMORY.md`)
- Neue Erkenntnisse am Ende der Arbeit dort ergaenzen.

## 2. Odroid-Kanal (falls die Sandbox kanalfaehig ist)
- Der Odroid N2+ (`odroidn2`) verbindet sich automatisch zu **jeder** Sandbox,
  die sich im Register `remote-shell/memory/sandboxes.json` eintraegt.
- Befehl an den Odroid senden:
  `bash /workspace/remote-shell/cmd_send.sh "<befehl>" [timeout]`
- Kurzanleitung: `remote-shell/OTHER-CHAT.md`
- Eine Sandbox kanalfaehig machen: `remote-shell/bootstrap.sh`

## 3. Automatik
`.openhands/setup.sh` laeuft bei jedem Agent-Start und ruft
`remote-shell/bootstrap.sh` auf -> Kanal, Registrierung und Memory-Sync
laufen ohne manuelle Eingabe.
