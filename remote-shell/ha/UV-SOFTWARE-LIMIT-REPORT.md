# Software-Grenze der CPU-Versorgungsspannung (ODROID-N2+) - Host-Bericht

> **Geltungsbereich.** Reine Host-/Repo-Analyse. **Kein Board angefasst**, kein
> remote-shell-Kanal, keine Register/PWM/OPP/Spannung geschrieben, keine
> laufenden Dienste oder Systemdateien geaendert, keine PRs veraendert.
> Alle Kernel-Zitate stammen aus `torvalds/linux` `master` (Stand 2026-10-17),
> die DT-Werte aus denselben Quellen. Repo-Messwerte werden nur zitiert, nicht
> neu erhoben. **Hypothesen sind als solche markiert.**

## 1. Kurzantwort

**Der Treiber erzeugt bereits den kleinsten gueltigen PWM-Wert (LOW-Feld = 0);
eine reine Softwareaenderung innerhalb der vorhandenen Ansteuerung reicht nach
aktuellem Nachweis nicht aus, um tiefer als ~680-730 mV zu kommen.**

Begruendung in einem Satz: Der `pwm-regulator` ist ein **invertierter
Continuous-Mode-Regler**; bei `regulator-min-microvolt` (= 680000 uV) ist der
relative Tastgrad **100** und das meson-Register-**LOW-Feld 0**. Niedriger geht
der Registerwert nicht (`lo` ist ein Zaehler `0..cnt`). Ein Absenken von
`regulator-min-microvolt` **benennt den Boden nur um**, es erzeugt keinen
kleineren Registerwert (Kapitel 4). Das ist die Klasse **B** aus der
Aufgabenstellung.

**Nachtrag (Datenblatt-Belege, Kapitel 3a):** Die 680-mV-Grenze ist **nicht
willkuerlich**. Das **Amlogic-S922X-Datenblatt** (Rev 0.2, Sec. 4.2, S. 48)
spezifiziert `VDDCPU_A/B = 0.681 - 1.032 V`; das DT-Minimum 680000 uV = 0.680 V
ist genau diese Silizium-Untergrenze. Die beiden DC-DC-Konverter (**MP8756GD**
fuer A, **Silergy SY8120B1ABC** fuer B) koennen laut Datenblatt bis **0.600 V**
regeln - sie koennten also tiefer, duerfen aber nicht, weil die **CPU** unter
0.681 V nicht spezifiziert ist. Die Grenze ist damit **Hardware (SoC)** - die
Frage "Software oder Hardware" ist **entschieden: Hardware.**

## 2. Vollstaendiger Spannungssteuerpfad (mit Zeilenbelegen)

Quellen: `drivers/cpufreq/cpufreq-dt.c`, `drivers/regulator/core.c`,
`drivers/regulator/pwm-regulator.c`, `include/linux/pwm.h`,
`drivers/pwm/pwm-meson.c`.

| # | Stufe | Datei / Funktion / Zeile | Wirkung |
|---|---|---|---|
| 1 | OPP -> Zielspannung | `cpufreq-dt.c:56` `dev_pm_opp_set_rate()` | setzt Takt **und** `opp-microvolt` der OPP |
| 2 | Regler-Core: Klemmen | `core.c:442-457` `regulator_check_voltage()` | `if (*min_uV < rdev->constraints->min_uV) *min_uV = ...->min_uV;` -> **Anforderung < 680 mV wird nach OBEN auf 680 mV geklemmt** (nicht abgelehnt) |
| 3 | Regler-Core: Dispatch | `core.c:4056-4094` `regulator_set_voltage_unlocked()`; `core.c:4195` / `core.c:4782` `_regulator_do_set_voltage()` | reicht `min_uV/max_uV` weiter; `core.c:3928-3929` addiert `uV_offset` (hier 0, nicht gesetzt) |
| 4 | Regler-Core: Ops-Wahl | `core.c:3740` / `1273` `continuous_voltage_range` | Continuous-Modus -> `.set_voltage` (nicht `.set_voltage_sel`) |
| 5 | `pwm-regulator` | `pwm-regulator.c:190-233` `pwm_regulator_set_voltage()` | `min_uV = rdev->constraints->min_uV` (680000); `diff_uV = 1040000-680000 = 360000`; `dutycycle = DIV_ROUND_CLOSEST_ULL((req-min)*100, diff)`; `max_uV_duty(0) < min_uV_duty(100)` -> `dutycycle = 100 - dutycycle`; dann `pwm_set_relative_duty_cycle(&pstate, dutycycle, 100)` (Z. 227) und `pwm_apply_might_sleep()` (Z. 229) |
| 6 | Continuous-Init | `pwm-regulator.c:298-323` `pwm_regulator_init_continuous()` | liest `pwm-dutycycle-range` -> `min_uV_dutycycle=100`, `max_uV_dutycycle=0`; `pwm-dutycycle-unit` nicht gesetzt -> Default **100** (Prozent) |
| 7 | PWM-API | `pwm.h:255-265` `pwm_set_relative_duty_cycle()` | `duty_cycle[ns] = DIV_ROUND_CLOSEST_ULL(duty*period, scale)`; lehnt `duty_cycle > scale` mit `-EINVAL` ab (Z. 258) |
| 8 | meson-PWM | `pwm-meson.c:159-222` `meson_pwm_calc()` | `cnt = fin_freq*period/1e9`; `duty==period` -> `hi=cnt, lo=0`; `duty==0` -> `hi=0, lo=cnt`; sonst `hi=duty_cnt, lo=cnt-duty_cnt` |
| 9 | Register | `pwm-meson.c:241-242` | `value = FIELD_PREP(PWM_HIGH_MASK, hi) | FIELD_PREP(PWM_LOW_MASK, lo)` -> `(hi<<16)|lo`; `PWM_HIGH_MASK=GENMASK(31,16)`, `PWM_LOW_MASK=GENMASK(15,0)` (`pwm-meson.c:47-48`) |

**Antworten auf die Leitfragen**

- *Wo wird eine Anforderung < 680 mV begrenzt?* In `core.c:454`
  (`regulator_check_voltage`) - geklemmt, **nicht** abgelehnt. `-EINVAL` gaebe es
  nur, wenn nach dem Klemmen `min_uV > max_uV` waere.
- *Welche Spannung erreicht `pwm_regulator_set_voltage()`?* Immer
  `[max(req,680000), min(req,1040000)]` - der geklemmte Wert.
- *Wie wird der Tastgrad berechnet?* `dutycycle = 100 - round((req-680000)*100/360000)`
  (invertierte Logik, `max_uV_duty < min_uV_duty`).
- *Wie ins Register?* `pwm.h` rechnet in ns, `pwm-meson` in Zaehler `cnt`;
  `hi` = High-Anteil (Bits 31..16), `lo` = Low-Anteil (Bits 15..0).
- *Ist Tastgrad 0 der kleinste Ausgangswert?* **Ja.** Relativer Tastgrad 0
  entspricht `max_uV`; das **Low-Feld** ist dann `cnt` (maximal). Der kleinste
  *Register*-Wert (`lo=0`) tritt bei relativem Tastgrad 100 = `min_uV` = 680 mV
  auf. Genau das ist die Untergrenze.
- *Weitere Begrenzungen?* Ja, aber sie verschaerfen nur nach oben:
  `pwm_set_relative_duty_cycle` lehnt `duty>scale` ab (`pwm.h:258`); die
  meson-Registermaske fasst nur 16+16 Bit; `cnt>0xffff` wird abgelehnt
  (`pwm-meson.c:196`). Keine davon erlaubt einen kleineren Wert als `lo=0`.

### 2.1 Numerische Gegenprobe (Host, `verify_chain.py`-Logik)

Modell `LOW = round((1 - duty_rel/100) * period)` gegen die **Repo-Messwerte**
(`LOWFREQ-README.md`, big, Periode 999):

| req | Modell LOW | Repo LOW | Repo gemessen |
|---|---|---|---|
| 731 mV | 140 | 142 | 766,8 mV |
| 700 mV | 60 | 56 | 746,6 mV |
| 680 mV | 0 | 0 | 722,0 mV |

Die Uebereinstimmung (Abweichung <= 4 Zaehler = 1,4 mV) **bestaetigt die Kette**:
die Repo-`duty`-Werte sind das Register-LOW-Feld, und die Klemme sitzt genau an
`regulator-min-microvolt`.

## 3. Die 680-mV-Grenze: Software oder Hardware?

| Groesse | Wert | Herkunft | Art |
|---|---|---|---|
| `regulator-min-microvolt` | 680000 | `meson-g12b-odroid-n2-plus.dts:19,26` (Overlay ueber Basis-N2: 721000) | **Software-Klemme** (Regler-Core) |
| `regulator-max-microvolt` | 1040000 | ebd. | **Software-Klemme** |
| `pwm-dutycycle-range` | `<100 0>` | `meson-g12b-odroid.dtsi:149,168` | **Abbildung** (invertierte Logik) |
| PWM-Periode (DT) | 1500 ns (N2+), 1250 ns (N2) | `-n2-plus.dts:22,29` / `-odroid.dtsi:148,167` | **Anforderung**, Treiber rundet auf `cnt` (Repo: 999/333) |
| `pwm-dutycycle-unit` | nicht gesetzt -> 100 | Default `pwm-regulator.c:302` | **Abbildung** |
| OPP-Spannungen | big 731000..1011000, little 761000..1001000 | `meson-g12b-a311d.dtsi` | **Software-Sollwerte** (Mainline autoritativ) |

**Reine Softwaregrenzen:** `regulator-min/max-microvolt`, `pwm-dutycycle-range`,
die OPP-Spannungen. Sie sind im DT frei aenderbar.
**Aus der Beschaltung abgeleitet:** die *Bedeutung* von Tastgrad 0..100 als
Spannung. Der DT sagt: Tastgrad 100 <-> 680 mV, Tastgrad 0 <-> 1040 mV. Das ist
die Werkskalibrierung des Bauteils, nicht durch Linux messbar.

**Herkunft der 680000 (Commit-/Quellenlage):**
- Der Mainline-DT **nennt die Bauteile** im Kommentar:
  `VDDCPU_A` = **MP8756GD** (`meson-g12b-odroid.dtsi:136-140`),
  `VDDCPU_B` = **Silergy SY8120B1ABC** (Z. 155-159). Der "pwm-regulator" ist also
  ein per PWM gefilterter **Feedback-Eingang** eines Buck-Converters.
- Commit `d14734a04a8a` ("enable DVFS", N2) nennt den Bereich
  *"0.73V to 1.01V using 2 distinct PWM outputs ... with an inverse duty-cycle"* -
  das sind die **Basis-N2**-Werte (721000..1022000).
- Commit `98d24896ee11` (N2+): *"The rev. C support a slightly higher VDDCPU_A &
  VDDCPU_B voltages"* -> setzt **680000..1040000**.
- **Der Beleg fuer 680000 ist gefunden: das Amlogic-S922X-Datenblatt (Rev 0.2,
  2019-03-10), Abschnitt 4.2 "Recommended Operating Conditions", S. 48**,
  spezifiziert `VDDCPU_A/B (Cortex A73/A53) = 0.681 - 1.032 V`. Die
  DT-Untergrenze **680000 uV = 0.680 V** entspricht dem Datenblatt-Minimum
  **0.681 V** (Rundung auf 1 mV). Siehe Kapitel 4.1.
- Die DT-Obergrenze **1040000 uV** liegt dagegen *ueber* dem Datenblatt-Maximum
  1.032 V - sie ist nur durch die OPP-Tabellen gedeckelt, nicht durch den Chip.
- Die Werksbeschaltung ist damit **nicht** mehr "unbelegt": 680 mV ist die
  **Silizium-Untergrenze**, nicht eine willkuerliche Softwarezahl.

**Drei Begriffe sauber getrennt:**
1. niedrigste *zulaessige Anforderung* = 680000 uV (Software-Klemme),
2. niedrigster *vom Treiber erzeugbarer PWM-Wert* = LOW-Feld 0 (rel. Tastgrad 100),
3. niedrigste *reale Ausgangsspannung* = **unbekannt**; gemessen ~722-730 mV,
   aber durch die ADC-Mittelung nach oben verzerrt (Kapitel 5).

## 3a. Datenblatt-Belege (Hardware-Grenzen) - **neu, entscheidend**

Drei Primaerquellen wurden beschafft und ausgewertet (nur lesend, kein Board):

**A) Amlogic S922X Datasheet, Rev 0.2 (2019-03-10), Sec. 4.2
"Recommended Operating Conditions", S. 48** (Dokument traegt den Aufdruck
*"Confidential for Hardkernel!"*; oeffentlich gespiegelte Kopie):

> `VDDCPU_A/B  Voltage for Cortex A73/A53 CPU  0.681 - 1.032 V`

Fussnote 1 dazu: *"Minimal VDDCPU_A/B voltage is for sleep mode while system runs
at very low speed. ... Considering the power supply may have 3% deviation, the
minimal voltage in actual application should not be set to lower than min spec
plus 0.02V."* -> **Empfehlung fuer die Anwendung: >= 0.701 V.**

- Das **DT-Minimum 680000 uV = 0.680 V** trifft das **Silizium-Minimum 0.681 V**
  (1-mV-Rundung). **Das ist der fehlende Beleg fuer die 680-mV-Grenze.**
- Der Datenblatt-Maximalwert 1.032 V liegt **unter** dem DT-Maximum 1040000 uV.
- Wichtig: diese Werte gelten **an den Pins des Chips**, nicht am DC-DC-Ausgang
  (Datenblatt-Fussnote: *"applicable at the pins of the chip, not the output of
  the DC-DC"*). Bei realem IR-Drop muss der Konverter **etwas hoeher** liefern.

**B) MP8756 (MPS) - VDDCPU_A-Regler, Rev 1.1 (2017-04-13), Electrical
Characteristics, S. 4-5:**
> `Feedback voltage V_REF  594 (min) 600 (typ) 606 (max) mV`
> `Output voltage (V_OUT)  0.6V to 5.5V` / `Output Adjustable from 0.6V`
> `Minimum on time T_ON Min  50 ns`

Der Baustein kann per FB-Teiler bis **0.6 V** herunterregeln - also **unter** die
680-mV-Grenze. Sein `V_REF = 600 mV +/-1%` ist die absolute Untergrenze des
Wandlers.

**C) Silergy SY8120B1ABC - VDDCPU_B-Regler (Datenblatt S. 1-2):**
> `V_FB FB Pin Voltage  588 (min) 600 (typ) 612 (max) mV` (2% Referenz)
> `V_OUT = 0.6*(1+R1/R2)`; `D_max Maximum Duty Cycle (V_FB=0V)  90 %`

Ebenfalls **0.6 V** als untere FB-Referenz.

**Konsequenz (neu):** Der Chip erlaubt **0.681 V**, die beiden Konverter koennen
bis **0.600 V** regeln. Die 680-mV-DT-Grenze ist damit **die Silizium-Untergrenze
- keine DC-DC- und keine Softwaregrenze.** Die DC-DC-Bauteile koennten tiefer,
duerfen aber nicht, weil die **CPU** unter 0.681 V nicht spezifiziert ist. Damit
ist die Grenze **keine "blosse Softwarezahl"**, sondern (auf 1 mV gerundet) die
**Hersteller-Untergrenze des SoC**.

Der N2+ (S922X Rev. C) hat den Bereich gegenueber dem N2 (721000 uV) nach unten
auf das Datenblatt-Minimum erweitert - konsistent mit "rev. C supports slightly
higher voltages".

## 3b. Schaltplan Rev 0.6 20210121 (Board-Revision) - **neu**

Das Board ist **Rev 0.6 20210121**. Der Plan ist ueber das Web-Archive
beschaffbar (`dn.odroid.com/S922X/ODROID-N2/Schematic/odroid-n2_rev0.6_20210121.pdf`,
Snapshot 2023-02-27). Die VDDCPU-Regler liegen auf **Sheet 16 "PWR DC/DC OTHERS"**
(nicht S.12; S.12 ist `S905D2_MIPI`). Damit ist Punkt 17 der Quellenliste
**ueberholt**.

**Regler-Zuordnung (aus dem Plan):**

| Ref | Bauteil | Rail |
|-----|---------|------|
| **U15** | **MP8756GD** | **VDDCPU_A** |
| **U17** | **SY8120B1ABC** | **VDDCPU_B** |
| U14 | MP8756GD | VDD_EE (eigene Rail) |

Beide CPU-Rails sind **PWM-gesteuert** (Netze `VDDCPUA_PWM` / `VDDCPUB_PWM`),
also genau der invertierte Continuous-Mode-Regler aus Kapitel 2.

**FB-Beschaltung VDDCPU_A (U15):** R73=**3K6**, R74=**75K**, R76=**10k**,
R77=**360k**, R78=**47k**, R81=**127k**, R71=499k, R72=499R, C189=1nF,
C176=220pF.
**FB-Beschaltung VDDCPU_B (U17):** R84=**360k**, R85=**47k**, R83=**10k**,
R82=**68k**, R87=**127k**, C190=27pF, C193=1nF.

**Wichtigster Befund:** Beide Rails nutzen **dasselbe Teilerpaar 360k/47k** und
dieselbe PWM-Injektionsbeschaltung (127k + 10k + 1nF). Das ist die
Hardware-Bestaetigung dafuer, dass der DT fuer **beide** Cluster dieselbe
Kalibrierung (`680000..1040000`, 1500 ns) verwendet - die Rev.-0.6-Bestueckung
traegt also auf **beiden** CPU-Rails bis 680 mV mit.

**Grenze der Auswertung (ehrlich):** Die **exakte Uebertragungsfunktion
Tastgrad -> Vout** liess sich aus dem PDF **nicht** rekonstruieren. Der Grund:
das FB-Netz ist eine **PWM-Injektion in den Teiler** (nicht ein einfacher
Teiler), und die Drahtendpunkte treffen die Bauteilsymbole im Vektorlayer nicht
punktgenau. Eine naive Rechnung `0,6*(1+360k/47k) = 5,2 V` ist daher
**unzulaessig** - sie ignoriert den PWM-Knoten und die Parallelzweige. Der
Schaltplan **bestaetigt die Werte und die Topologie**, liefert aber **nicht** die
Spannung bei Tastgrad 0. Diese Groesse bleibt - wie in Kapitel 8/9 dargelegt -
nur messtechnisch zu bestimmen.

**Ein N2±spezifischer Plan ist nicht archiviert** (nur `odroid-n2_*` bis rev0.6).
Der Rev-0.6-Plan ist der des Boards; N2 und N2+ teilen ihn.

## 3c. Software-Workarounds: geprueft und verworfen

Frage: gibt es einen reinen Softwareweg unter die 680 mV? **Nein.**

| Software-Move | Erreicht tiefer? | Grund |
|---|---|---|
| `regulator-min-microvolt` senken (z. B. 600000) | **Nein** | Benennt den Boden nur um; der Tastgrad bleibt bei 100 % gedeckelt. Die reale Spannung ist identisch. |
| Voltage-Table mit Tastgrad > 100 | **Nein** | `pwm.h:258` lehnt `duty > unit` ab; physisch ist der Tastgrad bei der Periode zu Ende. |
| Klemme in `core.c:454` patchen | **Nein** | Der Treiber kann trotzdem keinen kleineren PWM-Wert erzeugen - er klemmt auf 100 % (gleiche Spannung). |
| PWM-Periode/Frequenz aendern | **Nein** | Der Gleichanteil eines PWM ist `Tastgrad * Vcc`, frequenzunabhaengig. Am Board gemessen: Periode 999..15984 -> Spannung im Rauschen (Kapitel "PWM-Periode" in `LOWFREQ-README.md`). |
| `pwm-dutycycle-unit` feiner stellen | **Nein** | Aendert nur die Aufloesung (3,6-mV-Raster), nicht den Anschlag. |

Kern: der Boden 680 mV liegt **genau auf dem physikalischen Extremwert** des PWM
(LOW-Feld 0, Tastgrad 100). Ein Softwareeingriff kann nur *bis* zu diesem Extrem,
nie darueber hinaus.

**Gefaehrlicher Schein-Workaround:** `regulator-min-microvolt` auf z. B. 600 mV
setzen. Dann rechnet `cpufreq` mit "600 mV verfuegbar" und waehlt **hoehere
Frequenzen bei angeblich niedriger Spannung** - die Hardware bleibt aber bei
680 mV stehen. Ergebnis: **entfernte Sicherheitsmarge**, kein Minderverbrauch.
Das Datenblatt fordert ausdruecklich Mindestwert + 20 mV (>= 0,701 V).

## 3d. Hardware-Eingriffe: Aufwand und Bewertung

Ein Hardware-Eingriff ist der **einzige** Weg unter 680 mV - und damit per
Definition **ausserhalb der Spezifikation**.

| # | Eingriff | Aufwand | Wirkung | Bewertung |
|---|---|---|---|---|
| 1 | Widerstand **parallel zum oberen FB-Zweig** | **am einfachsten** (1 Bauteil, nichts ausloeten, reversibel) | senkt die ganze Kennlinie | ausserhalb Spec |
| 2 | Oberen FB-Widerstand tauschen | einfach (1 SMD) | dito | ausserhalb Spec |
| 3 | PWM-Injektionswiderstand (R81/R87 = 127k) aendern | einfach | aendert den PWM-Hub, nicht den Boden | begrenzt |
| 4 | Regler tauschen (andere Vref) | schwer | volle Freiheit | unverhaeltnismaessig |
| 5 | IR-Drop verbessern (Kupfer, Caps) | mittel | holt Marge, nicht Spannung | nutzlos fuer das Ziel |

**Warum #1 der einfachste ist:** `Vout = Vref * (1 + Rtop/Rbot)`. Tiefer geht es
nur ueber kleineres `Rtop` oder groesseres `Rbot`; ein **zusaetzlicher paralleler
Widerstand ueber den oberen Zweig** senkt `Rtop` wirksam, ohne dass etwas
ausgeloetet werden muss.

**Warum trotzdem nicht empfohlen:**

- **Die ganze Kurve verschiebt sich**, nicht nur der Boden. Ein Mod verlangt eine
  **komplette Neukalibrierung der OPP-Tabelle** (Kapitel 3e), sonst wird oben
  untervoltet - genau das fuehrte am Board schon zu einem Hang (s. u.).
- **Unter 0,681 V** liegt man ausserhalb der SoC-Spezifikation. Der Fehlermodus
  ist **stille Bitfehler im Datenpfad**, keine saubere Abschaltung.
- Das FB-Netz ist eine **PWM-Injektion**, kein einfacher Teiler. Welcher
  Widerstand genau der obere Zweig ist, liess sich aus dem PDF **nicht** eindeutig
  festnageln (Kapitel 3b) - ein Mod waere Blindflug plus Messtisch.

Die einzige fachlich vertretbare Hardware-Richtung zeigt **nach oben**: den Boden
von 680 mV auf >= 701 mV **anheben** (Datenblatt-Marge). Das kostet minimal
Leistung und kauft Sicherheit - lohnt aber kaum, weil 680 mV nominal nur 1 mV
unter der Spec liegt.

## 3e. Was bereits am Board gemessen und eingebaut wurde

Diese Analyse steht **nicht** allein; am Board wurde 2026-10-17 bereits real
gemessen und umgesetzt (Details: `LOWFREQ-README.md`, `UV-README.md`).

**Eingebaut und reboot-fest:**

- **Tief-Takt-Overlay** (`lowfreq.dts`): 14 neue OPP-Punkte je Cluster unter
  1 GHz (15.625 MHz ... 666.666 MHz), per `user_overlays` in `armbianEnv.txt`.
  Nur Teiler der Form `2^a*5^b` (der Meson-Taktgeber kennt nichts anderes; andere
  Raten werden still abgerundet und `cpufreq` melde die falsche Rate).
- **`cpu-freq-guard.service`**: setzt nach `sysinit` Untergrenze + `schedutil`
  (gewinnt gegen Armbians `MIN_SPEED`; Reihenfolge im Journal verifiziert).
- **`cpu-hotplug.service`**: nimmt ungenutzte Kerne im Leerlauf offline
  (Stufen + 6-Messungen-Hysterese, `plan_online()` host-getestet). Hintergrund:
  `cpuidle`-Treiber ist `none` (kein DT-`idle-states`) - Kerne schlafen nicht,
  nur der Takt faellt.
- **Sicherheitsnetz**: `cpufreq.default_governor=performance` als Boot-Arg,
  Hardware-Watchdog `RuntimeWatchdogSec=60`, `kernel.panic=10`,
  `watchdog-guard.timer`.

**Gemessen (alle Punkte unter Last, Rechenergebnisse per SHA-256 geprueft):**

- Alle 14 neuen Raten je Cluster: **stabil, 0 Rechenfehler**.
- Undervolt-Spielraum der Tief-Takte: **big -50 mV, little -83 mV bis zum
  Regler-Boden 680 mV** (dort 0 Fehler, auch im 60-s-Soak).
- Hohe Takte: echte **Silizium-Grenze**, steigt mit dem Takt; 1.908 MHz hing bei
  ~700 mV (Watchdog-Reboot). Undervolt muss also **frequenzabhaengig** sein.
- PWM-**Periode** als Hebel: **widerlegt** (nur Schaltfrequenz, nicht Spannung).
- Leistungseffekt ehrlich: **nur der Idle-Anteil** ist belastbar (schedutil
  waehlt 250 MHz statt 1 GHz). Unter Volllast ~1 °C, im Rauschen. Kein
  Stromsensor am Board, Temperatur ist nur ein Proxy.

**Noch NICHT eingebaut:**

- Das **Undervolt steckt nicht** in der committeten `lowfreq.dts`: die Punkte
  tragen die **Werksspannung** (little 761000, big 731000 uV). Der per
  `gen_lowfreq_dts.py --big-mv/--little-mv` erzeugte Undervolt wurde **gemessen**,
  aber **nicht** in die Datei uebernommen. Der Generator lehnt Werte unter
  680 mV ab; empfohlen sind >= 695 mV (Reserve gegen Boot-Loop).
- Kein abschliessender **Dauer-Soak** ueber alle Takte mit eingebautem Undervolt.

## 4. Voltage-Table-Modus: umgeht er die Grenze?
Im Table-Modus (`pwm-regulator.c:72-95` `pwm_regulator_set_voltage_sel()`) ist
`pwm_set_relative_duty_cycle(&pstate, duty_cycle_table[sel].dutycycle, 100)` (Z. 90)
gesetzt - der Tabellen-Tastgrad ist **direkt** der relative Wert (Prozent).

- `.map_voltage = regulator_map_voltage_iterate` (`pwm-regulator.c:242`): waehlt
  nur **vorhandene** Tabelleneintraege; kein Eintrag -> `-EINVAL`, **kein Klemmen**.
- `regulator-min/max-microvolt` wirken im Table-Modus **nur als Zulassigkeits-
  klemme** (`core.c:1273`, `3740`), **nicht** als Abbildung.
- **Entscheidend:** Der kleinste gueltige Tabellen-Tastgrad ist 0
  (`pwm.h:258` lehnt `duty > 100` ab, negative Werte existieren nicht). Bei
  Tastgrad 0 ist das LOW-Feld aber `cnt` - also **maximal**, nicht minimal. Ein
  Tabelleneintrag mit *noch kleinerem* LOW-Feld gibt es strukturell nicht.
- Ein Tabellenwert wie `(600000, 100)` wuerde nominal 600 mV behaupten, aber
  erzeugt **denselben Registerwert (LOW=0)** wie `(680000, 100)` - dieselbe
  "Boden umbenennen"-Falle.

**Fazit Kapitel 4:** Der Table-Modus **entkoppelt** zwar die Kalibrierung von
`regulator-min/max` (nuetzlich, um einzelne OPP-Punkte unabhaengig zu setzen),
**durchbricht aber nicht die `lo=0`-Untergrenze**. Er kann keinen Registerwert
unter 0 erzeugen. (Hypothese: Ein Table-Modus mit *echter* Hardware-Stufe
zwischen 680 und 1040 mV koennte die 3,6-mV-Quantisierung des Prozent-Rasters
verbessern - das ist aber eine **Auflosungs**-, keine **Bodens**-Frage.)

## 5. ADC-Messung: was sie belegt und was nicht

Messpfad: SARADC `meson-g12a-saradc` (`drivers/iio/adc/meson_saradc.c`),
IIO-Kanal **ch4** (physischer Kanal `NUM_CHAN_4`).

- **Kanalzuordnung:** ch8..12 sind die internen MUX-Kanaele `NUM_MUX_0_VSS`,
  `_1_VDD_DIV4`, `_2_VDD_DIV2`, `_3_VDD_MUL3_DIV4`, `_4_VDD`
  (`meson_saradc.c:296-300`). Die Selbstkalibrierung nutzt VDD_DIV4 und
  VDD_MUL3_DIV4 (`meson_saradc.c:1146-1171`, `nominal0 = 2^res/4`,
  `nominal1 = 2^res*3/4`). **ch4 selbst ist ein externer Eingang**; dass er
  VDDCPU_A folgt, ist eine **empirische Repo-Feststellung**, keine dokumentierte
  Schaltungszusicherung.
- **Aufloesung/Referenz:** 12 Bit (`meson_saradc.c:1267`), `vref_voltage = 1`
  (Z. 1257, `VREF_VDDA`) -> ~1,8 V Vollausschlag, `scale = 0.439453125 mV/LSB`.
- **Mittelung:** `read_raw()` nutzt `MEAN_AVERAGING, EIGHT_SAMPLES`
  (`meson_saradc.c:689`) und **leert vorher das FIFO** (Z. 651). Die angezeigten
  `*_mean_raw`-Kanaele sind also das **Mittel ueber 8 Wandlungen**.
- **Konsequenz fuer die Aussagekraft:** Ein solcher Mittelwert erfasst **keine
  kurzfristigen Einbrueche** und mittelt die **PWM-Restwelligkeit** (667 kHz)
  nicht zuverlaessig weg - die reale Spannung schwankt pro PWM-Periode. Die
  gemessenen ~722 mV bei Tastgrad 0 sind daher eine **obere Schranke** der
  mittleren Ausgangsspannung, nicht die Spitzen-/Minimalspannung.
- **Wiederholbarkeit:** In `LOWFREQ-README.md` streuen die Tastgrad-0-Werte
  zwischen **720,3 und 733,9 mV** (5 Messreihen) - also **~14 mV Spanne**. Das
  ist **groesser** als die reklamierte Restspanne zum DT-Boden. Die Werte belegen
  damit **einen Boden**, aber **nicht** dessen exakte Hoehe.
- **VDDCPU_B:** Es gibt **keinen** unabhaengigen Softwarekanal, der
  nachweislich VDDCPU_B folgt (im Repo ist ch5 konstant 907 mV). Fuer den
  little-Cluster existiert **keine** ADC-Verifikation - nur die Regler-Klemme.

## 6. Clustervergleich VDDCPU_A vs. VDDCPU_B

| | VDDCPU_A (big, A73) | VDDCPU_B (little, A53) |
|---|---|---|
| Regler-Knoten | `regulator-vddcpu-a` | `regulator-vddcpu-b` |
| Bauteil (DT-Kommentar) | MP8756GD | Silergy SY8120B1ABC |
| PWM | `pwm_ab` Kanal 0, 1500 ns | `pwm_AO_cd` Kanal 1, 1500 ns |
| min/max | 680000 / 1040000 | 680000 / 1040000 |
| Periode (Register, Repo) | 999 | 333 |
| ADC-Kanal | ch4 (empirisch) | **keiner** |
| Gemessener Boden | ~722-734 mV | nicht messbar |

Beide Cluster haben **dieselbe Software-Klemme** und **denselben `lo=0`-Boden**.
Der Unterschied ist die **Periode** (999 vs. 333) -> Registerauflosung 0,36 mV
(big) vs. 1,08 mV (little). Der little-Cluster hat **keine** Messbestaetigung;
jede Aussage ueber seine reale Untergrenze ist **Hypothese**.

## 7. Grenzentabelle

| Mechanismus | Beleg | Verhindert niedrigere Spannung? | Softwareseitig aenderbar? | Risiko/Unsicherheit |
|---|---|---|---|---|
| `regulator-min-microvolt = 680000` | `-n2-plus.dts:19,26`; `core.c:454` | **Ja** (klemmt Anforderungen nach oben) | Ja (DT/Overlay) - **aber wirkungslos** (s. u.) | Kein Gewinn, siehe naechste Zeile |
| `lo = 0`-Boden des meson-Registers | `pwm-meson.c:205-207,241` | **Ja** (kein Registerwert < 0) | **Nein** | Harte Abbildungsgrenze |
| Invertierte Abbildung `<100 0>` | `-odroid.dtsi:149,168`; `pwm-regulator.c:223` | Nur als Definition des Bodens | Ja, aber verschiebt alle Punkte | Kalibrierfehler, wenn willkuerlich |
| Prozent-Raster (`unit=100`) | `pwm-regulator.c:302`; `pwm.h:258` | Nein (nur 3,6-mV-Auflosung) | Ja (`pwm-dutycycle-unit`) | Feineres Raster moeglich, **Boden bleibt** |
| Voltage-Table-Modus | `pwm-regulator.c:90,242` | **Nein** (entkoppelt nur Kalibrierung) | Ja | Kein `lo<0`; Zwischenwerte entfallen |
| OPP-Spannungen | `meson-g12b-a311d.dtsi` | Nur obere Punkte | Ja (Overlay) | Werte unter 680 mV klemmen auf `lo=0` |
| `cpu-freq-guard` / `cpufrequtils` | `remote-shell/ha/cpu-freq-guard.sh` | **Nein** (nur Frequenz) | Ja | Frequenz-, nicht Spannungsgrenze |
| ADC-Mittelung (8 Samples, FIFO-Clear) | `meson_saradc.c:689,651` | **Nein** (Messartefakt) | Nein | Verzerrt gemessenen Boden nach oben |
| **SoC-Minimum VDDCPU 0.681 V** | **S922X-Datenblatt Sec. 4.2, S. 48** | **Ja (Hardware)** | **Nein** (Silizium) | Untergrenze des Chips; Anwendungsempfehlung >= 0.701 V |
| DC-DC-Regler (MP8756GD / SY8120B1ABC) | Datenblaetter: V_REF/V_FB = 0.600 V | Nein (koennten bis 0.600 V) | - | Nicht der begrenzende Faktor |

## 8. Messbewertung

- Die ADC-Werte **belegen einen Boden** (Tastgrad 0 ist reproduzierbar der
  tiefste Punkt) und **bestatigen die Regler-Klemme** bei 680 mV nominal.
- Sie **belegen nicht**, dass die *reale* Ausgangsspannung bei 680 mV liegt:
  gemessen ~722-734 mV, Streuung ~14 mV, und die 8-Sample-Mittelung mittelt die
  PWM-Welligkeit nicht weg. Der DT-Wert ist damit **konservativ**, der reale
  Boden liegt **darueber** - aber **wie weit**, ist aus den vorhandenen Daten
  **nicht** belastbar ableitbar.
- Fuer VDDCPU_B existiert **keine** Messung.
- Mit dem Datenblatt (Kapitel 3a) ist der gemessene Boden von ~722 mV **erwartbar
  zu hoch**: das Datenblatt spezifiziert VDDCPU **an den Chip-Pins**, der
  DC-DC-Ausgang muss den IR-Drop dazwischen liefern. Der Messwert ist damit
  **konsistent** mit einem 680-mV-Silizium-Minimum, belegt aber nicht dessen
  exakte Hoehe.

## 9. Naechster sicherer Schritt (rein lesend)

Die aussagekraeftigste **nicht-invasive** Pruefung, die noch fehlt, ist eine
**Korrelationsmessung ADC gegen einen unabhängigen Spannungsreferenzpunkt** -
aber ohne Boardzugriff (Vorgabe) bleibt als *naechster* Schritt:

1. **Statische Aufloesung des Registers pruefen:** an einer Kopie des laufenden
   DT/Overlays (`/proc/device-tree`) verifizieren, dass `pwm-dutycycle-unit`
   wirklich Default 100 ist und `pwm-dutycycle-range` exakt `<100 0>` - das
   entscheidet, ob das 3,6-mV-Raster greift (rein lesend, kein Schreiben).
2. **Bauteilkennlinie beschaffen:** Datenblatt MP8756GD / SY8120B1ABC
   (FB-Referenzspannung, min. Ausgang) - damit laesst sich pruefen, ob 680 mV
   bereits die Bauteil-Untergrenze ist. **Ohne dieses Datenblatt bleibt die
   Hardware-/Softwaregrenze nicht unterscheidbar.**
3. Erst danach (mit Freigabe, **nicht** in diesem Auftrag) eine einzige
   kontrollierte Messreihe Tastgrad 0 vs. 1-5, um zu sehen, ob das Register-LOW
   unterhalb des bisherigen Minimums ueberhaupt noch die Spannung senkt.

**Erledigt (Nachtrag 2026-10-17):** Schritt 2 ist mit den drei Datenblaettern
(Kapitel 3a) **abgeschlossen** - die Hardware-/Softwaregrenze ist entschieden.
Schritt 1 bleibt eine optionale, rein lesende Bestaetigung. Schritt 3 ist **nicht
mehr empfohlen**: unter 680 mV laege man ausserhalb der SoC-Spezifikation.

## 10. Einordnung

- **B: Der Treiber erzeugt bereits den kleinsten gueltigen PWM-Wert.** Der
  Register-Boden (`lo=0`) ist nach Quellcode nachgewiesen; eine Softwareaenderung
  innerhalb der vorhandenen Ansteuerung erzeugt **keinen** kleineren Wert.
- **A: Eine softwareseitige Begrenzung ist nachgewiesen** (die 680-mV-Klemme in
  `core.c:454` und `regulator-min-microvolt`), **aber ihre Aufhebung erweitert den
  realen Bereich nicht** - sie benennt den Boden nur um.
- **C: Die Unterscheidung Software- vs. Hardwaregrenze fuer die *reale*
  Ausgangsspannung ist offen.** Die ADC-Daten reichen dafuer nicht
  (Mittelung, ~14 mV Streuung, kein VDDCPU_B-Kanal).
- **Eine Hardwareaenderung ist NICHT bewiesen** und wird hier nicht empfohlen.

**Gesamtfazit:** Innerhalb der bestehenden PWM-Ansteuerung ist das Minimum
erreicht (Klasse **B**). Der einzige verbleibende Hebel waere eine **andere
Hardwarebeschaltung** (andere FB-Kennlinie) - das ist aber **unbewiesen** und
setzt das Datenblatt der genannten Bauteile voraus.

### 10.1 Abschluss nach Datenblatt-Beschaffung (Nachtrag)

Mit den drei Primaerquellen aus Kapitel 3a ist die Frage **entschieden**:

- **Die 680-mV-Grenze ist die Silizium-Untergrenze des S922X** (Datenblatt
  `VDDCPU_A/B = 0.681 - 1.032 V`). Sie ist **keine** Software- und **keine**
  DC-DC-Grenze.
- **Die DC-DC-Konverter koennten tiefer** (beide bis `V_REF = 0.600 V`), aber der
  **SoC ist unter 0.681 V nicht spezifiziert**.
- Damit bleibt es bei **Klasse B** (Treiber am kleinsten gueltigen PWM-Wert) -
  jetzt **zusaetzlich hardware-seitig belegt**: Der Registerboden (`lo=0`)
  entspricht genau der Silizium-Untergrenze. **Zwei unabhaengige Grenzen fallen
  zusammen.**
- Eine **echte Undervolting-Reserve** unter 680 mV existiert nach Datenblatt
  **nicht** - ein Absenken waere Betrieb **ausserhalb der Spezifikation**
  (Risiko: Instabilitaet, Datenverlust, verkuerzte Lebensdauer; das Datenblatt
  empfiehlt fuer die Anwendung sogar **>= 0.701 V** wegen 3% Versorgungsstreuung).
- **Klasse A ist damit ausgeschlossen** (es gibt keinen "versteckten" tieferen
  Softwareboden), **Klasse C ist aufgeloest** (die Hardwaregrenze ist belegt).

## 11. Reproduktion (Host)

```bash
# Reglerkette 1:1 gegen die Repo-Messwerte nachrechnen (kein Board):
python3 /tmp/verify_chain.py
# Quellen (nur lesend geladen):
#   drivers/regulator/pwm-regulator.c, drivers/regulator/core.c,
#   drivers/pwm/pwm-meson.c, include/linux/pwm.h,
#   arch/arm64/boot/dts/amlogic/meson-g12b-odroid-n2-plus.dts,
#   arch/arm64/boot/dts/amlogic/meson-g12b-odroid.dtsi,
#   drivers/iio/adc/meson_saradc.c
```

## 12. Quellen

1. `drivers/regulator/pwm-regulator.c` (Continuous/Table, Zeilen wie zitiert)
2. `drivers/regulator/core.c` (`regulator_check_voltage` 442-457,
   `regulator_set_voltage_unlocked` 4056-4094, `_regulator_do_set_voltage` 110ff.)
3. `include/linux/pwm.h` (`pwm_set/get_relative_duty_cycle` 212-265)
4. `drivers/pwm/pwm-meson.c` (`meson_pwm_calc` 159-222, Register 241-242)
5. `drivers/cpufreq/cpufreq-dt.c` (`dev_pm_opp_set_rate`)
6. `drivers/iio/adc/meson_saradc.c` (Kanaele 287-301, Kalibrierung 1140-1185,
   Mittelung 689, Param g12a 1263-1268)
7. `arch/arm64/boot/dts/amlogic/meson-g12b-odroid-n2-plus.dts` (680000..1040000)
8. `arch/arm64/boot/dts/amlogic/meson-g12b-odroid.dtsi` (Bauteilnamen, `<100 0>`)
9. `arch/arm64/boot/dts/amlogic/meson-g12b-a311d.dtsi` (OPP-Tabellen)
10. Commit `d14734a04a8a` "arm64: dts: meson-g12b-odroid-n2: enable DVFS"
11. Commit `98d24896ee11` "arm64: dts: meson: add support for the ODROID-N2+"
12. Commit `f7d933388f11` "arm64: dts: meson: odroid-n2 plus: fix vddcpu_a pwm"
13. Repo (nur zitiert): `remote-shell/ha/LOWFREQ-README.md`,
    `remote-shell/ha/UV-README.md`, `remote-shell/ha/PWM-REGULATOR-NOTES.md`,
    `remote-shell/ha/pwm_reg_model.py`, `remote-shell/ha/adc_voltage.py`,
    `remote-shell/ha/gen_lowfreq_dts.py`, `remote-shell/ha/cpu-freq-guard.sh`
14. **Amlogic S922X Datasheet Rev 0.2 (2019-03-10)**, Sec. 4.2 Recommended
    Operating Conditions, S. 48: `VDDCPU_A/B = 0.681 - 1.032 V`; Ripple-Spec
    VDDCPU_A/B `40 mV` (Sec. 4.3). Aufdruck "Confidential for Hardkernel!".
    (Beschafft ueber eine oeffentlich gespiegelte Kopie; Original bei Amlogic
    unter NDA.)
15. **Monolithic Power Systems MP8756 Rev 1.1 (2017-04-13)**, Electrical
    Characteristics S. 4-5: `V_REF = 594/600/606 mV`, `V_OUT 0.6V-5.5V`,
    `T_ON Min 50 ns`. (digikey-Spiegel: `mm.digikey.com/.../MP8756.pdf`)
16. **Silergy SY8120B1ABC Datasheet**, S. 1-2: `V_FB = 588/600/612 mV`,
    `V_OUT = 0.6*(1+R1/R2)`, `D_max 90 %`. (digikey-Spiegel:
    `mm.digikey.com/.../5399_SY8120B SOT-23-6.PDF`)
17. **Hardkernel-Schaltplan Rev 0.6 20210121** (Board-Revision), Sheet 16
    `PWR_DCDC_OTHERS.SchDoc`: U15=MP8756GD -> VDDCPU_A, U17=SY8120B1ABC ->
    VDDCPU_B, U14=MP8756GD -> VDD_EE. FB-Werte A: R73 3K6, R74 75K, R76 10k,
    R77 360k, R78 47k, R81 127k; B: R84 360k, R85 47k, R83 10k, R82 68k,
    R87 127k. Beschafft ueber das **Web-Archive** (Snapshot 2023-02-27):
    `http://web.archive.org/web/20230227180615if_/https://dn.odroid.com/S922X/ODROID-N2/Schematic/odroid-n2_rev0.6_20210121.pdf`.
    Direktabruf bei `dn.odroid.com` ist Cloudflare-blockiert; der Wayback-Spiegel
    funktioniert. Siehe Kapitel 3b. Das **FB-Teilerverhaeltnis R1/R2 bzw. die
    exakte PWM-Uebertragungsfunktion bleibt unbekannt** (PWM-Injektion, im
    Vektorlayer nicht punktgenau aufloesbar).

Stand: 2026-10-17. Host-Analyse, kein Boardzugriff.
