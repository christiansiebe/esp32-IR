# AGENTS.md — gemeinsames Gedaechtnis & Odroid-Kanal

Dieses Repository ist das **gemeinsame, persistente Gedaechtnis** aller
OpenHands-Chats an diesem Projekt. Jeder Chat soll es lesen und ergaenzen.

## 1. Gemeinsames Gedaechtnis (lesen + schreiben)
- Kanonische Datei (GitHub):
  https://github.com/christiansiebe/esp32-IR/blob/main/memory/SHARED.md
- Live-Kopie in jeder Sandbox: `/workspace/MEMORY.md`
  (Symlink: `~/.openhands/memory/MEMORY.md` -> `/workspace/MEMORY.md`)
- Neue Erkenntnisse am Ende der Arbeit dort ergaenzen.

## 2. Odroid-Kanal (falls die Sandbox kanalfaehig ist)
- Der Odroid N2+ (`odroidn2`) verbindet sich automatisch zu **jeder** Sandbox,
  die sich im Register `memory/sandboxes.json` eintraegt.
- Befehl an den Odroid senden:
  `bash /workspace/remote-shell/cmd_send.sh "<befehl>" [timeout]`
- Kurzanleitung: `tools/remote-shell/OTHER-CHAT.md`
- Eine Sandbox kanalfaehig machen: `tools/remote-shell/bootstrap.sh`

## 3. Automatik
`.openhands/setup.sh` laeuft bei jedem Agent-Start und ruft
`tools/remote-shell/bootstrap.sh` auf -> Kanal, Registrierung und
Memory-Sync laufen ohne manuelle Eingabe.
