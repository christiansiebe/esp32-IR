# Branch- & Isolations-Inventar (ESP32-IR)

Stand: 2026-11-10 · Branch `feat/c6-burst-lock` · rein lesend erstellt (nichts committet, nichts gepusht).

Diese Datei macht sichtbar, welche Branches im Repo tatsächlich isoliert/verwaist liegen und
was davon echte (ungemergte) Arbeit trägt gegenüber bloßen toten Kopien.

## Verwaiste Commits (isoliert, kein Branch zeigt darauf)

| Commit | Autor-Datum | Betreff | Echte Arbeit? | Sicherung |
|---|---|---|---|---|
| `1b4389f7567d60abe21d81e3d6bdc6ab44c4327d` | 2026-11-09 | **sync-slot + sml ack handshake (WIP)** | **JA — ungemergt** | Tag/Branch `archive/sync-slot-wip` |
| `8d8c90c7ef1538c6df164bb6dd24e4e330b47647` | 2026-11-09 | burst-locked receive window (Variante) | **Nein** — baum-identisch mit gemergtem `008fe26` | Tag/Branch (Archiv) |
| `5e6b31034637f18d8785c747f1c0c7e2fe9bda72` | 2026-11-09 | fix(wifi): reconnect listen_interval out of band | **Nein** — gleiches Thema via `07419b1` im main | Tag/Branch (Archiv) |

### `1b4389f` — der sync-slot-Versuch (wichtig)

- Liegt im **Git-Stash** (`refs/stash@{0}`), nicht in einem Branch → für `git log` unsichtbar.
- Inhalt: beidseitiges **synchronisiertes Empfangsfenster** (Option 3 im früheren Design):
  - `ESPNOW_SYNC_SLOT_MS` (Kconfig, default 0, range 0–1000) — Sender+Empfänger auf geteilter
    Wall-Clock (bestehendes Time-Sync); Empfänger öffnet `WIFI_PS_NONE`-Fenster je 5-s-Slot,
    parkt dazwischen `WIFI_PS_MIN_MODEM`.
  - `sync_slot_worker()` (`main/main.c`) — steuert Wake/PS je Slot.
  - `SML_TX_ACK_WAIT_MS 1200` + `sml_transport_ack_t` — Sender wartet blockierend 1200 ms auf
    das Empfänger-ACK („sie warten aufeinander").
- **Status:** WIP (work-in-progress), durch den neuen `feat/c6-burst-lock`-Ansatz (unabhängiges
  ACK `SML_ACK_TIMEOUT_MS 80` + burst-lock-Vorhersage) **ersetzt/nicht weiterverfolgt**. Grund für
  die Aufgabe nicht im Repo dokumentiert; siehe CHAT-THREAD/Design-Docs.

## Lokale Branches

| Branch | Status |
|---|---|
| `feat/c6-burst-lock` (aktuell) | aktive Arbeit: burst-lock + ACK |
| `feat/c6-led-lowpower` | alt (hat verwaisten Stash erzeugt) |
| `feat/nvs-web-knobs` | gemergt (in main) |
| `tmp-bigfix` | temporär |

## Remote-Branches (Gruppierung nach Abstand zu `origin/main`)

### Mit `behind=0` (stehen auf main-Spitze oder haben nur eigenen Stand) — „aktuell"
`fix/versioning-restore (0/26)`, `fix/c6-bigfix (0/21)`, `feat/c6-twt (0/28)`,
`feat/c6-rgb-led (0/28)`, `feat/c6-ap-switch (0/20)`, `feat/esp32c6-receiver (0/19)`,
`feat/nvs-usb-provision (0/19)`, `feat/nvs-web-knobs (0/36)`, `no-local-build (0/1)`.

### Kleiner Abstand (nur wenige Commits hinter/über main) — vermutlich kurzlebig/offen
z.B. `feat/boot-button-config-restore (5/13)`, `feat/energy-day-month-totals (28/1)`,
`feat/health-watchdog (47/2)`, `feat/link-fallback (246/193)`, `fix/board-guard-grace (3/7)`,
`fix/integration (5/8)`, `fix/link-realign (5/1)`, `fix/memory-repair (3/46)`,
`fix/net-watchdog (5/2)`, `fix/receiver-socket-headroom (5/36)`, `fix/sender-diag-stack (3/50)`,
`feat/sender-ota-autorollback (3/3)`, `feat/sender-sleep-state-announce (3/1)`,
`fix/upload-offload-storage-ready (44/2)`.

### Weit hinten (behind=246, oft viele ahead) — vermutlich alte/archivierte/PR-Branches
(Dutzende: `cline/*`, `docs/*`, `feat/ha-*`, `feat/lowfreq-*`, `fix/pwm-*`, `recovered/*`, …).
Ahead-Zahlen bis ~390 sind überwiegend **veraltete Kopien**, deren Inhalt teils schon in main ist.

## Empfehlung / Nächste Schritte (nicht ausgeführt)

1. **`1b4389f`** (isoliert, wertvoll) entscheiden: als Branch halten, oder bewusst verwerfen.
   Aktuell durch burst-lock-Ansatz ersetzt → in diesem Dokument als Archiv gesichert.
2. `8d8c90c` / `5e6b310` sind reine tote Kopien (Inhalt bereits gemergt) → Kandidaten zum
   Verwerfen, falls gewünscht.
3. Dutzende `behind=246`-Branches: prüfen, welche je gemergt (löschbar) vs. eigene Arbeit sind.
