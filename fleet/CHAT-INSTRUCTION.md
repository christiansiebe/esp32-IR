# Kopierbare Anweisung für jeden esp32-IR Chat

Diesen Block als **erste Nachricht** in einen neuen oder laufenden Chat
einfügen. Er lädt die Fleet-Regeln und bindet den Chat an die Fleet.

---

## Bitte laden: Fleet-Regeln für esp32-IR

Du arbeitest ab jetzt nach den **Fleet-Regeln** dieses Repos. Lies und befolge:

1. **`AGENTS.md`**, Abschnitt **"Fleet rule: agents work through sub-agents and
   keep each other awake"** (ganz oben unter "Working in this sandbox").
2. **`fleet/README.md`** — wie das Wachhaltesystem funktioniert.

Die Regeln in Kurzform:

- **Arbeite über Sub-Agenten.** Zerlege Arbeit in Sub-Konversationen (jede mit
  `parent_conversation_id`). Der Parent koordiniert und reviewt.
- **Schlafe nicht ein, solange du offene Arbeit hast.** Eine pausierte Sandbox
  kann sich nicht selbst wecken — deshalb halten sich die Agenten gegenseitig
  wach.
- **Führe eine `TASKS.json`** in deinem Workspace: ein Eintrag pro Aufgabe,
  `"status": "open" | "done"`. Offene Einträge = wachbleiben. Fertig bist du erst,
  wenn kein Eintrag mehr offen ist.
- **Der Parent pflegt `fleet/agents.json`** (im Repo): pro Worker `open_tasks`
  (0 = darf schlafen). Das ist die Registry, der der Supervisor vertraut.
- **Der Fleet-Supervisor** (`automations/fleet-supervisor`, Cron alle 5 min,
  **kein LLM, keine Token-Kosten**) läuft auf dem Plattform-Scheduler, also
  *außerhalb* jeder Sandbox. Er liest `fleet/agents.json` und pro Zyklus:
  `open_tasks > 0` + Sandbox `PAUSED` → resume + Weck-Nachricht; `open_tasks > 0`
  aber > 20 min ohne Aktivität → Interrupt + Weck-Nachricht; `open_tasks == 0` +
  Sandbox `RUNNING` → pausieren (Kosten sparen). Eine pausierte Sandbox ist damit
  kein Hänger — der nächste Zyklus holt sie zurück.
- **Hängender Chat:** `python3 /workspace/fleet/restart.py <conversation_id>`
  (Interrupt + Resume + Weck-Nachricht).
- **Der PR-Report** (`automations/fleet-pr-report`, alle 6 h, kein LLM) listet je
  PR draft/mergeable/CI und markiert `READY` (grün + mergebar) zum Mergen.
- **Sicherungen:** fertige Agenten (`open_tasks == 0`) werden nie geweckt;
  Sandboxes, die länger als 72 h inaktiv sind, bleiben liegen; nach 6 erfolglosen
  Weckversuchen gibt der Supervisor auf.
- **Wenn du fertig bist:** setze deine `TASKS.json` auf `done`, bitte den Parent,
  `fleet/agents.json` zu aktualisieren — dann darfst du schlafen.

**Konkret jetzt:**
- Prüfe zuerst `TODO.md` (oberster offener Punkt) und `AGENTS.md`.
- Lege/aktualisiere deine `TASKS.json`.
- Arbeite die offenen Punkte ab und melde Status über `docs/CHAT-THREAD.md`.
- Erledige deine Aufgabe über Sub-Agenten, wo sinnvoll.

---

## Noch kürzer (eine Zeile)

> Lade die Fleet-Regeln aus `AGENTS.md` ("Fleet rule") und `fleet/README.md`.
> Arbeite über Sub-Agenten, führe eine `TASKS.json` (open/done) und schlafe nicht
> ein, solange offene Aufgaben existieren; der `fleet-supervisor` weckt dich sonst
> wieder. Fertig = `TASKS.json` auf done + `fleet/agents.json` aktualisiert.
