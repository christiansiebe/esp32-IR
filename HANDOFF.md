# Übergabe für neuen Chat — esp32-IR

Kurz, selbsttragend. Diese Datei in einen neuen Chat mit mir einfügen (oder den Pfad nennen),
dann läuft es ohne Vorgeschichte weiter.

## Projekt

- Repo: `christiansiebe/esp32-IR` (ESP-IDF, ESP32, PlatformIO-frei).
- Zwei Rollen: **Sender** (liest IR/SML am Zähler, sendet per ESP-NOW) und
  **Empfänger** (Touch-Display, Webserver, SPIFFS-Historie, SNTP).
- Hauptbranch: `cline/power-history-diag`, **PR #1 offen** (`CLEAN`),
  Titel „Add persistent power history, on-demand diagnostics, crash journal, and complete CI flash artifacts".
- Basis `main` = `origin/main` = `0070840`.
- **Non-negotiable / darf nicht brechen:** ESP-NOW, OTA, Partition-Layout, Rollen.

## Git-Stand (geprüft)

| Ref | Commit | Status |
|---|---|---|
| `origin/main` | `0070840` | ok |
| `origin/cline/power-history-diag` | `3f58fec` | ok |
| lokal `cline/power-history-diag` | 1 Commit **unpushed** (oberster lokaler Commit) |
| `origin/recovered/ams-features-salvage` | `d10ea8c` | ✅ schon gepusht |

- Unpushed: der oberste lokale Commit („TODO/HANDOFF: record_count boot bug, partition change, binding SPIFFS rule").
- Lokal liegen keine anderen unpushed Commits. Kein `recovered/ams-features-salvage`-Push mehr nötig.

## ⚠️ Push blockiert — zuerst lösen

In der letzten Session gab es **keinen schreibfähigen Zugang** (hart geprüft):

- `GITHUB_TOKEN` = GitHub-App-Token (`ghu_…`, 40 Z.) → **`403 Permission … denied`** (read-only).
- `GITHUB_WRITE_TOKEN` = 93 Z., Prefix `githu…` → **`401 Bad credentials`** (ungültig/abgelaufen),
  für API *und* Git.

**Aktion:** fine-grained PAT (`github_pat_…`, *Contents: Read and write*, optional
*Workflows: Read and write*) auf `christiansiebe/esp32-IR` anlegen und als Secret
`GITHUB_WRITE_TOKEN` in OpenHands hinterlegen. Dann in **diesem** neuen Chat:

```bash
git remote set-url origin "https://x-access-token:${GITHUB_WRITE_TOKEN}@github.com/christiansiebe/esp32-IR.git"
git push origin cline/power-history-diag      # schiebt den lokalen Commit nach
```

Erst danach ist der Push-Blocker in der TODO.md abzuhaken.

## ⚠️ Verbindliche Regel: SPIFFS-Historie darf nie verloren gehen

- Partition `storage`: **Offset 0x320000, Größe 0xE0000 (917504 B)**, Datei
  `/powerlog/quarters.bin`, bis 30000 Minutenwerte.
- Offset/Größe **niemals** ändern oder verschieben.
- Flash-Bundles/`flasher_args.json` dürfen `storage` **nicht** schreiben
  (umgesetzt: `e944e04`/`aac6ee0`).
- Kein `erase-flash`/Full-Chip-Erase beim Deployment.
- Nach jedem Deployment prüfen: `record_count > 0`, kein RAM-Fallback (Akzeptanzkriterium).

## 🐛 Offener Bug: `record_count` nach Boot nicht wiederhergestellt

Symptom: Nach Reboot startet `record_count` bei 0 statt beim gespeicherten Wert
(ggf. RAM-Fallback trotz vorhandener Datei). Nächste Schritte:

1. Boot-Logzeile `Persistent history ready: N of 30000` gegen `record_count` in
   `/api/power-history` abgleichen.
2. Header-Handling prüfen (Fix `1612975`: Fresh-Header + In-Place-Reparatur) — gültiger
   Bestand darf nicht als „leer" gelten.
3. Migration prüfen (`4af921a`): alte `format_version` darf Datensätze nicht verwerfen.

## Partitionstabelle (geändert, `c3d3420`)

```
ota_0    app  ota_0   0x20000  0x180000
ota_1    app  ota_1   0x1a0000 0x180000
storage  data spiffs  0x320000 0xE0000   # unverändert, nicht verhandelbar
```

- `factory` entfernt; App flasht jetzt nach **`ota_0@0x20000`** (vorher `factory@0x20000`).
- Kein Factory-Fallback; Rollback in den anderen OTA-Slot via Boot-Confirmation.
- **Erstes Deployment = Full-Flash.**

USB-Full-Flash (Empfänger), `storage` **nicht** flashen:

```
0x1000   bootloader.bin
0x8000   partition-table.bin
0xf000   ota_data_initial.bin
0x20000  receiver-sml.bin
```

Sender: OTA über `http://192.168.4.1/update` → „Sender via ESP-NOW", oder per USB `sender-sml.bin`.

## Offene Tasks (Details in `TODO.md`)

- `record_count`-Bug beheben (oben).
- Push-Blocker lösen (PAT), den lokalen Commit pushen.
- AMS-Übernahmen integrieren (geborgene Module liegen auf `origin/recovered/ams-features-salvage`):
  `health_*`, `mqtt_*` (HA-Discovery/LWT), `boot_confirm`/`boot_policy`, `recovery_*`, Host-Tests,
  CMakeLists/sdkconfig. Stand: rekonstruiert, **nicht** integriert, nicht baubar.
- `main.c`: Sender-Diag-Zeitstempel als Uptime statt 1970; Sender→Empfänger-RSSI ausweisen.
- LICENSE + README fehlen.
- Persistenz: letzte bekannte Zeit persistieren; Tages-/Monats-Summen im Flash.

## Zuletzt erledigt (in Arbeit)

- Persistenz-Migration `4af921a` (alte Record-Formate migrieren statt verwerfen).
- Stabilitäts-/Fault-Injection-Harness `8764f22` (ASan/UBSan, `test/stability/`).
- Reboot-Persistenz-Host-Test (echter Init→Schreiben→Reboot-Zyklus, `fork()`),
  belegt `record_count`-Wiederherstellung (rot/grün).
- Post-OTA-Rollback-Bestätigung für den **Empfänger** (`boot_policy`/`boot_confirm`,
  Host-Test `test_boot_policy.c`) — **Hardware-Verifikation offen**.
- CI baut beide Rollen; Flash-Artefakt schreibt `storage` bewusst nicht.
- ESP-NOW-Wall-Clock-Sync (Sender fragt Empfänger) — Feldtest offen, `clock calibrations` war 0.

## Tests

```bash
./test/run_host_tests.sh     # Host-Tests + Stabilitäts-Harness, lokal grün
```
