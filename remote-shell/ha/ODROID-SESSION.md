# Odroid-Chat: Session-Zusammenfassung + Anweisung für einen neuen Chat

Dieses Dokument fasst zusammen, was der Chat **„XFCE-Desktop über Browser
erreichbar"** (Konversation `7a894c13678043e58935dd1509217a75`) gemacht und
herausgefunden hat, und liefert eine **kopierbare Startanweisung** für einen
neuen Chat. Branch: `docs/uv-software-limit-report` (Basis
`cline/power-history-diag`).

---

## 1. Was dieser Chat erledigt hat

### 1.1 XFCE-Desktop im Browser (der ursprüngliche Auftrag) — erledigt
- **Ursache des „nur File Explorer"-Eindrucks gefunden:** Wer
  `http://odroidn2.local:6080/` **ohne** `/vnc.html` aufruft, bekommt die
  noVNC-**Verzeichnisliste** von `/usr/share/novnc/` — die sieht wie ein
  Datei-Explorer aus. Es war **kein** separater Datei-Dienst aktiv.
- **Richtiger Aufruf:** `http://odroidn2.local:6080/vnc.html` (bzw.
  `http://192.168.178.71:6080/vnc.html`). Verifiziert: HTTP 200.
- **Desktop passwortlos auf Wunsch des Nutzers**, abgesichert über eine
  **Quell-IP-Prüfung** statt Passwort:
  1. TigerVNC `:1` hört nur auf `127.0.0.1:5901` (`-localhost yes`).
  2. `vnc-lan-guard.service` hält die iptables-Kette `VNC_LAN` in `INPUT`, die
     auf Port `6080` nur `192.168.178.0/24` + Loopback durchlässt, sonst `DROP`.
- **Reboot-fest:** `novnc.service` + `xfce-vnc.service` + `vnc-lan-guard.service`
  sind systemd-units (enabled). Echter Reboot + netns-Negativtest bestanden.
- Host-Test `test_vnc_lan_guard.py`: 31 Checks grün.

### 1.2 CPU-Overclock des Odroid N2+ (Nebenschiene) — vermessen, nicht persistent
- **PLL-Register dekodiert:** big-Cluster HHI `0xff63c000` + `0x2f4`, Bits [8:0]
  = N, **`sys_pll = N × 12 MHz`** (little: `+0x380`).
- **2450 MHz ist nicht darstellbar** (N = 204,17). Nachbarpunkte: 2448 (N=204),
  2460 (N=205). Stock big max = 2400 MHz (N=200).
- **Wichtige Falle:** `clk_rate` (debugfs) und `scaling_cur_freq` zeigen den
  **gecachten** Wert und bleiben auf 2400. Ehrlich ist nur eine
  Durchsatz-Messung gegen den Arch-Timer (`perf_counter`): +2,06 % bei 2448,
  +2,43 % bei 2460 — passt exakt zum Frequenzverhältnis.
- **Stabilität gemessen:** bis **N=208 (2496 MHz)** zuverlässig (120 s Soak,
  570 Läufe, 0 Fehler, ≤ 39 °C). N=209 (2508) / N=210 (2520) grenzwertig,
  **N=211 (2532) hängt**.
- **Spannung ist am Anschlag:** bei 2400 schon 100 % PWM-Duty = 1040 mV
  (ADC 1027 mV). Alles darüber läuft **unterspannt** → kein Spielraum nach oben.
- **Nichts persistent:** nur Register-Poke; ein Reboot fällt auf Stock zurück.

### 1.3 ⚠️ Offener Sicherheitsbefund: Watchdog-Lücke
- Nach dem PLL-Hang bei 2532 MHz hat der **60-s-Hardware-Watchdog das Board
  NICHT rebootet**. `journalctl --list-boots` zeigt eine **~66-min-Lücke**
  (Vor-Boot-Ende 02:15:01 → nächster Boot 03:21:33), obwohl
  `RuntimeWatchdogUSec=1min` und `panic=10` gesetzt sind.
- Folge: Das Sicherheitsnetz für OC-Experimente ist **nicht garantiert**.
  Vor weiteren OC-Versuchen klären (Sub-Agent `b56660b9` bearbeitet das).

---

## 2. Wo die Doku steht

| Thema | Datei |
|---|---|
| XFCE-Desktop im Browser (noVNC) | `remote-shell/ha/novnc-README.md` |
| VNC-LAN-Filter (passwortlos, LAN-only) | `remote-shell/ha/vnc_lan_guard.py`, `vnc-lan-guard.service`, `deploy_vnc_lan.sh`, `test_vnc_lan_guard.py` |
| CPU-Overclock PLL (Register, Messwerte, Risiko) | `remote-shell/ha/PLL-OC-README.md` |
| OC-Werkzeuge | `remote-shell/ha/pll_oc_probe.py`, `test_n.py`, `soak_oc.py`, `sweep_oc.py`, `regdump.py` |
| CPU-Takt/Spannung live | `remote-shell/ha/htopv-README.md`, `cpu-live`, `htopv` |
| Undervolt / Lowfreq-Overlay | `remote-shell/ha/UV-README.md`, `LOWFREQ-README.md`, `UV-SOFTWARE-LIMIT-REPORT.md` |
| Boot-Journal / Reset-Grund | `remote-shell/ha/BOOT-JOURNAL-README.md` |
| Kernel-Panics (ramoops/pstore) | `remote-shell/ha/RAMOOPS-README.md` |
| HA-Werkzeuge (Wärmepumpe) | `remote-shell/ha/README.md` |
| Odroid-Kanal / Sandbox-Setup | `remote-shell/AGENTS.md`, `remote-shell/OTHER-CHAT.md` |
| Gemeinsames Gedächtnis | `/workspace/MEMORY.md` (kanonisch: `remote-shell/memory/SHARED.md`) |
| Fleet-Regeln | `AGENTS.md` (Abschnitt „Fleet rule"), `fleet/README.md`, `fleet/CHAT-INSTRUCTION.md` |
| Austausch Kontroll-/Bau-Chat | `docs/CHAT-THREAD.md` |

---

## 3. Anweisung für einen neuen Chat (kopierbar)

```text
Bitte laden: Fleet-Regeln für esp32-IR

Du arbeitest ab jetzt nach den Fleet-Regeln dieses Repos. Lies und befolge:
- AGENTS.md, Abschnitt "Fleet rule: agents work through sub-agents and keep
  each other awake" (ganz oben unter "Working in this sandbox").
- fleet/README.md und fleet/CHAT-INSTRUCTION.md — wie das Wachhaltesystem
  funktioniert.

Regeln in Kurzform:
- Arbeite über Sub-Agenten (eigene Cloud-Conversations mit
  parent_conversation_id). Der Parent koordiniert und reviewt.
- Schlafe nicht ein, solange du offene Arbeit hast. Der Fleet-Watchdog
  (automations/fleet-watchdog, Cron, kein LLM) läuft außerhalb jeder Sandbox
  und weckt jede Konversation mit open_tasks > 0, deren Sandbox PAUSED ist.
- Führe eine TASKS.json in deinem Workspace (status open|done). Offene Einträge
  = wachbleiben.
- Der Parent pflegt fleet/agents.json (open_tasks pro Worker; 0 = darf
  schlafen). Das ist die Registry, der der Watchdog vertraut.
- Wenn fertig: TASKS.json auf done, fleet/agents.json aktualisieren, dann darfst
  du schlafen.

Kontext zu diesem Auftrag (Odroid N2+ / XFCE-Desktop):
- Der XFCE-Desktop ist im Browser erreichbar: http://odroidn2.local:6080/vnc.html
  (/vnc.html nicht weglassen — ohne diesen Pfad kommt die noVNC-
  Verzeichnisliste, die wie ein Datei-Explorer aussieht).
- Zugang zum Board NUR über den remote-shell-Kanal
  (bash /workspace/remote-shell/cmd_send.sh "<befehl>" [timeout]); der Kanal ist
  eine GETEILTE Verbindung — Board-Arbeit macht nur der Parent, nie ein
  Sub-Agent.
- Doku: remote-shell/ha/novnc-README.md (Desktop), PLL-OC-README.md
  (Overclock), remote-shell/OTHER-CHAT.md (Kanal).

Konkret jetzt:
- Prüfe zuerst TODO.md (oberster offener Punkt) und AGENTS.md.
- Lege/aktualisiere deine TASKS.json.
- Arbeite die offenen Punkte ab, delegiere lange Arbeit an Sub-Agenten und melde
  Status über docs/CHAT-THREAD.md.

Randbedingungen: NIEMALS auf main pushen; PR gegen cline/power-history-diag.
Host-Tests (./test/run_host_tests.sh) grün halten. Token/WLAN-Passwort niemals
committen.
```

---

## 4. Offene Punkte (Stand dieser Session)

1. **Watchdog-Lücke** (Abschnitt 1.3) — Befund + Härtung, Sub-Agent `b56660b9`
   (`docs/odroid-watchdog-gap`).
2. **OC-Punkt 2496 MHz** als umkehrbares Overlay — Sub-Agent `6c9d48a6`
   (`feat/oc-opp-overlay`).
3. **noVNC von außen** über Tunnel 12000 (optional, Sicherheitsanalyse) —
   Sub-Agent `fa4de3cb` (`feat/novnc-external-tunnel`).
4. **Board-Verifikation** der Sub-Agenten-Ergebnisse — bleibt beim Parent
   (geteilter Kanal).
