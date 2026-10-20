# Merge-Empfehlung fuer die offenen Odroid/OC-PRs

Basis-Branch: `cline/power-history-diag`
Basis-Stand dieser Pruefung: `5da5a9c` (HEAD von `origin/cline/power-history-diag`)
Erstellt: 2026-10-20, **reine Host-/Repo-Arbeit**.

> **Geltungsbereich.** Es wurde **kein Board angefasst** und **kein
> remote-shell-Kanal** benutzt (kein `cmd_send.sh`, kein `/dev/mem`, kein
> `systemctl` am Odroid). Geprueft wurden ausschliesslich
> `gh pr view` (mergeable/CI), `git diff origin/cline/power-history-diag...origin/<head>`,
> die mitgelieferten Host-Tests (in isolierten `git archive`-Baeumen) sowie die
> Merge-Vertraeglichkeit (`git merge-tree`). **Nichts wurde gemergt, kein PR
> geaendert.** Teil B (Rebase von #14/#6) ist unten dokumentiert.

## Kurzuebersicht

| PR | Head-Branch | Mergeable | CI | Host-Checks (nachgestellt) | Empfehlung |
|----|-------------|-----------|----|----------------------------|------------|
| #45 | `docs/ramoops-boot-journal` | MERGEABLE/CLEAN | 6/6 gruen | 81 Checks, 0 Fehler | **MERGE** |
| #44 | `feat/lowfreq-voltage-curve-rebased` | MERGEABLE/CLEAN | 6/6 gruen | 70 Checks, 0 Fehler | **MERGE** |
| #43 | `fix/lowfreq-safe-overlay-guards` | MERGEABLE/CLEAN | 6/6 gruen | ALLE TESTS OK | **MERGE** |
| #42 | `feat/odroid-ap-deploy` | MERGEABLE/CLEAN | 6/6 gruen | 48 Checks, 0 Fehler | **MERGE** |
| #41 | `feat/watchdog-actor-wiring` | MERGEABLE/CLEAN | 6/6 gruen | 17 Checks, 0 Fehler | **MERGE** |
| #40 | `fix/pwm-regulator-notes-accuracy` | MERGEABLE/CLEAN | keine CI (nur `remote-shell/**`) | 161 Checks, 0 Fehler | **MERGE** |
| #39 | `feat/oc-overlay-2472` | MERGEABLE/CLEAN | keine CI (nur `remote-shell/**`) | 24 Checks, 0 Fehler | **MERGE** |
| #7  | `docs/pwm-regulator-notes` | MERGEABLE/CLEAN | keine CI (nur `remote-shell/**`) | 157 Checks, 0 Fehler | **CLOSE** (von #40 ersetzt) |
| #5  | `feat/lowfreq-test-overlay-safe-restore` | MERGEABLE/CLEAN | keine CI (nur `remote-shell/**`) | ALLE TESTS OK | **CLOSE** (von #43 ersetzt) |
| #14 | `docs/uv-software-limit-report` | **CONFLICTING**/DIRTY | 3/3 gruen | Tests gruen | **CLOSE** (Kern in #45; Rest aufteilen) |
| #6  | `feat/lowfreq-voltage-curve` | **CONFLICTING**/DIRTY | keine CI | 44 Checks, 0 Fehler | **CLOSE** (von #44 ersetzt; Rebase gelang, s. Teil B) |

Alle Host-Checks wurden aus dem jeweiligen PR-Baum selbst ausgefuehrt
(`git archive origin/<head> | tar -x`), nicht aus dem Arbeitsbaum — so ist
ausgeschlossen, dass ein Ergebnis von einer anderen Branch stammt.

**Reihenfolge-Hinweis (nur Information, kein Merge hier).** #43, #44 und #45
haengen jeweils einen Block an **dasselbe** Datei-Ende von `test/run_host_tests.sh`.
Gegeneinander sind sie daher textlich konfliktbehaftet (`git merge-tree` rc=1),
obwohl jeder **einzeln** gegen den Basis-Branch CLEAN ist. Wer sie nacheinander
landet, muss den angehaengten Block beim jeweils naechsten Merge aufloesen
(trivial: nur die neuen `python3`/`sh`-Zeilen zusammenfuehren). #41 und #42
fuegen ihren Block weiter oben ein und sind mit den anderen kollisionsfrei.

---

## Teil A — Empfehlung je PR

### #45 — feat(odroid): ramoops/pstore + boot journal — **MERGE**
- **Zweck erfuellt.** Splittet den wertvollen Kern aus #14 heraus: ein
  ramoops-Overlay reserviert 1 MiB bei `0x50000000`, das Panic-Dmesg ueberlebt den
  Warm-Reset, `boot_journal_lib.classify_reset()` klassifiziert den Reset inkl.
  Panic-Kurztext. 18 neue Dateien, nichts Bestehendes geloescht.
- **Sicherheit (Boot-Loop).** Der einzige kritische Punkt ist die RAM-Adresse.
  `test_gen_ramoops_dts.py` prueft explizit, dass `0x50000000` im freien Fenster
  `0x40000000..0xdfd2dfff` liegt und **keine** bekannte Reservierung trifft
  (secmon/BL31 `0x05000000`, Kernel-Code/-Data, CMA `0xdfd2e000+`,
  U-Boot `scriptaddr`, `fdt_addr_r`) und dass Adresse 64K-aligniert ist. Ein
  falsches `reg` waere der klassische Boot-Loop-Ausloeser — genau dieser Test
  deckt ihn ab. Das Overlay wird zudem ueber `boot.cmd` nur bei erfolgreichem
  `fdt apply` aktiv; sonst bleibt der Original-DT.
- **Ueberholt?** Nein — es **ist** der Ersatz fuer #14.
- **Fehler/Risiko.** Keine gefunden. `sh -n` fuer die drei Skripte ok, alle vier
  Host-Tests gruen, in `test/run_host_tests.sh` registriert.
- **Fazit:** inhaltsvoll, getestet, kollisionsfrei gegen die Basis. **MERGE.**

### #44 — fix(odroid): lowfreq voltage curve rebased — **MERGE**
- **Zweck erfuellt.** Ersetzt #6: pro Rate eine eigene Spannung fuer die
  Tief-Takte, mit Schutzgrenzen (Regler-Boden 680 mV hart, <695 mV Warnung,
  unbekannte Rate/&gt;14 Raten Fehler, `--mv`+`--table` zusammen Fehler).
- **Wichtigster Punkt.** #6 hatte `render()` entfernt; `gen_oc_dts.py` (Basis)
  ruft aber `lf.render(...)` in `render_full()` auf — ohne `render()` waere
  `gen_oc_dts.py --with-lowfreq` gebrochen. #44 behaelt `render()` bei und
  ergaenzt einen Regressionstest, der `gen_oc_dts.py --rate-mhz 2472 --with-lowfreq`
  wirklich startet. Ich habe das unabhaengig nachvollzogen:
  `gen_oc_dts.py --rate-mhz 2472 --with-lowfreq` rendert „big 2472 MHz (N=206)
  @ 1040 mV + 28 Tief-Takte“, und der Default-Lauf von `gen_lowfreq_dts.py` ist
  **byte-identisch** zu `lowfreq.dts`.
- **Sicherheit.** Die Schutzgrenzen sind unveraendert streng; die Erweiterung
  erzeugt nur zusaetzliche OPP-Eintraege, der Boot-Boden (1.0 GHz,
  `cpu-freq-guard`) bleibt Sache von #43/#5. Kein Boot-Loop-Risiko gegenueber
  dem Basisstand.
- **Ueberholt?** Nein. Ueberschneidet sich **nicht** mit #39 (getrennte Dateien;
  #44 laesst den OC-Standardpunkt bewusst bei 2496).
- **Fazit:** behebt den blockierenden #6-Konflikt und haelt den OC-Pfad intakt.
  **MERGE.**

### #43 — fix(odroid): safe lowfreq overlay — **MERGE** (ersetzt #5)
- **Zweck erfuellt.** Behebt die drei #5-Befunde aus `PR-REVIEW-ODROID.md`:
  1. **Additivitaet:** `set_overlay_lowfreq add|remove` fasst nur den Token
     `lowfreq` an, fremde Overlay-Namen bleiben stehen. #5 schrieb die ganze
     `user_overlays=`-Zeile und haette z. B. ein `ramoops`-Overlay geloescht —
     ein echtes, stilles Risiko (Overlay weg = Funktion weg).
  2. **MIN_SPEED als zweiter Boden:** `MIN_SPEED=1000000` in
     `/etc/default/cpufrequtils`, weil Armbians `armbian-hardware-optimization`
     `scaling_min_freq` **nach** `cpu-freq-guard.service` setzt und den 1.0-GHz-Boden
     sonst unterlaufen kann. Das ist genau der Boot-Loop-relevante Fix.
  3. **`--dry-run`/Testmodus** ohne Board-DTB bricht nicht mehr mit `die` ab,
     sondern warnt — der harte Abbruch bleibt nur fuer den echten Board-Lauf.
- **Sicherheit.** Die relevanteste Verbesserung im ganzen Satz: #5 konnte den
  1.0-GHz-Boden unterlaufen lassen (Armbian-Falle), #43 schliesst das.
- **Ueberholt?** Nein — **ist** der Ersatz fuer #5.
- **Fazit:** inhaltlich strenger als #5, Tests gruen (`ALLE TESTS OK`).
  **MERGE.**

### #42 — feat(odroid): AP deploy/revert/dead-man tooling — **MERGE**
- **Zweck erfuellt.** Host-seitiges Ausroll-/Ruecknahme-Werkzeug fuer den
  Odroid-WLAN-AP (hostapd/dnsmasq/NAT/NM), mit `odroid_ap_deadman.sh` +
  Timer (Auto-Revert nach N Minuten ohne `--confirm`).
- **Sicherheit.** **Dry-Run ist Default** (`--apply` noetig), Dienste bleiben
  `masked`, bis ein echter Ausroll entschieden ist; `AP_ROOT` erlaubt das
  Testen ohne Board/root. Der Dead-Man ist die geforderte Absicherung
  (Abschnitt 7.2). Keine Boot-Loop-Naehe (kein DT/CPU-Eingriff). Die
  NAT-/Dienst-Aenderungen sind idempotent (`-C`/`-A`, „unmask/enable“).
- **Fehler/Risiko.** Keine gefunden; 48 Host-Checks gruen, `sh -n` ok.
  Hinweis: `test_ap_deploy.py` steht im PR **nicht** an einem Ort, der mit #41
  kollidiert (Block weiter oben) — beide sind zusammen kollisionsfrei.
- **Fazit:** sauber getrenntes, trockenlauf-faehiges Tooling. **MERGE.**

### #41 — feat(watchdog): monitor -> power-cut wrapper -> actor — **MERGE**
- **Zweck erfuellt.** Verdrahtet Beobachter und Aktor: `watchdog_powercut.sh`
  (Wrapper, baut den `watchdog_actor.py`-Aufruf aus Env, durablem Cooldown),
  `watchdog-monitor.service` (`Restart=always`), `deploy_watchdog_actor.sh`
  und `test_watchdog_integration.py`.
- **Sicherheit (wichtig bei einem Aktor, der Strom trennt).**
  **Trockenlauf ist Default**; geschaltet wird nur mit
  `WATCHDOG_POWERCUT_ARMED=1` bzw. `--armed` — eine bewusste Bediener-Handlung.
  Der Cooldown ist **durabel** (`--state-file`), ueberlebt also den
  Monitor-Neustart und verhindert eine Schalt-Schleife. Der Integrationstest
  faehrt einen simulierten Heartbeat-Ausfall durch die **echte** `decide()`-Logik
  und den Wrapper (Trockenlauf) und prueft: genau **ein** Cut geplant, der
  durabler Cooldown unterdrueckt den zweiten, danach wieder erlaubt. Keine Mocks,
  kein Netz, keine Hardware.
- **Fehler/Risiko.** Keine gefunden; 17 Checks gruen, `sh -n` ok. Die verbleibenden
  Punkte (Aktor-Wahl, Heartbeat-Transport, Fail-safe, Cooldown-Wert) sind
  ausdruecklich **Nutzer-Entscheide** und im Doc getrennt.
- **Fazit:** trockenlauf-faehig, mit durablem Cooldown. **MERGE.**

### #40 — fix(odroid): PWM-regulator notes accuracy — **MERGE** (ersetzt #7)
- **Zweck erfuellt.** Korrigiert die #7-Befunde: vertauschte Registerformel
  (`(hi<<16)|lo`, HIGH in Bits 31..16) jetzt richtig, der Vakuum-„Entkopplungs“-Test
  (verglich einen Wert mit sich selbst) durch einen echten Kontrast ersetzt, der
  unbelegte Cross-Check gegen `uv.py` jetzt belegt (nur Konstanten, kein
  `/dev/mem`), fehlender Abschnitt 6 nachgezogen.
- **Sicherheit.** Reine Doku + Modell + Tests; kein DT, kein CPU-Eingriff.
  Kein Boot-Risiko. Die Tests importieren `uv.py`, aber nutzen nur Konstanten und
  reine Rechenfunktionen — kein Register-/Sysfs-Zugriff.
- **Ueberholt?** Nein — **ist** der Ersatz fuer #7.
- **Fazit:** 161 Checks, 0 Fehler. **MERGE.** (CI zeigt „no checks“, weil der
  Workflow `remote-shell/**` per `paths-ignore` ausschliesst — das ist erwartet,
  kein Fehlschlag.)

### #39 — oc: OC-Punkt 2496 (N=208) -> 2472 (N=206) — **MERGE**
- **Zweck erfuellt.** Setzt den Standard-OC-Punkt auf 2472 MHz (N=206), den am
  laengsten und fehlerfrei vermessenen Punkt (30 min Soak, 22828 Laeufe,
  0 Fehler), statt 2496 (nur 120 s Soak). 2496/2484 bleiben dokumentiert.
- **Sicherheit.** Der Boot-Schutz bleibt unveraendert: `oc-freq-guard.service`
  deckelt `scaling_max_freq` auf den Werkswert 2400, der OC-Punkt ist nur per
  ausdruecklichem Hochsetzen erreichbar. Ein-Boot-Testfenster und Ruecknahme
  unveraendert. Verschiebt nur den Standardpunkt — **kein** Boot-Loop-Risiko
  (OC wird nie automatisch gewaehlt).
- **Ueberschneidung.** Keine Datei-Ueberschneidung mit #44. Wenn #44 zuerst
  landet, bleibt #39 CLEAN (getrennte Dateien). Wenn #39 zuerst landet, bricht
  #44 **nicht** — #44 laesst den OC-Standardpunkt bewusst unberuehrt (Test nutzt
  `--rate-mhz 2472` als Argument, nicht den Default).
- **Fazit:** empirisch besser begruendeter Standardpunkt, Tests gruen. **MERGE.**

### #7 — docs(odroid): research notes on the pwm-regulator — **CLOSE**
- **Grund.** Vollstaendig durch **#40** ersetzt. #7 enthaelt die falsche
  Registerformel (`((cnt - duty_cnt) << 16) | duty_cnt`) und den Vakuum-Test;
  #40 korrigiert beides und ergaenzt den belegten Cross-Check. Ein Merge von #7
  nach #40 wuerde die korrigierte Formel wieder auf die alte setzen.
- **Aktion:** schliessen, mit Verweis auf #40 („superseded by #40“).

### #5 — feat(odroid): one-boot test overlay + stock restore — **CLOSE**
- **Grund.** Vollstaendig durch **#43** ersetzt. #5 schreibt `user_overlays=`
  komplett (loescht fremde Overlay-Namen), setzt **kein** `MIN_SPEED` (der
  1.0-GHz-Boden ist unterlaufbar) und bricht bei `--dry-run` ohne DTB mit `die`
  ab. Alle drei Punkte sind in #43 behoben.
- **Aktion:** schliessen, mit Verweis auf #43 („superseded by #43“).

### #14 — feat(odroid): ramoops/pstore (Sammel-Branch) — **CLOSE**
- **Grund.** 14 Commits, 67 Dateien (+4964/-148) aus vielen Themen; als Ganzes
  nicht mergebar und seit dem Basis-Fortschritt **CONFLICTING** (Konflikt in
  `remote-shell/memory/SHARED.md`). Der wertvolle, board-verifizierte Kern
  (ramoops + Boot-Journal) ist **1:1 in #45** enthalten — ich habe das
  gegengeprueft: 16 der 17 geteilten Dateien sind **byte-identisch** (nur
  `ha/README.md` unterscheidet sich, weil #45 dort den ramoops-Block eintraegt,
  #14 einen breiteren CPU-Block). #45 hat zusaetzlich die vier Host-Tests in
  `run_host_tests.sh` registriert, was #14 **nicht** tut.
- **Nicht enthalten in #45** (und damit nicht verloren, sondern **offen fuer
  eigene PRs**): htopv/cpu-live/cpu-volt, noVNC/XFCE/vnc-lan-guard,
  PLL-OC-Proben (`pll_oc_probe.py`, `soak_oc.py`, `sweep_oc.py`, `margin_oc.py`),
  die Kanal-Backoff-Fixes (`cmd_agent.next_delay`, `register_sandbox.prune_sandboxes`,
  `test_channel_backoff.py`), `UV-SOFTWARE-LIMIT-REPORT.md`, die Verschiebung
  `tools/remote-shell/*` -> `remote-shell/ha/` sowie `MIN_ONLINE=1`.
  Diese Themen brauchen je einen kleinen, thematischen PR.
- **Aktion:** schliessen, mit Verweis auf #45 und die Liste der noch
  ausstehenden Themen. (Alternativ: offen lassen als Merkzettel — aber dann
  nicht mergen.)

### #6 — feat(odroid): per-rate voltage curve — **CLOSE**
- **Grund.** Durch **#44** ersetzt. Der Rebase ist **gelungen** (Teil B), aber
  #6 entfernt `render()` und bricht damit `gen_oc_dts.py --with-lowfreq`; #44
  behaelt `render()` und deckt den Pfad mit einem Regressionstest ab. #6 hat
  ausserdem **keine** Registrierung in `run_host_tests.sh`.
- **Aktion:** schliessen, mit Verweis auf #44 („superseded by #44“).

---

## Teil B — Konflikt-PRs

### #14 `docs/uv-software-limit-report` -> **Rebase verworfen, CLOSE empfohlen**
- **Ist #14 durch #45 abgedeckt?** **Im Kern ja.** Die ramoops/Boot-Journal-Dateien
  sind in #45 **byte-identisch** enthalten (siehe oben, 16/17 Dateien SAME); die
  uebrigen Themen sind in #45 bewusst ausgelassen.
- **Rebase-Versuch.** `git rebase origin/cline/power-history-diag` auf
  `docs/uv-software-limit-report` **scheitert bereits im ersten Konflikt** in
  `remote-shell/memory/SHARED.md` (Auto-Merge von `remote-shell/README.md` ok,
  dann Konflikt). Das ist genau das geteilte-Infrastruktur-Problem, das #45 durch
  Weglassen dieser Dateien vermeidet. Ich habe den Rebase **abgebrochen**
  (`git rebase --abort`) — es gibt keinen neuen Branch und keinen PR dafuer.
- **Ergebnis:** Rebase **nicht** sinnvoll als Ganzes. Empfehlung **CLOSE**;
  die verbleibenden Themen als eigene kleine PRs nachziehen (Liste bei #14).

### #6 `feat/lowfreq-voltage-curve` -> **Rebase gelungen (lokal), aber CLOSE empfohlen**
- **Rebase durchgefuehrt.** `git rebase origin/cline/power-history-diag` auf
  `feat/lowfreq-voltage-curve`. Ein Konflikt in `remote-shell/ha/gen_lowfreq_dts.py`:
  - **Aufloesung:** beide Seiten behalten — `render()` (Basis, fuer `gen_oc_dts.py`)
    **und** die neuen Kurven-Helfer (#6). Im zweiten Konfliktblock (`main()`)
    die kurvenbasierte Fassung uebernommen.
  - **Ergebnis:** `gen_lowfreq_dts.py` parst sauber; Default-Lauf
    **byte-identisch** zu `lowfreq.dts`; `gen_oc_dts.py --with-lowfreq` rendert
    Tief-Takte + OC-Punkt; `test_gen_lowfreq_dts.py` -> **44 Checks, 0 Fehler**;
    `./test/run_host_tests.sh` **gruen** (exit 0).
  - Der Rebase-Branch liegt lokal als `ha/rebase-6` und ist **funktional
    gleichwertig zu #44** (Unterschied: #44 registriert die Tests in
    `run_host_tests.sh` und hat einen zusaetzlichen OC-Regressionstest; die
    `render()`-Funktion steht nur an anderer Stelle in der Datei).
- **Warum trotzdem CLOSE?** #44 ist der bereits aufgeraeumte, getestete und
  registrierte Nachfolger. Zwei PRs mit derselben Funktion sind unnoetig; #44 ist
  die bessere Wahl. **Kein** neuer Branch/PR fuer den Rebase gepusht, weil er
  #44 nur duplizieren wuerde.
- **Ergebnis:** Rebase **gelungen** (dokumentiert), Empfehlung **CLOSE** fuer #6,
  **MERGE** fuer #44.

---

## Randbedingungen (eingehalten)

- Nicht auf `main` gepusht; kein Merge in gemeinsame Branches.
- Kein Board, kein remote-shell-Kanal, kein `/dev/mem`, kein `systemctl` am Odroid.
- `./test/run_host_tests.sh` gruen auf Basis und auf dem Rebase-Branch.
- Reine Host-/Repo-Arbeit; Empfehlungen nur als Dokument, PRs unveraendert.
