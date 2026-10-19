# Übergabe: ESP-IDF-Chat (2026-10-19) — Ersatz für Conversation `1e748515943a404aa166c052651ca188`

Der alte Chat **„ESP-IDF"** (`1e748515943a404aa166c052651ca188`) ist **archiviert**
(Sandbox-Status `ERROR`, `send-message` → HTTP 410 „Conversation is archived"). Er ist
nicht wiederbelebbar. Dieses Dokument trägt die **zusammengefassten Infos** des alten
Chats in einen frischen Chat.

## Auftrag an den neuen Chat

1. **Lies dieses Dokument vollständig.** Es ist die Zusammenfassung des Vorgängers.
2. **Arbeitsbranch:** `feat/dfs-clock-visibility`. **NIEMALS auf `main` pushen.**
   PRs gegen `cline/power-history-diag`.
3. **Fleet-Regeln laden** (falls Fleet-Arbeit): `AGENTS.md` („Fleet rule"),
   `fleet/README.md`, `fleet/CHAT-INSTRUCTION.md`.
4. **Prüfe** `TODO.md` (oberster offener Punkt) und `docs/CHAT-THREAD.md`.
5. **Host-Tests grün halten:** `./test/run_host_tests.sh`.

## Zusammenfassung des alten Chats (aus dem Condensation-Event)

USER_CONTEXT: German-speaking user debugging **ESP32-IR** (ESP32 firmware) + OpenHands
"chat fleet". Active task: fix sender's SML-parser wedge. User rejected a parser-only
hypothesis, demanded regression analysis, then approved the agreed plan: **(1) put
firmware fixes on a clean branch off `main` + open PR; (2) get...** (truncated). After
the OTA the user said: **"das war ein dummes experiment, es hat den ganzen tag den
zähler blockiert"** (the experiment blocked the meter all day).

TASK_TRACKING:
- **T1 (root cause / sender wedge)**: DONE — real cause was experimental `sender-dfs` CI image flashed to live sender.
- **T2 (channel desync)**: OPEN/NEW — receiver SoftAP follows home WLAN to channel 7 while sender stays on 1; suspected real recurring bug.
- **T3 (plan step 1: clean branch off `main` + PR)**: PENDING.
- **T4 (remove sender-dfs footgun from CI)**: DONE (branch `feat/dfs-clock-visibility`).

COMPLETED:
- **Root cause proven**: experimental CI artifact `caa0140` ("ci: build an experimental
  sender-dfs image (ESP-NOW modem sleep on)") was flashed onto the live sender; ESP-NOW
  modem sleep blocked the meter for a day. SML-parser hypothesis was WRONG.
- **Relay OTA executed**: sender now runs `firmware-maintenance`; link restored —
  `sml_sent_ok` went from 23 in ~3 h to 1/s (seq 115→270 stable over 3 min),
  `parser_resyncs=0`, `gap_mid` rising, heap stable 125524.
- **End-to-end verified**: `/api/dashboard` shows live values — import 83110868→83111206,
  power 742→737 W, avg 736.55 W, `crc_valid` 330→491, `crc_invalid=0`.
- **CI cleanup**: removed `sender-dfs` role + its `case` branch from
  `.github/workflows/esp-idf-build.yml`; committed ("ci: drop the experimental sender-dfs
  image") and pushed to branch `feat/dfs-clock-visibility` (after `git reset` to origin
  tip 2b05c6a, reapplying patch).
- **Documented**: findings written to `2026-10-18.md` and `MEMORY.md` (new section
  "the sender-dfs footgun + OTA recovery"); fixed a heading/structure mistake in MEMORY.md.

PENDING:
- Investigate **channel desync** (receiver ch7 vs sender ch1).
- Plan step 1: firmware fixes on clean branch off `main` + open PR.
- Sender currently unreachable again ("No sender MAC known yet").

CURRENT_STATE:
- Sender: fw `firmware-maintenance`, `wifi_ps_mode=1`, `build_profile=1`,
  `light_sleep_enabled=1` — `wifi_ps_mode=1` explained as expected (comes from default
  `CONFIG_ESPNOW_SENDER_LIGHT_SLEEP=y`, not the dfs image).
- Receiver console: fritzbox connected, **esp-now channel 7**, sntp synced, free heap
  96296, liveness guarded.
- New hot trace: bootlog `wifi:ap channel adjust o:1,1 n:7,1` — receiver SoftAP follows
  home WLAN to ch7; AP started SSID=`ESP32-IR-Meter`, channel=1, IP=192.168.4.1.
- Latest dashboard after receiver reboot: all values null (import/export/power null,
  crc_valid=0), cpu_freq 80 MHz.

CODE_STATE:
- `.github/workflows/esp-idf-build.yml`: `sender-dfs` role removed (was line 90
  `- role: sender-dfs` and `case` at 168/169); YAML validated OK.
- Channel logic in `main.c`: `ESPNOW_CHANNEL = CONFIG_ESPNOW_CHANNEL`;
  `scan_receiver_ap_channel()`; `espnow_align_channel_from_receiver_ap()`;
  `ap_config.ap.channel = ESPNOW_CHANNEL`; `esp_wifi_set_channel(g_espnow_channel, …)`;
  link-fallback logic (`link_fallback_should_fallback`).
- Crash journal present: `crash_journal.c`, `crash_journal_mark_clean_restart()`.
- Memory: `/workspace/project/1e748515943a404aa166c052651ca188/.openhands/memory/{2026-10-18.md,MEMORY.md}`.

VERSION_CONTROL_STATUS:
- Branch: `feat/dfs-clock-visibility`; origin tip 2b05c6a ("docs(fleet): Odroid family
  complete - PR #46/#47 merged, registry to 0 open").
- Cleanup commit "ci: drop the experimental sender-dfs image" pushed to this branch.
- Relevant commits: `caa0140` (experimental dfs image — the culprit), `d7d8cd8` (DFS
  stepping visible), `e350984`/`0778a0e` (sender 80 MHz ceiling / modem sleep fix),
  `cf36ce3`.
- `sender-dfs` job still existed on remote branch before cleanup; now removed.
- Note: `caa0140` is ancestor of `feat/dfs-clock-visibility`, `fix/record-count-web-guard`.

## Trajectory des alten Chats

- 2537 Events, ~85k Tokens/Turn (Kontextfenster 1M → ~8,5 % Auslastung, **nicht** zu lang).
- Tools: terminal (705×), file_editor (86×), invoke_skill (2×), create_pr (1×).
- Vollständiger Download (8,9 MB): `GET /api/v1/app-conversations/1e748515943a404aa166c052651ca188/download`.
