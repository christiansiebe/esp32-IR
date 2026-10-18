# Review der offenen Odroid-PRs #5, #6, #7, #8, #14

Basis-Branch: `cline/power-history-diag`
Basis-Stand bei dieser Pruefung: `6ba6af3` (HEAD, nach den Merges #18, #19, #20)
Nachpruefung: `bcb9938` — die drei Commits dazwischen (`57516a3`, `c6598fb`,
`bcb9938`) aendern **nur** `remote-shell/memory/SHARED.md`. Alle Befunde,
Zeilennummern und der #6-Merge-Konflikt wurden gegen `bcb9938` erneut
verifiziert und sind unveraendert.
Erstellt: 2026-10-18, reine Host-/Repo-Arbeit.

> **Geltungsbereich.** Es wurde **kein Board angefasst** und **kein
> remote-shell-Kanal** benutzt. Geprueft wurden ausschliesslich die Diffs
> `git diff origin/cline/power-history-diag...origin/<head>`, die Shell-Syntax
> (`sh -n`), die mitgelieferten Host-Tests sowie die Merge-Vertraeglichkeit
> gegen den aktuellen Basis-Branch (`git merge-tree`). Kein PR wurde geaendert,
> nichts wurde gemerged.

## Kurzuebersicht

| PR | Head-Branch | Host-Checks (jetzt) | Auf Basis vorhanden? | Merge-Vertraeglichkeit | Empfehlung |
|----|-------------|---------------------|----------------------|------------------------|------------|
| #5 | `feat/lowfreq-test-overlay-safe-restore` | `sh -n` ok; Test **21/28** (umgebungsabhaengig) | nein (alles neu) | konfliktfrei | **UPDATE** (kleine Fixes) |
| #6 | `feat/lowfreq-voltage-curve` | 44/44 ok | nein | **Konflikt** in `gen_lowfreq_dts.py`; bricht `gen_oc_dts.py` | **UPDATE** (blockierend) |
| #7 | `docs/pwm-regulator-notes` | 157/157 ok | nein | konfliktfrei | **UPDATE** (Genauigkeit) |
| #8 | `docs/pr-review-5-6-7` | (reine Doku) | nein | konfliktfrei | **CLOSE** (ueberholt) |
| #14 | `docs/uv-software-limit-report` | Tests gruen (siehe unten) | nein, aber 66 Dateien | konfliktfrei, aber Scope/Staleness | **UPDATE** (Rebase + Scope teilen) |

`git merge-tree --write-tree origin/cline/power-history-diag origin/<head>`:

```
#5  rc=0   (kein Konflikt)
#6  rc=1   CONFLICT (content): remote-shell/ha/gen_lowfreq_dts.py
#7  rc=0
#8  rc=0
#14 rc=0
```

---

## PR #5 — one-boot test overlay + stock restore

Neue Dateien: `deploy_lowfreq_safe.sh` (274 Z.), `restore-stock.sh` (155 Z.),
`test_lowfreq_safe.sh` (104 Z.). Keine bestehende Funktion geaendert.

### Zweck erfuellt?

**Ja.** Das Kernversprechen ("Overlay genau einen Boot aktiv, danach automatisch
zurueck") ist im Code umgesetzt und host-getestet:

- `deploy_lowfreq_safe.sh` setzt `user_overlays=` **leer** und legt den Marker
  `/etc/lowfreq-test.pending` an.
- Die erzeugte Unit `lowfreq_test_hold.sh` traegt bei vorhandenem Marker
  `user_overlays=lowfreq` ein und loescht den Marker; ohne Marker setzt sie
  `user_overlays=` wieder leer. Der Test bildet Boot 1/2/3 ab (Abschnitt 4).
- `restore-stock.sh` ist idempotent (Test Abschnitt 6/7) und entfernt auch
  `cpu-hotplug.service` und `lowfreq-test-hold.service`.

### Boot-Loop-Risiko

Kein akutes Risiko, aber eine **echte Schutzluecke**:

1. **`CPUFREQUTILS` ist toter Code.** Die Variable wird in
   `deploy_lowfreq_safe.sh:84` deklariert und danach **nirgends** benutzt
   (verifiziert mit `grep -n CPUFREQUTILS`). Das Skript schreibt
   `/etc/default/cpufrequtils` nie. Genau diese Datei ist aber die dokumentierte
   Falle des Repos (`LOWFREQ-README.md`, Abschnitt "Wichtig: zwei Stellen fuer
   die Untergrenze"): Armbians `armbian-hardware-optimization` liest `MIN_SPEED`
   daraus und setzt `scaling_min_freq` **nach** `cpu-freq-guard.service`. Ist
   `MIN_SPEED` dort nicht `1000000`, kann der 1.0-GHz-Boden unterlaufen werden.
   Der Kommentar im Skript ("only reachable by an explicit probe") behauptet
   damit mehr, als das Skript allein sicherstellt. Der Punkt ist **geerbt**:
   auch `deploy_lowfreq.sh` und `deploy_oc_safe.sh` auf dem Basis-Branch fassen
   `cpufrequtils` nicht an (verifiziert). `lowfreq_set_floor.sh` macht es
   richtig und sollte hier als Vorbild dienen.

### Weitere Befunde

2. **`--dry-run` bricht ohne Board-DTB ab.** Die Hilfe sagt "Braucht kein root
   und kein Board", aber `deploy_lowfreq_safe.sh:145` ruft `die` beim fehlenden
   Board-DTB - **auch im Dry-Run**. Reproduziert:

   ```
   $ LOWFREQ_ROOT=<baumnur-mit-armbianEnv> sh deploy_lowfreq_safe.sh
   == 1. Overlay bauen und pruefen ...
   FEHLER: <root>/boot/dtb/amlogic/meson-g12b-odroid-n2-plus.dtb fehlt ...
   rc=1
   ```

   Empfehlung: im Dry-Run nur warnen.

3. **`user_overlays` wird komplett ueberschrieben.** `set_user_overlays()`
   ersetzt die ganze Zeile (`sed -i "s/^user_overlays=.*/user_overlays=$1/"`).
   Ein eventuell vorhandener Fremd-Eintrag (z. B. `user_overlays=spi1`) geht
   still verloren. Fuer dieses Board unkritisch, aber unsauber. (Vgl. `#14` und
   `deploy_oc_safe.sh`, die additiv arbeiten - hier nicht konsistent.)

4. **Host-Test ist umgebungsabhaengig - und faellt mit installiertem `dtc` um.**
   Auf einem Host **ohne** `dtc` laeuft `test_lowfreq_safe.sh` gruen. Sobald
   `dtc` vorhanden ist, greift der Build-Zweig und das Skript bricht beim
   fehlenden Board-DTB ab; der Test meldet dann `7 Test(s) fehlgeschlagen`:

   ```
   $ sh test_lowfreq_safe.sh      # Host mit device-tree-compiler
   FAIL - deploy: user_overlays leer (erwartet 'user_overlays=', war 'user_overlays=lowfreq')
   ...
   7 Test(s) fehlgeschlagen
   ```

   Das ist derselbe Fehler wie Punkt 2. PR #8 hat "28/28 ok" gemeldet - das galt
   nur fuer eine `dtc`-lose Umgebung.

### Shell-Syntax / Tests

- `sh -n` fuer alle drei Skripte: **ok**.
- `test_lowfreq_safe.sh`: **21/28 ok** auf einem Host mit `dtc` (28/28 ohne).

**Fazit #5: UPDATE.** Inhaltlich richtig und mergebar. Vor einem echten
Board-Lauf sollte der `MIN_SPEED`/`cpufrequtils`-Schutz nachgezogen werden
(Punkt 1); Punkt 2/4 (Dry-Run + Testrobustheit) und Punkt 3 (`user_overlays`
erhalten) sind kleine, klar umrissene Fixes.

---

## PR #6 — per-rate voltage curve for the lowfreq OPP overlay

Dateien: `gen_lowfreq_dts.py` (+167/-15), `test_gen_lowfreq_dts.py` (+100/-2).

### Zweck erfuellt?

Ja. `--big-table` / `--little-table` erlauben eine Spannung **pro Takt**
(inline `RATE=VALUE,...` oder JSON `@datei`), die alten `--big-mv` /
`--little-mv` bleiben. Die Schutzgrenzen stehen **wirklich im Code**:

- `REG_FLOOR_UV = 680000` (`:73`), harter Abbruch in `check_curve()`
  (`:196-198`) fuer beide Wege.
- `RESERVE_WARN_UV = 695000` (`:77`), Warnung auf stderr (`:199-202`).
- Unbekannte Rate und > 14 Raten werden abgelehnt; `--mv`+`--table` zusammen
  ebenfalls.

### BLOCKIEREND: Der PR bricht jetzt den bereits gemergten OC-Pfad (#20)

Auf dem Basis-Branch (nach Merge #20) gibt es `gen_lowfreq_dts.py:render(...)`,
das `gen_oc_dts.py` importiert:

```
# gen_oc_dts.py (Basis, PR #20):
import gen_lowfreq_dts as lf
return lf.render(big_mv, little_mv, extra_big=[(rate_hz, uv, 'OC')], header_note=note)
```

Der PR #6 **entfernt `render()`** und ersetzt es durch
`build_curve()`/`check_curve()`/`main()`. Folgen, host-reproduziert nach
simuliertem Merge:

```
$ git merge-tree ... origin/feat/lowfreq-voltage-curve
CONFLICT (content): Merge conflict in remote-shell/ha/gen_lowfreq_dts.py

$ python3 gen_oc_dts.py --with-lowfreq --out /tmp/oc.dts
  File "gen_oc_dts.py", line 96, in render_full
    return lf.render(big_mv, little_mv, ...)
AttributeError: module 'gen_lowfreq_dts' has no attribute 'render'

$ python3 test_gen_oc_dts.py
KeyError: 2496000000
```

Der Konflikt ist aufloesbar, aber der PR muss beim Rebase `render()` (oder eine
kompatible API) wieder anbieten, sonst ist `gen_oc_dts.py --with-lowfreq` und
`test_gen_oc_dts.py` kaputt. Das ist ein **Regressionsrisiko**, kein reines
Merge-Artefakt.

### Weitere Befunde

1. **Keine Obergrenze.** `--big-mv 2000` bzw. `--big-table 24000=2000` laufen
   ohne Fehler durch und schreiben `opp-microvolt = <2000000>`. Verifiziert:
   ```
   $ python3 gen_lowfreq_dts.py --big-mv 2000 --out /tmp/o1.dts
   /tmp/o1.dts geschrieben: ... (big 2000 mV, little 761 mV)   # rc=0
   ```
   Ueber dem Regler-Maximum (N2+ 1040 mV) ist der Wert physikalisch
   unerreichbar (der Regler klemmt), also **kein Boot-Loop-Risiko**, aber ein
   stiller Fehlwert im DTB. Eine Warnung/Ablehnung > Werksmaximum waere
   konsistent mit dem Zweck.
2. **Kein Schutz gegen versehentlich hohe Werte bei negativen Eingaben** gibt
   es nicht - negative Werte werden korrekt abgelehnt (`24000=-5` -> Abbruch).
   Also nur die Obergrenze fehlt.
3. **Rueckwaerts-Kompatibilitaet bestaetigt:** Ohne Argumente ist die Ausgabe
   byte-identisch zur committeten `lowfreq.dts` (`diff` leer).

### Shell-Syntax / Tests

- `py_compile` ok. `test_gen_lowfreq_dts.py`: **44/44 ok**. Deckt inline/JSON,
  Boden-Ablehnung, Reserve-Warnung, unbekannte Rate, > 14 Raten und die
  `--mv`+`--table`-Kollision ab.

**Fazit #6: UPDATE (blockierend).** Vor dem Merge **rebasen** und `render()`
bzw. eine kompatible Schnittstelle wiederherstellen, sonst bricht der
OC-Overlay-Pfad (#20) samt Test. Die fehlende Obergrenze ist eine Empfehlung.

---

## PR #7 — research notes on the pwm-regulator and Amlogic DVFS

Dateien: `PWM-REGULATOR-NOTES.md` (447 Z.), `pwm_reg_model.py` (233 Z.),
`test_pwm_reg_model.py` (156 Z.). Reine Doku + Host-Modell.

### Zweck erfuellt?

Ja. Die Notizen erklaeren die beiden `pwm-regulator`-Modi, das Kalibrierproblem
und wer auf G12B Takt/Spannung setzt. Die Aussagen sind plausibel und im
Wesentlichen korrekt; die tragenden Punkte (kein SCPI im Mainline-Pfad,
`cpufreq-dt` + `pwm-regulator`, `opp-microvolt` autoritativ) decken sich mit
dem Repo-Befund.

### Befunde (aus PR #8 uebernommen und verifiziert)

1. **Registerformel vertauscht** (`PWM-REGULATOR-NOTES.md:169`). Der Text
   zitiert korrekt `channel->hi = duty_cnt; channel->lo = cnt - duty_cnt;` und
   `FIELD_PREP(PWM_HIGH_MASK, hi) | FIELD_PREP(PWM_LOW_MASK, lo)` (HIGH = Bits
   31..16), schreibt dann aber `((cnt - duty_cnt) << 16) | duty_cnt`. Richtig
   ist **`(duty_cnt << 16) | (cnt - duty_cnt)`**. Die Formel widerspricht der
   unmittelbar darueber zitierten C-Zeile.
2. **Vakuumer "Entkopplungs"-Test.** `test_pwm_reg_model.py` vergleicht
   `table_select(900000, 900000, table)[1]` mit sich selbst - immer wahr.
   Ebenso ist `pwm_reg_model.py:183` (`demo_table_decoupling`) ein No-op: er
   ruft `table_select(req, req, table)` zweimal mit identischen Argumenten und
   kommentiert "min_uv-Klemme geaendert", ohne die Klemme zu aendern. Der
   eigentliche Beweis (gleicher Eintrag, geaenderte `min/max`-Klemme -> gleicher
   duty) fehlt.
3. **Unbelegter Cross-Check** (`PWM-REGULATOR-NOTES.md:369-371`): behauptet
   einen Vergleich gegen `uv.py.uv_to_duty`/`duty_to_uv`. `uv.py` wird in
   `test_pwm_reg_model.py`/`pwm_reg_model.py` **nirgends** importiert
   (verifiziert per grep). Entweder implementieren oder als Herleitung ohne
   Testbezug kennzeichnen.
4. **Abschnitt 6 fehlt.** Ueberschriften springen von `## 5` (Zeile 314) direkt
   zu `## 7` (Zeile 341).

### Shell-Syntax / Tests

- `py_compile` ok. `test_pwm_reg_model.py`: **157/157 ok** (rein logisch).

**Fazit #7: UPDATE (Genauigkeit).** Inhaltlich wertvoll und mergebar; die
Registerformel, der vakuume Test und der unbelegte Cross-Check sollten
korrigiert werden, weil sie als Beleg fuer die spaetere Arbeit dienen.

---

## PR #8 — review of the open PRs #5, #6, #7

Datei: `remote-shell/ha/PR-REVIEW.md` (282 Z.), Stand Basis `6378017`
(2026-10-17), also **vor** den Merges #18/#19/#20 und vor diesem Review.

### Ist der PR durch die spaeteren Merges ueberholt?

**Ja, in wesentlichen Punkten:**

1. **Falsches Merge-Urteil fuer #6.** PR #8 nennt #5/#6/#7 "merge-faehig mit
   Anmerkungen". Fuer #6 stimmt das **nicht mehr**: seit Merge #20
   (`af08832`, `gen_oc_dts.py` importiert `lf.render`) kollidiert #6 mit dem
   Basis-Branch und bricht `gen_oc_dts.py --with-lowfreq`/`test_gen_oc_dts.py`
   (siehe PR #6 oben). PR #8 konnte das nicht wissen.
2. **Zahlenangaben sind umgebungsabhaengig.** PR #8 meldet `#5: 28/28`. Auf
   einem Host mit `dtc` sind es **21/28** (Punkt 4 bei #5). Die Aussage ist
   damit nicht reproduzierbar haltbar.
3. **Basis-Stand veraltet.** Die im Dokument genannten Zeilennummern beziehen
   sich auf `6378017` und die PR-Branches von damals; der Basis-Branch hat
   seither `gen_lowfreq_dts.py` (durch #20) strukturell geaendert.
4. **Sachlich weiter gueltige Befunde.** Die konkreten Funde (MIN_SPEED-Luecke
   in #5, fehlende Obergrenze in #6, Registerformel/vakuumer Test in #7) sind
   korrekt und in dieses Dokument uebernommen - sie bleiben wertvoll.

### Empfehlung

**CLOSE.** Der PR ist ein Momentaufnahme-Review, dessen zentrales Urteil
("alle drei merge-faehig") durch die spaeteren Merges ueberholt ist. Sein
Inhalt ist in `PR-REVIEW-ODROID.md` (dieses Dokument) aufgegangen und
aktualisiert. Falls er erhalten bleiben soll, muesste er als historisch
gekennzeichnet und das #6-Urteil korrigiert werden - der Aufwand lohnt
gegenueber dem CLOSE nicht.

---

## PR #14 — ramoops/pstore + Boot-Journal (Branch `docs/uv-software-limit-report`)

### Umfang

Der PR-Body beschreibt **ramoops/pstore**, der Branch traegt aber **66 Dateien,
+4913/-148** aus vielen Themen: Boot-Journal, htopv-Panel, noVNC/XFCE,
vnc-lan-guard, CPU-Hotplug (MIN_ONLINE=1), PLL-OC-Proben, Subagent-Tools, plus
Kanal-Skripte (`cmd_agent.py`, `register_sandbox.py`, `publish.py`). Der Branch
ist **14 Commits hinter** dem Basis-Branch (`git rev-list --left-right --count`:
`14 22`).

### Funktionale Pruefung (ramoops/Boot-Journal)

Der ramoops-Teil ist **substanziell und auf dem Board verifiziert** (Aussage des
PR-Bodys; hier nur host-geprueft):

- `gen_ramoops_dts.py` erzeugt `ramoops.dts`; `render()` ist **byte-identisch**
  zur committeten `ramoops.dts` (verifiziert).
- Adresse `0x50000000` liegt laut `test_gen_ramoops_dts.py` im freien Fenster
  `0x40000000..0xdfd2dfff` und kollidiert mit keiner bekannten Reservierung
  (secmon, Kernel, CMA, Framebuffer). Das ist der **kritische** Punkt bei
  CPU-/RAM-nahen Overlays und sauber abgesichert.
- `boot_journal_lib.classify_reset()` und die pstore-Dedupe-Logik
  (`consumed_pstore_mtime`, monoton) sind reine Funktionen mit klarer Semantik.
  Die im PR-Body beschriebene Bugfix-Erzaehlung (alter Archiv-Dump
  ueberschrieb die neuere Marke) ist im Code nachvollziehbar: `find_pstore_panic`
  verwirft `mtime <= exclude_mtime`, und `cmd_start` haelt
  `consumed_pstore_mtime` sticky/monoton.

### Host-Tests (alle gruen)

```
test_gen_ramoops_dts.py     gen_ramoops_dts: 26 Checks, 0 Fehler
test_boot_journal_lib.py    boot_journal_lib: 36 Checks, 0 Fehler
test_boot_journal_pstore.py test_boot_journal_pstore: 12 Checks, 0 Fehler
test_boot_journal_flow.py   test_boot_journal_flow: 10 Checks, 0 Fehler
test_vnc_lan_guard.py       vnc_lan_guard: 31 Checks, 0 Fehler
test_cpu_live.py            cpu-live: 13 Checks, 0 Fehler
test_htopv.py               htopv: 15 Checks, 0 Fehler
test_cpu_hotplug.py         134 Checks, 0 Fehler
remote-shell/test_channel_backoff.py  channel-backoff: 15 Checks, 0 Fehler
./test/run_host_tests.sh    Firmware-Suite gruen (u. a. stability 65 Checks)
```

`sh -n` fuer die geaenderten `.sh`: alle ok. **Ausnahme:**
`remote-shell/ha/reverse_ssh.sh` hat `#!/bin/bash` und ist **unveraendert aus
`tools/remote-shell/` verschoben** (R100, 0 Aenderungen); `sh -n` schlaegt dort
fehl (`Syntax error: "(" unexpected`). Kein neuer Fehler, nur ein Hinweis, dass
die Datei nicht `/bin/sh`-tauglich ist.

### Probleme fuer einen Merge

1. **Stale Basis / Scope.** Ein Merge bringt einen 14-Commits-alten Stand und
   fasst viele **geteilte** Dateien an (`remote-shell/cmd_agent.py`,
   `register_sandbox.py`, `publish.py`, `memory/SHARED.md`,
   `ha/LOWFREQ-README.md`, `ha/deploy_lowfreq.sh`, `ha/cpu_hotplug.py`). Der
   Merge-Baum ist konfliktfrei, aber die Aenderungen an `LOWFREQ-README.md`
   (u. a. `MIN_ONLINE=1`, Monitor-/Takt-Korrektur) sind gegen den heutigen
   Basis-Stand nicht mehr abgeglichen - hier ist eine fachliche Nachkontrolle
   noetig, kein Auto-Merge.
2. **Loescht Basis-Dateien.** Der Merge entfernt
   `tools/remote-shell/odroid-novnc-setup.sh` und
   `tools/remote-shell/odroid-setup-desktop.sh` (Migration nach
   `remote-shell/ha/`). Das ist beabsichtigt (`publish.py` raeumt sie mit),
   sollte aber bewusst bestaetigt werden.
3. **Ueberlappt thematisch mit bereits Gemergtem.** #19 hat den externen
   noVNC-Bridge-Pfad gemergt; #14 bringt die LAN-Desktop-Variante
   (`novnc.service`, `xfce-vnc.service`, `deploy_novnc.sh`,
   `vnc_lan_guard.py`). Keine Datei-Kollision, aber inhaltlich nah - die
   READMEs sollten sich nicht widersprechen (host-seitig nicht abschliessend
   pruefbar, da Board-Verifikation).
4. **Test `test_n.py` ist kein Host-Test.** Er oeffnet `/dev/mem` und ist nur
   mit Argument auf dem Board lauffaehig (`IndexError: list index out of range`
   ohne Argument). Er wird nicht in `./test/run_host_tests.sh` gefuehrt - ok,
   aber im Branch leicht mit einem Host-Test zu verwechseln.

**Fazit #14: UPDATE.** Der ramoops/pstore-Kern ist wertvoll und auf dem Board
belegt; als Ganzes ist der Branch aber ein 14-Commits-alter Sammel-Branch, der
geteilte Infrastruktur-Dateien anfasst. Empfehlung: **rebasen und in
thematische PRs teilen** (mindestens: ramoops+boot-journal | htopv |
noVNC/vnc-lan-guard | Hotplug/PLL | Kanal-Skripte), dann einzeln mergen. Kein
Merge in der aktuellen Form.

---

## Zusammenfassung

| PR | Empfehlung | Kernpunkt |
|----|------------|-----------|
| #5 | **UPDATE** | `MIN_SPEED`/`cpufrequtils`-Schutz fehlt (toter Code); Dry-Run/Test mit `dtc` fixen; `user_overlays` erhalten. |
| #6 | **UPDATE (blockierend)** | Merge-Konflikt + entfernt `render()` -> `gen_oc_dts.py`/`test_gen_oc_dts.py` kaputt. Vor Merge rebasen. |
| #7 | **UPDATE** | Registerformel vertauscht, vakuumer Test, unbelegter Cross-Check, Abschnitt 6 fehlt. |
| #8 | **CLOSE** | Momentaufnahme, durch #18-#20 ueberholt; Urteil zu #6 jetzt falsch; Inhalt hier aktualisiert. |
| #14 | **UPDATE** | ramoops-Teil stark, aber 14 Commits stale und breiter Sammel-Scope; rebasen + teilen. |

Kein PR fuehrt zu einem Boot-Loop oder Datenverlust, und keiner der PRs wurde
veraendert. Reine Host-/Repo-Arbeit: **kein Board angefasst, kein
remote-shell-Kanal benutzt.**

## Durchgefuehrte Checks (reproduzierbar)

```bash
# Merge-Vertraeglichkeit je PR
for h in feat/lowfreq-test-overlay-safe-restore feat/lowfreq-voltage-curve \
         docs/pwm-regulator-notes docs/pr-review-5-6-7 docs/uv-software-limit-report; do
  git merge-tree --write-tree origin/cline/power-history-diag origin/$h >/dev/null \
    && echo "$h: konfliktfrei" || echo "$h: KONFLIKT"
done

# Shell-Syntax (#5, #14)
sh -n remote-shell/ha/deploy_lowfreq_safe.sh remote-shell/ha/restore-stock.sh
sh -n remote-shell/ha/deploy_ramoops.sh remote-shell/ha/revert_ramoops.sh

# Host-Tests je PR
sh      remote-shell/ha/test_lowfreq_safe.sh          # 21/28 mit dtc, 28/28 ohne
python3 remote-shell/ha/test_gen_lowfreq_dts.py       # 44/44
python3 remote-shell/ha/test_pwm_reg_model.py         # 157/157
python3 remote-shell/ha/test_gen_ramoops_dts.py       # 26
python3 remote-shell/ha/test_boot_journal_lib.py      # 36
python3 remote-shell/ha/test_boot_journal_pstore.py   # 12
python3 remote-shell/ha/test_boot_journal_flow.py     # 10
python3 remote-shell/test_channel_backoff.py          # 15

# #6: Rueckwaerts-Kompatibilitaet + Regression am OC-Pfad
python3 remote-shell/ha/gen_lowfreq_dts.py --out /tmp/lf.dts \
  && diff /tmp/lf.dts remote-shell/ha/lowfreq.dts   # leer = identisch
python3 remote-shell/ha/gen_oc_dts.py --with-lowfreq --out /tmp/oc.dts
# -> AttributeError: module 'gen_lowfreq_dts' has no attribute 'render'
```

_Hinweis: Dieses Review wurde von einem KI-Agenten (OpenHands) im Auftrag des
Nutzers erstellt._
