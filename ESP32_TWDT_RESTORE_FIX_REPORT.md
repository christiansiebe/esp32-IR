# ESP32 TWDT Restore-Fix — Abschlussbericht (Phase A)

**Datum:** 2026-11-20
**Branch:** `fix/twdt-restore-import`
**Commit-SHA:** `1d67f31fa4ac8eab4c8e09b5222a06e48d1f0a6e`
**CI-Run:** [`45383146522`](https://github.com/christiansiebe/esp32-IR/actions/runs/45383146522) — **success**
**Status:** Phase A abgeschlossen. **Kein** Flash/OTA, **kein** Neustart, Store unveraendert.

---

## 1. Ausgangszustand (gesichert)

- Basis-Worktree `/mnt/ssd/wt-ap-rearm`, Branch `fix/c6-ap-autooff-rearm`, Commit `7163025` (= `v0.4.0-97-g7163025`).
- Neuer Arbeitszweig `fix/twdt-restore-import` (frischer Worktree `/mnt/ssd/wt-twdt-restore-import`), inhaltlich identisch zu `7163025`.
- Bestehende uncommittete Metadaten-Aenderungen im Bestands-Worktree (`AGENTS.md`, `dependencies.lock`) wurden **nicht** ueberschrieben.
- Es existiert ferner ein alter, weit zurueckliegender Zweig `origin/feat/twdt-worker-registration`; dessen TWDT-Ansatz ist auf der aktuellen Basis durch `task_wdt_guard` abgeloest. Kein Aufbau darauf.
- Vereinzelte, identische Vollerfassung der Aufrufkette / Mutex / TWDT in `docs/twdt-restore-analysis.md`.

## 2. Ursache (mit Codebelegen)

Die reproduzierbaren Reboots (`last reset reason: task watchdog`, einmal `panic / exception`) wurden durch einen **verhungernden, TWDT-subscribed Live-Writer waehrend eines langen CSV-Restore-Imports** ausgeloest:

- Der Import `power_history_import_sorted()` (power_history.c:1828) nimmt `g_history_mutex` mit `portMAX_DELAY` (p.c:1844) und haelt sie ueber einen **ganzen 64er-Batch** inkl. Splices (`shift_up_renumber`, bis zu `POWER_HISTORY_MAX_SPLICE_RECORDS=512` Rewrites je Record, provoke keine Lock-Freigabe).
- Der TWDT-subscribed `power_history_worker` (prio 4) nimmt dieselbe Mutex mit `portMAX_DELAY` (p.c:637, jetzt 641) und kann in der Wartezeit **keinen TWDT (nach p.c:627/636/683)**. Ein Import, der die Mutex >120 s haelt (Splices + SPIFFS, bis zu 512 fseek+fwrite je Record), laesst den subscribed Worker verhungern → TWDT-Panic + Reboot.
- Der Import laeuft im `history_backfill_worker` (prio 3, m:11431), der **nicht** TWDT-subscribed war → der lange Import war fuer den TWDT unsichtbar, so dass nur der Live-Writer den Stau "spielte".
- Splices entstehen, wenn waehrend des Restores Live/Backfill-Minuten (24.11.) in einen Ring einsortiert werden, der aeltere Import-Minuten (14.–22.11.) traegt. Zwischen zwei 8-s-Bloecken war `restore_active` false → Live konnte einschleusen.

Belegte Beobachtung (Live, vor Phase A): Store-Ruecksprung nach Reboot (record_count 2968 → 0/517 → 3149), range bis 5377507428 (Tail-Muell), `task watchdog` als Reset-Grund.

## 3. Gewaehlte Architektur (Variante C + progressive Feeds; D implizit)

Zwei Bausteine, beide minimal-invasiv:

1. **`power_history_worker` nummt die Mutex mit begrenztem Timeout**
   `POWER_HISTORY_WRITE_LOCK_TIMEOUT_MS = 30000` statt `portMAX_DELAY`. Bei Timeout
   werden die bereits aus der Queue genommenen Live-Records in korrekter Reihenfolge
   an die Queue-Front zurueckgelegt (kein Minute-Verlust), der TWDT wird gefuettert
   und die Iteration wiederholt sich. Der Writer blockiert nicht mehr unbegrenzt
   hinter einem laufenden Import.

2. **Der `history_backfill_worker` (Import) wird TWDT-subscribed** und feedet bei
   Fortschritt:
   - pro `POWER_HISTORY_IMPORT_FEED_EVERY = 8` importierter Records in
     `power_history_import_sorted()` (p.c:1904),
   - an jedem Splice-Yield in `shift_up_renumber()` (p.c:419),
   - je ESP-NOW-Page-Fetch der Backfill-Session (m:7867),
   - je Arbeiter-Loop (m:8020).
   Damit ist ein langer, gesunder Import als arbeitender Task sichtbar; ein in
   SPIFFS/fsync eingefr.Abortener Import feedet nicht mehr → TWDT feuert ihn korrekt.

Kein blindes Feeding in unbegrenzter Warteschleife, kein pauschales Watchdog-Entfernen,
keine reine Timeout-Verlängerung (TWDT bleibt 120 s), kein Live-Datenverlust, keine
inkonsistenten Zwischen-Zustaende.

## 4. Verworfene Alternativen und Gruende

- **Variante A nur (Daten ausserhalb des Locks vorbereiten):** verworfen — die
  Einfuegeposition (zeitlich sortiert) und die Splice-Einordnung haengen von
  konsistenten Ring-Lesezugriffen ab; ein Lock-freies Vorbereiten gefaehrdet die
  COW-Atomaritaet und ist unverhaeltnismaessig invasive.
- **Variante B nur (`restore_active`-Entkopplung):** teilweise schon vorhanden
  (`power_history_enqueue` lehnt bei `g_history_restore_active` ab, p.c:1777);
  loest das TWDT-Verhungern aber nicht, weil der Live-Writer weiterhin auf die
  gehaltene Mutex wartet. Wird beibehalten, aber nicht als alleinige Loesung.
- **Reine Timeout-Verlängerung** des TWDT: ausdruecklich verboten (Auftrag) und wuerde
  die Erkennung echter Staus verschlechtern.
- **Pauschale Entfernung/Deaktivierung des Watchdogs:** verboten (Auftrag) und
  wuerde einen SPIFFS-Hang wieder "still" machen.

## 5. Geaenderte Dateien

| Datei | Aenderung |
|---|---|
| `main/power_history.h` | neue Konstanten WRITE_LOCK_TIMEOUT_MS / IMPORT_FEED_EVERY |
| `main/power_history.c` | bounded Mutex-Take + Queue-Ruecklage; progressive Feeds im Import/Splice |
| `main/main.c` | Backfill-Worker subscribed + Feeds (Loop, ESP-NOW-Fetch) |
| `main/task_wdt_policy.h` | neuer Worker `BACKFILL` ("hist_backfill") |
| `main/task_wdt_policy.c` | Name/Role/max_block fuer Backfill-Worker |
| `test/host/include/esp_shim.h` | `xQueueSendToFront`-Stub (Host-Determinismus) |
| `test/test_task_wdt_policy.c` | Socket-Worker-Register + subscribed_count 2→3 (Receiver) |
| `test/test_history_csv_import_store.c` | WDT-Fairness-Konstanten- + Splice-Druck-Regressionstests |
| `docs/twdt-restore-analysis.md` | Arbeitsanalyse mit Zeilen-Beleg |

## 6. Commit & CI-Ergebnisse

**Commit:** `1d67f31fa4ac8eab4c8e09b5222a06e48d1f0a6e`
**Workflow:** `ESP-IDF firmware`, Branch `fix/twdt-restore-import`
**Conclusion: success**

| Job | Status | Conclusion |
|---|---|---|
| Host tests (aggregation) | completed | success |
| Build receiver | completed | success |
| Build receiver-apoff | completed | success |
| Build sender | completed | success |
| Build receiver-c6 | completed | success |
| Build receiver-c6-apoff | completed | success |

CI-Run abgeschlossen 2026-11-20T11:22:51Z.

## 7. Regressionstestergebnisse

Erweiterte Host-Suite (laueft in `test/run_host_tests.sh`, Job "Host tests"):
- `test_task_wdt_policy.c`: Backfill-Worker in Receiver-Role + max_block 60 s, deckt 120-s-Timeout; subscribed_count Receiver 3.
- `test_history_csv_import_store.c`:
  - `test_wdt_fairness_constants`: WRITE_LOCK_TIMEOUT > 0 und < 120 s; Feed-Stride > 0.
  - `test_import_splice_pressure`: 100er-Prefill + 64 tiefe Spliced-Minuten → Time-Order intakt, alle Minuten vorhanden.
- Bestehende Import-Tests (extends/dedupe, forward order, idempotent, pre-2024, restore-quiesce, arg guards) weiterhin gruen.

CI bestaetigt: Host-Tests success.

## 8. Verbleibende Risiken

- **CI bildet, testet aber nicht auf echter Hardware:** Das tatsaechliche TWDT-Verhalten
  (Live-Writer-Delay, Splice-Feed) ist auf Ziel-HW nicht direkt geprueft — nur die
  Konstruktion ist abgesichert (Strings via grep in CI; Policy/Store host-getestet).
- **Splice-Depth bleibt O(ring)-Begrenzung:** `POWER_HISTORY_MAX_SPLICE_RECORDS=512` pro
  Minute bleibt; tiefere Minuten werden weiterhin als "too deep" uebersprungen
  (`g_splice_skipped`). Bei extrem voller/unsortierter Ring kann ein Import Minuten
  zuruecklassen (Gap) — beabsichtigtes Schutzverhalten, kein Datenverlust.
- **Sequenzieller Betrieb von Live/Import:** Live-Minuten, die waehrend eines Imports in
  die Queue kommen, werden verzögert (bis Import fertig), aber nicht verworfen.
- **`parsed:64`-Abbruch des Großuploads:** unabhaengig von diesem Fix (separates
  MID-Stream-Limit von 64 Zeilen je Batch). Fuer den 10125-Zeilen-Restore wird weiterhin
  das bewaehrte part_-Block-Verfahren (63 Zeilen < Grenze) verwendet.

## 9. Genauere spaetere Test- und Rollout-Schritte (nach Freigabe)

1. **Freigabe fuer Produktivtest** separat einholen (kein autonomes Update).
2. **CI-Artifact `receiver-c6-sml-<ver>.bin`** aus Run 45383146522 beziehen,
   Pruefsumme dokumentieren.
3. **Empfänger** per gedaempelten OTA (Current C6) auf den neuen Build bringen,
   Rollback-Konfiguration beibehalten.
4. **Verifizieren** auf empfaenger-seitigem Store: `bootlog` sauber, TWDT-Subscriber
   Zaehler 3 (power_history + health + buildfall), Smooth Betrieb.
5. **Restore erneut versuchen** (part_-Blocke, 63 Zeilen): erwartet kein TWDT-Reboot;
   ueberwachen per Watcher (Reset-Grund + records) wie zuvor.
6. **Store-Reparatur** erst NACH Sicherung: vollstaendige Rohkopie + Export
   `/api/power-history.csv`, Pruefsumme, getrennte Identifikation valider Records vs.
   Tail-Muell (Zeitbereich `1794688680..5377507428` = Integritaetsfehler); keine
   automatische Bereinigung.
7. **Abschluss**: End-Verifikation Backup 10125 Zeilen vs. Ring, Bericht.