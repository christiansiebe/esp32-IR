# Review der offenen PRs #5, #6, #7

Basis-Branch: `cline/power-history-diag` (Stand `6378017`).

> **Geltungsbereich.** Reine Host-/Repo-Pruefung. Es wurde **kein Board
> angefasst** und **kein remote-shell-Kanal** benutzt. Geprueft wurden die
> Diffs `git diff origin/cline/power-history-diag...origin/<head>`, die
> Shell-Syntax (`sh -n`), die mitgelieferten Host-Tests sowie - fuer die
> Spannungsaussagen - der Linux-Quelltext/Device-Tree aus `torvalds/linux`
> `master` (Stand 2026-10-17).

## Kurzuebersicht

| PR | Branch | Head | Host-Checks | Ergebnis |
|----|--------|------|-------------|----------|
| #5 | `feat/lowfreq-test-overlay-safe-restore` | `bcf1027` | `sh -n` ok; `test_lowfreq_safe.sh` 28/28 ok | **merge-faehig mit Anmerkungen** |
| #6 | `feat/lowfreq-voltage-curve` | `f47da65` | `test_gen_lowfreq_dts.py` 44/44 ok | **merge-faehig mit Anmerkungen** |
| #7 | `docs/pwm-regulator-notes` | `89237de` | `test_pwm_reg_model.py` 157/157 ok | **merge-faehig mit Anmerkungen** |

Kein PR fuehrt zu einem Datenverlust oder einer Sicherheitsluecke. Die
Schutzgrenzen der Spannungs-PR (#6) stehen **wirklich im Code**, nicht nur in
Kommentaren. Die Anmerkungen unten sind Genauigkeits-/Konsistenzpunkte und
betreffen ueberwiegend Doku und Tests, nicht die Kernfunktion.

---

## PR #5 - one-boot test overlay + stock restore

Dateien: `deploy_lowfreq_safe.sh` (274 Z.), `restore-stock.sh` (155 Z.),
`test_lowfreq_safe.sh` (104 Z.). Neue Dateien, keine bestehende Funktion
geaendert.

### Zweck erfuellt?

Ja. Das Kernversprechen - "Overlay genau einen Boot aktiv, danach automatisch
zurueck" - ist im Code umgesetzt und wird host-getestet:

- `deploy_lowfreq_safe.sh:174` setzt `user_overlays=` **leer** und legt den
  Marker `/etc/lowfreq-test.pending` an (`:254`).
- Die erzeugte Unit `lowfreq_test_hold.sh` traegt bei vorhandenem Marker
  `user_overlays=lowfreq` ein und loescht den Marker; ohne Marker setzt sie
  `user_overlays=` wieder leer. Der Test bildet Boot 1/2/3 ab
  (`test_lowfreq_safe.sh`, Abschnitt 4) - alle gruen.

### Schutzgrenzen (Boot-Loop)

- `cpufreq.default_governor=performance` wird in `extraargs` gesetzt
  (`deploy_lowfreq_safe.sh:178`).
- `cpu-freq-guard.service` wird installiert/aktiviert und pinnt die Untergrenze
  auf 1.0 GHz (`:189-195`).
- **Aber:** die Boot-Governor-Zeile ist **nicht** der volle Schutz. Genau die
  Falle, die das Repo selbst dokumentiert (`LOWFREQ-README.md`, Abschnitt
  "zwei Stellen fuer die Untergrenze"), wird hier nicht geschlossen: Armbians
  `armbian-hardware-optimization` liest `MIN_SPEED` aus
  `/etc/default/cpufrequtils` und setzt `scaling_min_freq` **nach**
  `cpu-freq-guard.service`. `deploy_lowfreq_safe.sh` fasst
  `/etc/default/cpufrequtils` **nie** an (Variable `CPUFREQUTILS` in `:84`
  wird deklariert, aber nirgends benutzt - toter Code). Ist `MIN_SPEED` dort
  nicht `1000000`, kann der Boot-Schutz unterlaufen werden. Der Kommentar
  `:25-28` ("same as deploy_lowfreq.sh ... only reachable by an explicit
  probe") behauptet mehr, als das Skript allein sicherstellt. Der Punkt ist
  **geerbt**: auch der Basis-`deploy_lowfreq.sh` schreibt `MIN_SPEED` nicht,
  und `lowfreq_set_floor.sh` setzt es erst, wenn man die Untergrenze bewusst
  senkt. PR #5 verschaerft das Problem nicht, wiederholt aber die Zusage.

### Konkrete Punkte

1. **`deploy_lowfreq_safe.sh:84` - `CPUFREQUTILS` ist toter Code.**
   Die Variable wird nie gelesen/geschrieben. Entweder entfernen oder - besser -
   analog zu `lowfreq_set_floor.sh` `MIN_SPEED=1000000` in
   `/etc/default/cpufrequtils` setzen, damit der 1.0-GHz-Boden auch nach
   Armbians Optimierungsschritt haelt. Sonst ist die Aussage `:25-28` zu stark.
   (Kein akuter Boot-Loop, aber eine reale Schutzluecke.)

2. **`deploy_lowfreq_safe.sh:141-146` - harter Abbruch trotz `--dry-run`-Zusage.**
   Wenn `dtc` installiert ist und `lowfreq.dts` existiert, aber das Board-DTB
   fehlt, bricht `:145` mit `die` ab - auch im Dry-Run. Die Hilfe `:50` sagt
   aber "Braucht kein root und kein Board". Verifiziert:
   `LOWFREQ_ROOT=<baumnur-mit-Env> sh deploy_lowfreq_safe.sh --dry-run` ->
   `FEHLER: .../meson-g12b-odroid-n2-plus.dtb fehlt`, exit 1. In einer
   Testumgebung ohne DTB ist der Dry-Run damit nicht benutzbar. Empfehlung:
   die DTB-Pruefung im Dry-Run nur warnen statt `die`.

3. **`deploy_lowfreq_safe.sh:97` - `user_overlays` wird komplett ueberschrieben.**
   `sed -i "s/^user_overlays=.*/user_overlays=$1/"` ersetzt eine eventuell
   vorhandene Liste (z. B. `user_overlays=spi1`) durch den leeren/neuen Wert.
   Im Test reproduziert: aus `user_overlays=spi1` wird nach `deploy` + `restore`
   gar keine Zeile mehr. Fuer dieses Board unkritisch (nur lowfreq-Overlay),
   aber ein stiller Verlust fremder Overlay-Eintraege. Ein Kommentar oder
   Erhalt der uebrigen Namen waere sauberer.

4. **Kein `sync` vor dem impliziten Reboot-Hinweis / Reihenfolge Marker vs. cp.**
   Der Marker (`:254`) wird **nach** dem Kopieren des DTBO (`:168`) gesetzt -
   korrekt. `restore-stock.sh:155` empfiehlt `sync && reboot`, das ist ok.
   Kein Fehler, nur zur Bestaetigung der Reihenfolge geprueft.

5. **`deploy_lowfreq_safe.sh:112-118` - `--revert` exec't `restore-stock.sh`
   korrekt**, respektiert `--dry-run`. Getestet, ok.

### Shell-Syntax / Tests

- `sh -n` fuer alle drei Skripte: **ok**.
- `test_lowfreq_safe.sh`: **28/28 ok** (ohne Board, mit Stub-`systemctl`).
- `restore-stock.sh` ist idempotent (Test Abschnitt 7) und deckt die
  Doku-Zusage `:8` (cpu-hotplug mitentfernen) im Code ab (`:106`).

**Fazit #5:** merge-faehig. Punkt 1 (MIN_SPEED-Schutz) sollte vor einem echten
Board-Lauf nachgezogen werden, Punkt 2 ist ein Testkomfort-Fehler.

---

## PR #6 - per-rate voltage curve for the lowfreq OPP overlay

Dateien: `gen_lowfreq_dts.py` (+167/-15), `test_gen_lowfreq_dts.py` (+100/-2).

### Zweck erfuellt?

Ja. Neue Optionen `--big-table` / `--little-table` erlauben eine Spannung **pro
Takt** (inline `RATE=VALUE,...` oder JSON-Datei mit `@`-Praefix, flach oder
nach Cluster verschachtelt). Die alte `--big-mv` / `--little-mv`-Schnittstelle
bleibt erhalten.

### Schutzgrenzen - wirklich im Code?

**Ja, das ist der wichtigste Punkt und er ist sauber geloest.** Beide Grenzen
stehen als aktive Pruefung in `check_curve()` und nicht nur in Kommentaren:

- **Regler-Untergrenze 680 mV:** `gen_lowfreq_dts.py:73` `REG_FLOOR_UV = 680000`,
  geprueft in `:196-198` (`uv < REG_FLOOR_UV` -> `SystemExit`). Gilt fuer
  **beide** Wege (`--mv` und `--table`), weil beide durch `check_curve()`
  laufen. Getestet: `--big-table 24000=670` -> Abbruch, keine Datei
  (`test_gen_lowfreq_dts.py`, Abschnitt "Kurve unter dem Boden").
- **Reserve-Warnung unter 695 mV:** `:77` `RESERVE_WARN_UV = 695000`, `:199-202`
  sammelt Warnungen, `main()` gibt sie auf **stderr** aus. Getestet: 690 mV ->
  Warnung, 695 mV -> keine Warnung, auch fuer `--big-mv`.
- **Zusatzschutz:** unbekannte Raten (`:185`) und > 14 Raten (`:179-180`)
  werden abgelehnt; `--mv` und `--table` zusammen werden abgelehnt (`:168`).

### Konkrete Punkte

1. **`gen_lowfreq_dts.py:191-203` - keine Obergrenze.** Es gibt nur eine
   Untergrenze. `--big-mv 2000` bzw. `--big-table 24000=2000` laufen ohne
   Fehler durch und schreiben `opp-microvolt = <2000000>`. Verifiziert.
   Eine Spannung **ueber** dem Regler-Maximum (1040 mV) ist physikalisch
   unerreichbar, der Regler klemmt - also kein Boot-Loop-Risiko, aber ein
   stiller Fehlwert, der im DTB landet. Der PR-Doc-Text nennt nur die
   Untergrenze als Schutzgrenze. Eine analoge Warnung/Ablehnung fuer Werte
   ueber dem Werksmaximum (z. B. > 1040 mV) waere konsistent mit dem Zweck
   ("Schutzgrenzen wirklich im Code").

2. **`gen_lowfreq_dts.py:205-213` `describe()` - kosmetische Fehlanzeige.**
   Bei einer **konstanten** Kurve, deren Wert ungleich der Werksspannung ist
   (z. B. `--big-table 24000=700,50000=700,...`), zeigt `describe` mit
   `len(vals)==1` korrekt `big 700 mV`; bei gemischten Werten zeigt es
   `Kurve <min>-<max>`. Das ist ok. Nur: die Zusammenfassung nennt die Kurve
   ueber **alle 14** Raten, auch die unveraenderten - die Zahl "14 Punkte"
   kann daher mehr suggerieren, als tatsaechlich gesetzt wurde. Reine
   Kosmetik, kein Fehler.

3. **Rueckwaerts-Kompatibilitaet verifiziert:** Ohne Argumente und mit
   `--big-mv 690` ist die Ausgabe **byte-identisch** zum Basis-Branch
   (`diff` leer). Das Standardverhalten aendert sich also nicht.

### Shell-Syntax / Tests

- `py_compile`: ok. `test_gen_lowfreq_dts.py`: **44/44 ok**.
- Die Tests decken inline/JSON, Boden-Ablehnung, Reserve-Warnung, unbekannte
  Rate, > 14 Raten und die `--mv`+`--table`-Kollision ab.

**Fazit #6:** merge-faehig. Punkt 1 (fehlende Obergrenze) ist die einzige
inhaltliche Luecke und betrifft nur unplausible Eingaben; sie ist kein
Boot-Loop-Risiko.

---

## PR #7 - research notes on the pwm-regulator and Amlogic DVFS

Dateien: `PWM-REGULATOR-NOTES.md` (447 Z.), `pwm_reg_model.py` (233 Z.),
`test_pwm_reg_model.py` (156 Z.). Reine Doku + Host-Modell, kein Board-Code.

### Zweck erfuellt?

Ja. Die Notizen erklaeren die beiden `pwm-regulator`-Modi, das Kalibrier-Problem
und wer auf G12B Takt/Spannung setzt. Die zentralen technischen Aussagen habe
ich gegen `torvalds/linux` `master` geprueft:

- **Treiber-Verzweigung** `of_property_present(np, "voltage-table")` ->
  Table-Mode, sonst Continuous: **bestaetigt** (`pwm-regulator.c`).
- **Continuous-Formel** `dutycycle = min_uV_duty - (req-min)*diff_duty/diff_uV`
  bei invertierter Range: **bestaetigt** (`pwm_regulator_set_voltage`).
- **Voltage-Table `set_voltage_sel` schreibt den Tabellen-Tastgrad direkt, ohne
  min/max**: **bestaetigt** (`pwm_set_relative_duty_cycle(..., dutycycle, 100)`).
  `map_voltage = regulator_map_voltage_iterate`: **bestaetigt**.
- **N2+ Regler-Node** `680000..1040000`, `pwms ... 1500`: **bestaetigt**
  (`meson-g12b-odroid-n2-plus.dts`). **N2** `721000..1022000`, `pwms ... 1250`:
  **bestaetigt** (`meson-g12b-odroid.dtsi`).
- **Kein `scpi`-Node in `meson-g12-common.dtsi`** (nur
  `secure-monitor = "amlogic,meson-gxbb-sm"`): **bestaetigt**.
- **SM-Kommandoliste ohne DVFS** (`SM_EFUSE_*`, `SM_GET_CHIP_ID`,
  `SM_THERMAL_CALIB_READ`, `SM_A1_PWRC_SET/GET`): **bestaetigt**
  (`meson_sm.h`).
- **Zitierte Commits** `d14734a04a8a`, `b96d4e92709b`, `6c4d636bc00d`,
  `62183863f708`: Betreffzeilen **bestaetigt** (GitHub-API).

Die im Repo gemessene Form `V = 680000 + duty*360000/period` und die Periode
`999/333` sind genau die Continuous-Abbildung mit `pwm-dutycycle-range = <100 0>`
und decken sich mit `UV-README.md`.

### Konkrete Punkte

1. **`PWM-REGULATOR-NOTES.md:169` - falsche Registerformel.**
   Dort steht: "Der Registerinhalt ist also `((cnt - duty_cnt) << 16) | duty_cnt`".
   Im Treiber gilt aber `hi = duty_cnt`, `lo = cnt - duty_cnt`, und das Register
   ist `(hi << 16) | lo` (Bits 31..16 = HIGH, Bits 15..0 = LOW, `PWM_HIGH_MASK`
   = GENMASK(31,16)). Korrekt ist also **`(duty_cnt << 16) | (cnt - duty_cnt)`**.
   Der Satz widerspricht der unmittelbar darueber zitierten (richtigen) C-Zeile.
   Die Folgerung danach ("Repo-`duty` ist das Low-Feld") bleibt zufaellig richtig,
   aber die Formel ist vertauscht und sollte korrigiert werden.
   (`pwm-meson.c` verifiziert: `channel->hi = duty_cnt; channel->lo = cnt - duty_cnt;`)

2. **`PWM-REGULATOR-NOTES.md:369-371` - unbelegter Cross-Check.**
   Der Text behauptet: "Cross-Check des Modells gegen
   `uv.py.uv_to_duty`/`duty_to_uv`: die Abweichung ist immer <= halber
   Treiber-Schritt (1,8 mV)". `test_pwm_reg_model.py` importiert `uv.py`
   **nicht** und ruft diese Funktionen **nirgends** auf (grep: keine Referenz).
   Die Aussage ist also nicht durch die Tests belegt. Entweder den Cross-Check
   tatsaechlich implementieren oder den Satz als Behauptung/Herleitung ohne
   Testbezug kennzeichnen.

3. **`test_pwm_reg_model.py:102-104` - vakuumer "Entkopplungs"-Test.**
   Der Check vergleicht `table_select(900000,900000,table)[1]` mit sich selbst
   (identische Argumente), ist also immer wahr und prueft die Entkopplung
   **nicht**. Der eigentliche Beweis (gleicher Tabelleneintrag, geaenderte
   min/max-Klemme -> gleicher duty) fehlt. Sauber waere ein Vergleich mit
   unterschiedlicher `min_uV`/`max_uV`-Klemme, z. B.
   `table_select(600000,1040000,table) == table_select(900000,900000,table)`.
   Analog ist `pwm_reg_model.py:183` (`demo_table_decoupling`) ein No-op: es
   ruft `table_select(req, req, table)` zweimal mit identischen Argumenten und
   kommentiert "min_uv-Klemme geaendert", ohne die Klemme zu aendern.

4. **`PWM-REGULATOR-NOTES.md` - Abschnitt 6 fehlt.**
   Die Ueberschriften springen von `## 5` (`:314`) direkt zu `## 7` (`:341`).
   Entweder fehlt ein Abschnitt oder die Nummerierung ist verrutscht.

### Shell-Syntax / Tests

- `py_compile`: ok. `test_pwm_reg_model.py`: **157/157 ok** (rein logisch,
  kein Board). `pwm_reg_model.py` laeuft durch und zeigt die Demo-Tabellen.

**Fazit #7:** merge-faehig. Punkt 1 (Registerformel) und Punkt 3 (vakuumer Test)
sollten korrigiert werden, weil sie als Beleg fuer die spaetere Arbeit dienen.

---

## Gesamtempfehlung

Alle drei PRs erfuellen ihren Zweck und sind **merge-faehig**. Vor einem echten
Board-Lauf mit #5 sollte jedoch die `MIN_SPEED`/`cpufrequtils`-Schutzluecke
(Punkt 1 zu #5) geschlossen werden, weil sie die dokumentierte
Boot-Schutzbehauptung untergraebt. Die inhaltlichen Korrekturen in #7
(Registerformel, vakuumer Test) sind wichtig fuer die Nachvollziehbarkeit, aber
nicht blockierend. Keiner der Punkte erzeugt ein Datenverlust- oder
Sicherheitsproblem, und keiner der PRs wurde veraendert.

## Durchgefuehrte Checks (reproduzierbar)

```bash
# Shell-Syntax
sh -n remote-shell/ha/deploy_lowfreq_safe.sh
sh -n remote-shell/ha/restore-stock.sh
sh -n remote-shell/ha/test_lowfreq_safe.sh

# Host-Tests je PR
sh  remote-shell/ha/test_lowfreq_safe.sh          # 28/28 ok   (#5)
python3 remote-shell/ha/test_gen_lowfreq_dts.py   # 44/44 ok   (#6)
python3 remote-shell/ha/test_pwm_reg_model.py     # 157/157 ok (#7)

# Rueckwaerts-Kompatibilitaet #6 (Ausgabe byte-identisch zum Basis-Branch)
diff <(git show origin/cline/power-history-diag:remote-shell/ha/gen_lowfreq_dts.py \
        | python3 /dev/stdin --out /dev/stdout) \
     <(python3 remote-shell/ha/gen_lowfreq_dts.py --out /dev/stdout)
```
