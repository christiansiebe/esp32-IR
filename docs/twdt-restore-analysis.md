# TWDT-Restore-Analyse (Phase A, Arbeitsdokument)

Branch-basiert auf `fix/c6-ap-autooff-rearm` @ `7163025` (v0.4.0-97-g7163025)
Worktree: `/mnt/ssd/wt-twdt-restore-import` (Branch `fix/twdt-restore-import`)

## Repository/Arbeitsbaum-Ausgangszustand (2026-11-20, Phase A Start)

- Worktree Wurzel: `/mnt/ssd/wt-ap-rearm` (Bestands-Worktree, Branch `fix/c6-ap-autooff-rearm`)
- Base-Commit: `7163025` = `v0.4.0-97-g7163025`
- Git-Remote: `origin https://github.com/christiansiebe/esp32-IR.git`
- Bestehende uncommittete Aenderungen im Bestands-Worktree: `AGENTS.md` (Notizen), `dependencies.lock` — NICHT ueberschreiben.
- Neuer Arbeitszweig: `fix/twdt-restore-import` (frischer Worktree `/mnt/ssd/wt-twdt-restore-import`), identischer Inhalt zu `7163025`.
- Es existiert ferner `origin/feat/twdt-worker-registration` (alter, weit zurueckliegender Zweig; dessen TWDT-Ansatz ist auf der aktuellen Basis durch `task_wdt_guard` abgeloest). Kein Aufbau darauf.

## Aufrufkette (CSV-Restore-Import) — belegt mit Zeilennummern

Basis: Worktree-Snapshot `7163025` (`fix/twdt-restore-import`).

1. HTTP-Handler `power_history_upload_handler()` (main/main.c:5702) — POST /api/power-history-upload
   - `httpd_req_async_handler_begin()` (m:5737) → startet `csv_upload`-Task (prio 3, Stack 4096, m:5744-5749)
   - setzt `g_history_upload_active = true` + `power_history_set_restore_active(true)` (m:5742-5743)
2. `power_history_upload_task()` (m:5690) — nur Parser
   - liest Body in 256-B-Chunks (`httpd_req_recv`, m:5663), ruft `history_upload_feed()` (m:5665)
   - bei voller Batchgrenze `HISTORY_UPLOAD_BATCH` (m:5541) ruft history_upload.c:73-74 `history_upload_flush_batch()`
   - flush ruft `up->flush(...)` = `history_upload_flush_cb()` (m:5588)
3. `history_upload_flush_cb()` (m:5588)
   - kopiert Batch (max 64 records) in `u->xfer`
   - `xQueueSend(g_history_upload_queue, &batch, 30000ms)` (m:5600-5605)
   - wartet bis `u->xfer_done` (bis 30s) — blockiert den csv_upload-Task
4. `history_backfill_worker()` (m:8000, prio 3, NICHT TWDT-subscribed)
   - Loop: drainiert `g_history_upload_queue` (m:8010) → `history_upload_import_batch(batch)` (m:8012)
   - `history_upload_import_batch()` (m:5568): ruft `power_history_import_sorted()` und setzt `*batch->done = true`
5. `power_history_import_sorted()` (power_history.c:1828)
   - `xSemaphoreTake(g_history_mutex, portMAX_DELAY)` (p.c:1844) — **portMAX_DELAY**
   - pro Record: `find_first_at_or_after_time()` + ggf. `shift_up_renumber()` (p.c:1921) + `write_record_idx()` (p.c:1934/1938)
   - Splice-Bound: `POWER_HISTORY_MAX_SPLICE_RECORDS=512` (p.h:38); bei Überschreitung `g_splice_skipped` (p.c:1946)
   - am Ende: `fflush` + `fsync` (p.c:1974), Header-COW (`write_header_copy`, p.c:1978-1985)
   - `xSemaphoreGive(g_history_mutex)` (p.c:1990)

Die Mutex wird also ueber EINEN GANZEN 64er-Batch gehalten (bis zu 512 Splice-Rewrites je Record
moeglich). Der TWDT-subscribed `power_history_worker` (prio 4) nimmt dieselbe Mutex danach mit
`portMAX_DELAY` (p.c:637).

## Mutex g_history_mutex: alle Take/Release-Stellen (power_history.c)

| Zeile | Stelle | Take-Timeout | Haltezeit |
|---|---|---|---|
| 637 | power_history_worker (Live/Backfill-Writer) | **portMAX_DELAY** | ein Batch write |
| 682 | power_history_worker | Give | |
| 1004 | (read API) | 5000 ms | kurz |
| 1018 | (read API) | Give | |
| 1554/1567/1574/1597 | rebuild-Flaeche | 5000 ms | kurz |
| 1620/1625/1630 | rebuild | 10000 ms | kurz |
| 1711/1717/1743 | rebuild/end | Give | |
| 1844 | **power_history_import_sorted** (CSV-Import) | **portMAX_DELAY** | GANZER Batch inkl. Splices |
| 1875/1930/1940/1954/1976/1983/1990 | import_sorted (Gives/Err-Pfade) | | |
| 2053 | try-lock (status) | 100 ms | |
| 2085/2106/2281/2382/2480/2524/2704/2780 | read APIs | 2500 ms | kurz |

Die zwei **portMAX_DELAY**-Stellen (637 und 1844) sind die Konfliktpunkte: Der Import haelt die
Mutex, der subscribed Live-Writer wartet unbegrenzt und kann den TWDT nicht fuettern.

## TWDT-Abonnenten und Feed-Stellen

Abonnenten (nur Receiver-relevant; UART ist Sender-only):
- `power_history_worker` (p.c:620 subscribe; Feeds p.c:627, 636, 683)
- `health_task` (health_task.c:36/38, alle 5 s)
- (Sender-only: uart_capture m:3972, uart_event m:4219 — im C6-Receiver kompiliert)

`task_wdt_guard_init()` (main.c:11291) armiert TWDT mit 120 s Timeout, panic on
(task_wdt_policy.c:10/16). Der liveness-Guard (m:10249) ist ein SEPARATER esp_restart-
Waechter (nicht der TWDT).

**Kritischer Pfad:** power_history_worker fuettert den TWDT nur (a) nach Queue-Timeout,
(b) nach Batch-Collect, (c) nach Mutex+Write (p.c:627/636/683). Zwischen (636) und (683)
liegt `xSemaphoreTake(g_history_mutex, portMAX_DELAY)` — wenn der Import (Hist-Backfill,
nicht subscribed) die Mutex >120 s haelt, verhungert der subscribed Worker im Take und der
TWDT schlaegt zu (Reboot, genau die beobachteten Reset-Reasons "task watchdog"/"panic").

## Live/Backfill-Zugriff waehrend des Imports

- `power_history_enqueue()` (p.c:1763) lehnt ab, solange `g_history_restore_active` (p.c:1777).
  Der Upload-Handler setzt es nur WAEHREND EINES Uploads true (m:5743); zwischen zwei
  Bloecken (8-s-Pause) ist es false → Live/Backfill kann Minuten in `g_history_queue` legen.
- Der `power_history_worker` (subscribed) verarbeitet dann Live-Batches → nimmt die Mutex
  (p.c:637). Wenn gerade ein Import laeuft (Mutex gehalten), wartet er → TWDT-Risiko.
- Splices entstehen genau dann, wenn Import-Minuten (14.-22.11) in einen Ring einsortiert
  werden, der bereits neuere Live-Minuten (24.11) trägt → `shift_up_renumber` schreibt viele
  Records um → verlängert die Mutex-Haltezeit.

## Reboot-Belege (Live, vor Phase A, 2026-11-20)

- `last reset reason : task watchdog` (simulate, nach part_050 code=000; vorher uptime 0)
- `last reset reason : panic / exception` einmal gesehen (Crash-Journal-Pfad)
- Store-Ruecksprung nach Reboot: record_count 2968 → 0/517 → 3149, range bis 5377507428 (Müll)

## Prioritäten der konkurrierenden Tasks

- power_history_worker: prio 4 (p.c:1512) — subscribed
- hist_backfill_worker: prio 3 (m:11431) — NICHT subscribed, Import laeuft hier
- csv_upload: prio 3 (m:5745) — NICHT subscribed
- health: HEALTH_TASK_PRIORITY — subscribed, pollt alle 5 s

Damit ist der Power-History-Writer der einzige subscribed Task, der jemals laenger auf die
Mutex warten muss; genau dort greift der Fehler ein.

## Fix-Entscheidung (Variante C + progressive Feeds; D implizit)

**Kern der Aenderung (2 Bausteine):**

1. **`history_backfill_worker` (der CSV-Import) wird TWDT-subscribed** und feedet den TWDT
   bei Fortschritt (progressive Feeds in `power_history_import_sorted`/`shift_up_renumber`
   und im Worker-Loop). Damit ist der Import ein sichtbarer, arbeitender Task: solange er
   Records schreibt, gilt er als gesund; friert er in einer SPIFFS-Operation ein, feedet er
   nicht mehr und der TWDT feuert ihn (tatsaechlich blockierter Worker wird erkannt).

2. **`power_history_worker` nimmt die Mutex mit begrenztem Timeout** statt `portMAX_DELAY`
   (`POWER_HISTORY_WRITE_LOCK_TIMEOUT_MS`). Beim Timeout werden die bereits gesammelten
   Live-Records **nicht verworfen**, sondern in korrekter Reihenfolge an die Queue-Front
   zurueckgelegt, der TWDT wird gefuettert und die Iteration wiederholt sich. Der Live-
   Writer blockiert dadurch nicht mehr unbegrenzt hinter einem laufenden Import.

Gruende fuer diese Wahl (gegenueber den Alternativen des Auftrags):
- **Variante A (Daten ausserhalb des Locks vorbereiten)** allein reicht nicht: Die
  Einfuegeposition (zeitlich sortiert) und die Splice-Einordnung haengen von konsistenten
  Ring-Lesezugriffen ab; ein Lock-freies Vorbereiten wuerde die COW-Atomaritaet des Stores
  gefaehrden und ist unverhaeltnismaessig invasiv.
- **Variante B (nur `restore_active`-Entkopplung)** ist bereits teilweise implementiert
  (`g_history_restore_active`, p.c:1777) und adressiert Live-Einschub zwischen den Bloecken.
  Sie loest aber die TWDT-Verhungerung nicht: Solange ein Import laeuft und die Mutex haelt,
  wartet der subscribed Live-Writer weiter vergebens. B wird beibehalten, ist aber nicht die
  alleinige Loesung.
- **Variante C (Mutex-Warte + TWDT-Abo umbauen)** ist die Hauptloesung, umgesetzt als
  *begrenzter* Mutex-Wait ohne Datenverlust + *Fortschritts-Feeds* des Import-Tasks. Es ist
  KEIN blindes Feeding in einer unbegrenzten Warteschleife: der Worker feedet nur nach einem
  abgelaufenen, begrenzten Timeout (einmal pro Zyklus) und der Import feedet nur, wenn er
  wirklich Records schreibt.
- **Variante D (Import in atomare Teiloperationen)** wird implizit unterstuetzt: Der Import
  bleibt pro Batch atomar (COW-Header am Ende, unveraendert), aber der Live-Writer kollidiert
  nicht mehr damit; eine echte Aufsplittung der Import-Semantik ist unnoetig, wenn der
  Konflikt (endless Mutex-Wait des Workers) beseitigt ist.

**Kein** pauschales Entfernen des Watchdogs, **kein** blinder Feed in unbegrenzter
Warteschleife, **keine** reine Timeout-Verlängerung, **kein** Live-Datenverlust.

## Geplante Code-Aenderungen

- `task_wdt_policy.h/.c`: neuer Worker `TASK_WDT_WORKER_BACKFILL` ("hist_backfill"),
  Receiver-only, `max_block_ms = 60000` (konservativ: Import eines 64er-Batches liegt
  darunter; progressive Feeds halten ihn gesund), `required_timeout` bleibt 120 s.
- `power_history.h`: neue Konstanten `POWER_HISTORY_WRITE_LOCK_TIMEOUT_MS = 30000` und
  `POWER_HISTORY_IMPORT_FEED_EVERY = 8`.
- `power_history.c`:
  - `power_history_worker`: Mutex-Take mit Timeout, Ruecklegen bei Timeout + Feed.
  - `power_history_import_sorted`/`shift_up_renumber`: progressive `task_wdt_guard_feed()`.
- `main.c` `history_backfill_worker`: `task_wdt_guard_subscribe(TASK_WDT_NAME_BACKFILL)` +
  Feed im Loop.
- Tests: `test_task_wdt_policy.c` erweitern (Backfill-Worker in Receiver-Role, max_block,
  required_timeout deckt 120 s); `test_history_csv_import_store.c`/host-Suite fuer
  WRITE_LOCK/Feed-Konstanten; CI greift die Strings ab („subscribed to the task watchdog").