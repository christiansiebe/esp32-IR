# Resilience: transiente LLM-Fehler im `oh`-CLI und im Worker

Datum: 2026-10-19
Branch: `cline/power-history-diag`

## Symptom

Conversations auf dem Odroid-Agent-Server brachen sporadisch ab mit:

```
ConversationErrorEvent code=LLMServiceUnavailableError
litellm.InternalServerError: Litellm_proxyException - Missing credentials.
Please pass an `api_key`, `workload_identity`, `admin_api_key`, or set the
`LITELLM_PROXY_API_KEY` environment variable.
```

Der Fehler ist als `classification={kind: transient, retryable: true,
user_action: retry}` markiert. Das `oh`-CLI hat ihn aber **nicht** als solchen
behandelt: es wartete die volle Poll-Dauer (~240 s) leer und lieferte nichts
zurueck — der Nutzer sah „keine Antwort“.

## Ursachenanalyse (auf dem Odroid geprueft)

1. **Der Schluessel ist ein Snapshot pro Conversation.** Die Conversation
   bekommt beim Anlegen `llm.api_key` aus `~/.openhands/profiles/default.json`
   (`/root/.openhands/settings.json` -> `agent_settings.llm.api_key`).
   Rotiert der Managed-Proxy-Schluessel danach, behaelt die alte Conversation
   den veralteten Wert und scheitert **dauerhaft** — ein erneutes Senden an
   dieselbe Conversation hilft nicht (verifiziert: dieselbe Conversation
   scheiterte wiederholt, eine frische Conversation antwortete sofort).
2. **Der Refresh-Hook ist inaktiv.** `openhands/agent_server/managed_llm_key.py`
   (`register_managed_llm_key_refresh`) registriert nur dann einen
   API-Key-Refresh, wenn `OH_LLM_API_KEY_REFRESH_URL` (und
   `OH_LLM_API_KEY_REFRESH_BASE_URLS`) gesetzt sind. Auf dem Odroid sind diese
   Variablen **nicht** gesetzt -> der Hook laeuft nie.
3. **`events/search` liefert max. 100 Events und standardmaessig die
   AELTESTEN.** Das alte `oh` zaehlte neue Events ueber einen Offset
   (`len(items)`). Sobald eine Conversation >100 Events hatte, war der Offset
   dauerhaft 100 und neue Events wurden nie gesehen -> leerer 240-s-Wait.

## Aenderungen

### `remote-shell/oh.sh`

- **Sofortiges Melden von Fehlern:** `ErrorEvent`/`ConversationErrorEvent`
  beenden die Warteschleife sofort (kein 240-s-Leerwait).
- **Retry mit Backoff:** bei transienten Fehlern wird der Auftrag bis zu
  `OH_RETRIES`-mal wiederholt (`OH_RETRY_BASE` als Basis, verdoppelt sich).
- **Selbstheilung (Fresh-Conversation-Fallback):** bei „Missing
  credentials“/`Litellm_proxyException` (stale Schluessel) wird automatisch
  eine **frische** Conversation angelegt (`OH_FRESH_ON_ERROR=1`, Default).
- **Robuste Event-Erkennung:** `events/search?sort_order=TIMESTAMP_DESC`
  (die jeweils letzten 100 Events) + Abgleich ueber die Event-**ID**-Menge
  statt eines Offsets -> funktioniert auch bei Conversations mit >100 Events.
- **Antwort auch via `finish`-Action:** der Agent kann seine Antwort als
  `ActionEvent` mit `tool_name=finish` (`action.message`) liefern, nicht nur
  als `MessageEvent`.
- **Ehrliche Exit-Codes:** `0` Erfolg, `3` transienter Fehler (retrybar),
  `4` nicht-retrybarer Fehler, `5` kein Ergebnis im Zeitfenster, `2`
  Bedienfehler, `1` Conversation konnte nicht angelegt werden.

Stellschrauben (Env): `OH_RETRIES` (Default 3), `OH_RETRY_BASE` (Default 8 s),
`OH_FRESH_ON_ERROR` (Default 1).

### `remote-shell/worker/worker.py`

- `run_task` behandelt Exit-Code `3`/`5` **und** Textmuster wie
  „missing credentials“, „litellm_proxyexception“, „internalservererror“,
  „rate limit“, „service unavailable“, 502/503/504 als **retrybar**.
- Bei retrybarem Fehler wird der Task bis zu `WORKER_RETRIES`-mal
  (`WORKER_RETRY_BASE`, Default 10 s, Backoff) mit **frischer Conversation**
  wiederholt, statt ihn als `status: error` zu verbuchen.
- Nicht-retrybare Fehler werden unveraendert als Task-Fehler gemeldet
  (kein unnoetiges Warten).

### `remote-shell/worker/worker.py` — verwaiste Tasks nach Neustart

`claim()` verschiebt den Task atomar `queue/` -> `running/`. Ein **laufender**
Task ueberlebte einen Worker-Neustart bisher NICHT: er blieb in `running/`
liegen und wurde nie wieder aufgegriffen (kein Ergebnis).

- `claim()` stempelt beim Beanspruchen `running/<id>.pid` mit
  `{pid, ts}` des Workers.
- `requeue_orphans()` laeuft beim Start in `main()` und schiebt
  `running/*.task` zurueck nach `queue/` — aber **nur**, wenn der gestempelte
  Worker-Prozess nicht mehr lebt (`/proc/<pid>/cmdline` enthaelt kein
  `worker.py`). So wird ein Task nicht doppelt gegriffen, wenn ein anderer
  Worker (gemeinsame Queue) ihn noch ausfuehrt. Alt-Tasks ohne Stempel werden
  ebenfalls zurueckgestellt.
- Beim Abschluss wird der Stempel `running/<id>.pid` entfernt.

## Verifikation (echte Laeufe auf dem Odroid)

1. **Vergiftete Conversation, Selbstheilung:** Session zeigte auf die
   dauerhaft fehlerhafte Conversation `83f60033-...`. `oh` erkannte den
   transienten Fehler, wechselte auf eine frische Conversation und lieferte
   die Antwort `HEAL-OK2` (Exit-Code 0).
2. **Worker-Retry (Unit, Fake-`oh`):** 2x transienter Fehler, dann Erfolg ->
   `run_task` gab `rc=0`, „Erfolg nach 2 Wiederholung(en)“; ein echter Fehler
   (rc=1) wurde **nicht** wiederholt.
3. **E2E ueber die Queue:** echter Job `e2e-...` -> Worker -> `oh` -> Ergebnis
   `status: ok rc: 0`, Ausgabe enthielt die erwartete Terminal-Ausgabe
   („Erfolg nach 1 Wiederholung(en)“).
4. **Neustart-Resilienz (E2E):** Task `restart-...` (Agent mit `sleep 25`)
   wurde von worker1 beansprucht; mitten im Lauf wurden die Worker neu
   gestartet. Log: `23:57:31 [requeue] verwaister Task ... zurueck in die
   Queue` -> `23:57:32 [worker1] Task ... uebernommen` -> `23:58:32 fertig: ok`.
   Ergebnis `status: ok rc: 0`.
5. **Neustart-Resilienz (Unit):** `requeue_orphans` stellte einen Task mit
   toter PID und einen stempellosen Alt-Task zurueck (2), liess einen Task mit
   lebender Worker-PID unangetastet.
6. **Exit-Codes:** `oh` liefert `0` bei Erfolg, `0` bei Wiederverwendung einer
   gesunden Conversation, `3` bei nicht heilbarem transienten Fehler
   (`OH_FRESH_ON_ERROR=0`).

## Betrieb

- Backups: `/usr/local/bin/oh.bak-*`, `/root/worker/worker.py.bak-*`,
  `remote-shell/backups/oh.sh.bak-*`, `remote-shell/backups/worker.py.bak-*`.
- Nach Aenderungen an `oh.sh`/`worker.py`: auf den Odroid kopieren und
  `systemctl restart odroid-worker@1 odroid-worker@2`.

## Offener Punkt (Empfehlung)

Der Managed-Key-Refresh-Hook ist auf dem Odroid weiterhin **inaktiv**. Das
verhindert zwar den Absturz nicht (dafuer sorgt jetzt der Fresh-Conversation-
Fallback), wuerde aber die Ursache beheben. Dazu muessten auf dem Agent-Server
`OH_LLM_API_KEY_REFRESH_URL` und `OH_LLM_API_KEY_REFRESH_BASE_URLS` gesetzt
werden (Deployment-Opt-in, siehe OpenHands #5189). Das ist hier bewusst NICHT
angefasst worden (keine Aenderung an Server-/Netzwerkkonfiguration).
