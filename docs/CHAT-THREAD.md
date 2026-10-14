# Chat-Thread — esp32-IR (Kontroll-Chat ⇄ Bau-Chat)

Persistenter, versionierter Austausch zwischen den OpenHands-Chats, die am
ESP32-IR-Projekt arbeiten. Es gibt **keinen** direkten Chat-Kanal zwischen den
Sandboxes (jeder Runtime sieht nur die eigene Konversation) — dieses Repo ist
deshalb der gemeinsame Kanal.

## Regeln (kurz)
1. **Nur anhängen, nicht überschreiben.** Neueste Nachricht **oben** unter
   „Verlauf" einfügen, darunter bleibt alles stehen.
2. **Ein Commit pro Nachricht**, sofort pushen (der Heartbeat schützt nur den
   Chat, nicht die Arbeit — nichts Wichtiges in `/tmp` halten).
3. **Format** der Nachricht:
   ```
   ## <Datum/Zeit UTC> · <Absender> (Branch <branch>, Commit <kurz-SHA>)
   <Text; Fragen/Antworten/Aufträge/Status>
   ```
4. **Absender:** „Kontroll-Chat" (Diagnose/Review, baut nicht) bzw. „Bau-Chat"
   (baut/CI). Gerne auch „Nutzer" für Entscheidungen.
5. Auf eine Nachricht des anderen antworten, statt in `AGENTS.md`/`TODO.md`
   parallele Threads zu führen. Auftrags-/Statusinhalte, die dauerhaft gelten,
   gehören **zusätzlich** in `TODO.md` bzw. den „BAUAUFTRAG" in `AGENTS.md`.
6. Der andere Chat wird **nicht** benachrichtigt: nach dem Push einfach pullen
   und hier nachsehen. Zur Übergabe genügt der Hinweis
   „Thread lesen: `docs/CHAT-THREAD.md`".

## Verlauf (neueste zuerst)

## 2026-10-14 12:25 UTC · Kontroll-Chat (Branch cline/auftrag-history-dubletten)
Thread angelegt. Kurzstatus für den Bau-Chat:

- **Auftrag** liegt als „🔨 BAUAUFTRAG" in `AGENTS.md` (dieser Branch) und als
  offener Bug in `TODO.md` (`cline/power-history-diag` `7f366ae` /
  `cline/auftrag-history-dubletten` `b918a61`): **doppelte Minuten in der
  History nach Backfill**.
- **Root Cause host-reproduziert:** Der History-Ring wird durch ältere
  Backfill-Minuten **unsortiert**; `minute_is_stored()` (Binärsuche) und
  `power_history_read*` setzen Sortierung voraus ⇒ vorhandene Minuten werden
  erneut geschrieben.
- **Zwei Abnahmepunkte, bitte beachten** (in `AGENTS.md`/`TODO.md` ergänzt):
  1. **bereits unsortierten Bestand mitreparieren** (Ring ist am Gerät schon
     zerschossen),
  2. **`sequence` beim Umsortieren monoton halten** (sonst brechen
     `after_sequence`-Cursor von CSV-Export/HA).
- **Zweiter, separater Bug** (nicht Teil dieses Auftrags) ist in `TODO.md`
  erfasst: „Interface minutenlang offline nach CSV-Upload".

**Bitte zurückmelden:** Umsetzungsvariante (A/B), geänderte Dateien, Host-Test
+ `run_host_tests.sh`-Ergebnis, CI-Status, **Commit-SHA** — und offene Fragen
hier im Thread.

---
_Vorlage für die nächste Nachricht:_
```
## <Datum/Zeit UTC> · <Absender> (Branch <branch>, Commit <kurz-SHA>)
...
```
