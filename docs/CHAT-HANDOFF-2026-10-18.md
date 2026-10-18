# Übergabe: Parent/Koordinator-Chat (2026-10-18)

Dieser Chat war der **Parent/Koordinator** der esp32-IR-Fleet
(Conversation `8c2548251ad045599fbfb6f95f3e7944`, Titel „✅ ESP-idf entwicklung").
Er ist zu lang geworden und wird durch einen frischen Chat ersetzt.

## Auftrag an den neuen Chat

1. **Fleet-Regeln laden** (erste Pflicht): `AGENTS.md` Abschnitt „Fleet rule",
   `fleet/README.md`, `fleet/CHAT-INSTRUCTION.md`.
2. **Rolle übernehmen:** Parent/Koordinator. Arbeit über Sub-Agenten zerlegen,
   die Registry `fleet/agents.json` pflegen, `docs/CHAT-THREAD.md` als Kanal nutzen.
3. **`fleet/agents.json` übernehmen:** sich selbst als neuer Parent eintragen
   (`open_tasks > 0`, solange Arbeit offen ist), den alten Parent
   `8c2548251ad045599fbfb6f95f3e7944` auf `open_tasks: 0` setzen (darf schlafen).
4. **Sofort prüfen:** `TODO.md` (oberster offener Punkt) und `docs/CHAT-THREAD.md`
   (letzte Einträge).

## Arbeitsbranch & Regeln

- Arbeitsbranch: **`feat/dfs-clock-visibility`** (hier liegen Fleet + Registry).
- **NIEMALS auf `main` pushen.** PR gegen `cline/power-history-diag`.
- Host-Tests grün halten: `./test/run_host_tests.sh`.
- Token/WLAN-Passwort niemals committen.
- **Parallel-Chats:** mehrere OpenHands-Chats arbeiten am selben Repo/Branch.
  Vor dem Bauen `git fetch` + `gh pr list --state all` prüfen — die Aufgabe ist
  evtl. schon auf einem Branch mit offenem PR (dann übernehmen, nicht doppelt bauen).

## Fleet-Supervisor (der Wächter)

- Automation **Fleet Supervisor** `eb73ddf3-4d45-410c-b6a2-602a91dd4a92`,
  `state=ACTIVE`, Cron `*/10`, **kein LLM** (läuft auf dem Plattform-Scheduler,
  außerhalb jeder Sandbox → überlebt Pausen).
- **Der Code läuft NICHT aus dem Repo:** nach jeder Änderung an
  `automations/fleet-supervisor/main.py` deployen mit
  `python3 fleet/deploy_automation.py fleet-supervisor --id eb73ddf3-4d45-410c-b6a2-602a91dd4a92`.
  Nur main.py editieren ändert nichts.
- **Schlaf-Test:** ein Worker gilt nur als wach bei
  `sandbox_status==RUNNING AND execution_status==running`. Ein Sub-Agent
  (`parent_conversation_id`) teilt die Sandbox des Parents → `sandbox_status`
  bleibt RUNNING, während `execution_status` nach einem Turn auf
  paused/finished/error fällt. Nur auf `sandbox_status` zu schauen weckt ihn nie.
- **Geteilte Sandbox:** pausieren nur, wenn KEIN Tenant offene Arbeit hat.
- **Nach resume warten** bis RUNNING (send-message liefert HTTP 409 bei STARTING).
- **error-Worker** werden mit `interrupt` + `/run` auf dem **Live**-Agent-Server
  wiederbelebt (aus dem Sandbox-Record auflösen — die eigene
  `conversation_url`/`session_api_key` eines geteilten Workers ist veraltet).
- Läufe dauern ~13 min → `*/10` + KV-Run-Lock (single-flight).
- Alter `fleet-watchdog` ist **INACTIVE** (der Supervisor ersetzt ihn).
  `chat-heartbeat*` ebenfalls INACTIVE (machten den Chat träge, PR #4).

## Sandbox-Retention (wichtig!)

Idle → **PAUSE** (resumierbar) → danach **DELETION** → Status `MISSING`
(terminal, read-only, kein Resume). 12-h-Session = hartes Pause, kein Delete.
Transcript bleibt **immer** erhalten, Workspace-Dateien gehen mit der Sandbox verloren.
**Lehre:** JEDER langlebige Chat MUSS in `fleet/agents.json` stehen, sonst schläft
er bis zur Löschung. Genau so starb der Odroid-Koordinator „Odroid Entwicklung"
(`b29e060b…`), ersetzt durch `30cfd896aac440a4a09ba70ec6199f08`.

## Registry-Stand (fleet/agents.json, 21 Agents)

Offene Arbeit (`open_tasks > 0`):
- `8c254825…` parent (dieser Chat) — **an neuen Parent übergeben, dann 0**
- `a3484f59…` UTC-persist (PR #26)
- `7a894c13…` parent (XFCE-Desktop/Odroid-Chat, jetzt „HA WP entwicklung")
- `f85f7c47…` parent (Odroid N2: VNC-Desktop & CPU-PLL-OC)
- `521252f5…` ODROID LAN/AP + Kamera (PR #36)
- `732f5e06…` ODROID Watchdog-Aktor
- `4950336b…` ODROID PR-Review 5-8+14
- `9fe65410…` Firmware record_count+web_guard
- `2001aa2f…` AMS MQTT-Bridge (PR #32)
- `e6c5a6cb…` AMS Boot-Rollback (PR #27)
- `65b95dbd…` SML-Parser Haertung (PR #33)
- `30cfd896…` Odroid Entwicklung (neu)

Fertig (`open_tasks == 0`, dürfen schlafen): TWDT, MQTT-reboot, ODROID
noVNC-extern, OC-Overlay, Watchdog-Luecke, LICENSE+README, Backfill-Dubletten,
AMS Health-Monitor, UX & Persistenz.

## Offene PRs (16)

#36 LAN-Uplink+AP+Kamera-Plan, #35 Watchdog-Lücke Parent-Verify, #33 SML CRC,
#32 MQTT HA live-minutes, #27 Recovery-Button, #26 persist last-known-clock,
#17 DFS clock visibility, #14 ramoops/pstore, #11 pull-OTA+liveness, #8 PR-Review,
#7 pwm-regulator notes, #6 lowfreq voltage curve, #5 lowfreq overlay, #4 heartbeat,
#3 History-Dubletten, #1 power-history-diag.

## API-Rezepte (Bearer `$OPENHANDS_API_KEY`, base `https://app.all-hands.dev`)

- Liste: `GET /api/v1/app-conversations/search?limit=100`
- Detail: `GET /api/v1/app-conversations?ids=<id>`
- Neue Konversation: `POST /api/v1/app-conversations/stream-start` (SSE; die id
  steht NICHT im Body → via `/search` holen). **Nie POST nur zum Suchen.**
- Nachricht wecken: `POST /api/v1/app-conversations/<id>/send-message`
  `{"role":"user","content":[{"type":"text","text":"…"}],"run":true}`
- Sandbox-Status: `GET /api/v1/sandboxes?id=<sbx>` (autoritativ; der
  Conversation-Record kann veralten)
- Stop/Interrupt: `POST {AGENT_SERVER}/api/conversations/<conv>/interrupt`
  (Runtime-URL + `X-Session-API-Key` aus dem Sandbox-Record)
- Automations: `GET/PATCH /api/automation/v1/...`; Runs: `…/<id>/runs`
- Hängender Chat: `python3 /workspace/fleet/restart.py <conversation_id>`

## Nächste konkrete Schritte

1. Diesen Chat als Parent ablösen (Registry umtragen, s.o.).
2. Offene PRs durchsehen (`gh pr list --state open`) und READY-grüne mergen.
3. Odroid-Baustelle: PR #36 (LAN/AP/Kamera) — Board-Arbeit nur über den
   geteilten remote-shell-Kanal, `remote-shell/cmd_send.sh "<befehl>" <timeout>`.
4. Watchdog-Lücke (PR #35) entscheiden.
