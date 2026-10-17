# ODROID-N2: PWM-Regler, Spannungsabbildung und Amlogic-DVFS

Recherche-Notizen zu den beiden `pwm-regulator`-Modi, zum Kalibrier-Problem der
Spannungsabbildung und dazu, wer auf dem Amlogic S922X (meson-g12b) Takt und
Kernspannung tatsaechlich setzt.

> **Geltungsbereich / Vorbehalt.**
> Diese Datei ist **reine Host-Recherche** (Linux-Quelltext, Binding, Upstream-
> Commits, oeffentliche Doku). Es wurde dafuer **kein Board angefasst** und
> **keine neue Messung** gemacht. Alle Zahlen aus Kernel/Device-Tree/Community
> sind als **extern** gekennzeichnet; die wenigen Bezuege auf frueher im Repo
> gemessene Werte verweisen nur auf `UV-README.md` / `LOWFREQ-README.md` und
> erfinden nichts. Wo etwas nur eine **Schlussfolgerung** und keine Messung ist,
> steht das ausdruecklich dabei.

## 1. Kurzfassung

- Der `pwm-regulator` kennt zwei Betriebsarten: **Continuous Voltage** (lineare
  Abbildung aus `regulator-min/max-microvolt` + `pwm-dutycycle-range`) und
  **Voltage Table** (explizite Paare `voltage-table = <uV dutycycle>`). Welche
  greift, entscheidet allein die Anwesenheit der `voltage-table`-Property.
- Im **Continuous-Modus haengen alle Sollwerte an einem einzigen linearen Fit**,
  der an `regulator-min-microvolt` verankert ist. Aendert man die Untergrenze,
  verschiebt sich **jeder** andere Sollwert (Offset *und* Steigung). Das ist das
  hier gemeinte Kalibrier-Problem.
- Eine **`voltage-table` beseitigt genau diese Kopplung**: jeder Punkt hat sein
  eigenes `(uV, duty)`-Paar, `regulator-min/max-microvolt` wirken dann nur noch
  als **Klemmen** (welche Punkte erreichbar sind), nicht mehr als Abbildung.
  Preis: keine Zwischenwerte mehr, und die Tabelle ist weiterhin eine **statische
  Werkskalibrierung** (kein Auto-Abgleich).
- **Wer steuert was:** Auf **G12B (S922X) benutzt der Mainline-Kernel kein
  SCPI-DVFS.** Takt kommt aus dem meson-Clock-Treiber (`cpufreq-dt`), Spannung
  aus dem `pwm-regulator` - **beides setzt Linux selbst** aus der OPP-Tabelle.
  Der Amlogic-**Cortex-M-Co-Prozessor (AO/SCP, `bl30`-Firmware)** und **BL31
  (TF-A)** fassen CPU-DVFS im Mainline-Pfad nicht an. Im **Vendor-BSP (Kernel
  4.9)** dagegen laeuft DVFS ueber genau diesen Co-Prozessor (`meson_cpufreq`).
  Fuer DT-basiertes Undervolting heisst das: **im Mainline-Pfad ist
  `opp-microvolt` im DT autoritativ und wirkt** - was zum Repo-Befund passt, dass
  nur der Overlay-Weg dauerhaft haelt.

## 2. Das Linux-`pwm-regulator`-Binding

Binding: `Documentation/devicetree/bindings/regulator/pwm-regulator.yaml` [1].
Treiber: `drivers/regulator/pwm-regulator.c` [2].

Das Binding beschreibt zwei Modi; die Wahl ist rein deklarativ:

> *"If voltage-table is provided, then the device will be used in Voltage Table
> Mode. If no voltage-table is provided, then the device will be used in
> Continuous Voltage Mode."* [1]

Im Treiber entspricht das genau einer Verzweigung im Probe-Pfad:
`of_property_present(np, "voltage-table")` -> `pwm_regulator_init_table()`, sonst
`pwm_regulator_init_continuous()` [2].

Die ODROID-N2-Nodes nutzen **Continuous Voltage** (keine `voltage-table`) [5].

### 2.1 Continuous Voltage Mode

**Properties** (alle optional, mit Default) [1]:

| Property | Bedeutung | Default |
|---|---|---|
| `pwm-dutycycle-unit` | Ganzzahl, in der `pwm-dutycycle-range` ausgedrueckt ist. `<100>` = Prozent | `100` |
| `pwm-dutycycle-range` | 2 Werte: Tastgrad fuer `regulator-min-microvolt`, dann fuer `regulator-max-microvolt` (in `pwm-dutycycle-unit`) | `<0 100>` |
| `regulator-min-microvolt` / `regulator-max-microvolt` | Spannungsbereich; **beide** gehen in die lineare Abbildung ein | (Pflicht ueber das Regulator-Core) |

Das Binding warnt selbst vor der Modellannahme [1]:

> *"This solution does make an assumption that a %50 duty-cycle value will cause
> the regulator voltage to run at half way between the supplied max_uV and min_uV
> values."*

**Abbildung Wunschspannung -> Tastgrad** (`pwm_regulator_set_voltage`, [2]):

```c
/* diff_duty = |max_uV_duty - min_uV_duty| ; diff_uV = max_uV - min_uV */
dutycycle = DIV_ROUND_CLOSEST_ULL((req_min_uV - min_uV) * diff_duty, diff_uV);
if (max_uV_duty < min_uV_duty)          /* invertierte Logik */
        dutycycle = min_uV_duty - dutycycle;
else
        dutycycle = min_uV_duty + dutycycle;
pwm_set_relative_duty_cycle(&pstate, dutycycle, duty_unit);
```

`pwm_set_relative_duty_cycle()` rechnet den relativen Tastgrad in Nanosekunden um
(`state->duty_cycle = duty * period / unit`) [3]. Der Rueckweg
(`pwm_regulator_get_voltage`) ist die Umkehrung und liefert
`min_uV + voltage * diff_uV / diff_duty` [2].

**Konkret fuer den ODROID-N2+** (extern, aus dem Mainline-DT [6]):

```
regulator-min-microvolt = <680000>;
regulator-max-microvolt = <1040000>;
pwm-dutycycle-range     = <100 0>;     /* invertiert: min_uV -> duty 100, max_uV -> duty 0 */
```

Damit ist `min_uV_duty = 100`, `max_uV_duty = 0` (`max < min` -> invertierte
Logik), `diff_duty = 100`, `diff_uV = 360000` und

```
duty_rel(req) = 100 - (req - 680000) * 100 / 360000
```

Der ODROID-N2 (ohne "+") hat dieselbe Struktur, aber einen engeren Bereich:
`721000..1022000` und `pwms`-Periode `1250` statt `1500` [5]; der N2+ ueberschreibt
beides auf `680000..1040000` / Periode `1500` [6].

### 2.2 Voltage Table Mode

**Property** [1]:

| Property | Bedeutung |
|---|---|
| `voltage-table` | Matrix `uint32` aus Paaren `voltage_uV  dutycycle_percent`; **Tastgrad immer in Prozent** |

Beispiel aus dem Binding [1]:

```
voltage-table = <1114000 0>, <1095000 10>, <1076000 20>,
                <1056000 30>, <1036000 40>, <1016000 50>;
```

**Treiberverhalten** [2]:

- `pwm_regulator_init_table()` liest die Paare, setzt
  `desc.n_voltages = anzahl_paare` und waehlt `pwm_regulator_voltage_table_ops`.
- `.list_voltage` liefert den uV-Wert des Selektors,
  `.set_voltage_sel` schreibt `pwm_set_relative_duty_cycle(..., duty, 100)` -
  also **den Tabellen-Tastgrad direkt**, ohne Umrechnung aus min/max.
- `.map_voltage = regulator_map_voltage_iterate` waehlt beim Wunschwert den
  passenden Tabelleneintrag; Zwischenwerte werden auf den naechsten Eintrag
  gerundet.

Wichtig: In diesem Modus liest der Treiber `regulator-min/max-microvolt`
**nicht** fuer die Tastgrad-Berechnung. Das Binding fasst die Grenzen so zusammen
[1]:

> *"Limitations are that the regulator can only operate at the voltages supplied
> in the table. Intermediary duty-cycle values which would normally allow finer
> grained voltage selection are ignored and rendered useless."*

### 2.3 Beide Modi im Vergleich

| | Continuous Voltage | Voltage Table |
|---|---|---|
| Aktivierung | keine `voltage-table` | `voltage-table` vorhanden |
| Tastgrad-Einheit | `pwm-dutycycle-unit` (Default 100) | immer Prozent (100) |
| Abbildung | lineare Interpolation aus `regulator-min/max` + `pwm-dutycycle-range` | explizites `(uV, duty)`-Paar je Punkt |
| `regulator-min/max` | **definieren** die Abbildung | wirken nur als **Klemme** (erreichbare Selektoren) |
| Aufloesung | feiner (theoretisch beliebige Zwischenwerte) | grob (nur Tabellenpunkte) |
| `get_voltage` liefert | aus dem aktuellen Tastgrad zurueckgerechnet | den Nominalwert des Selektors |

### 2.4 Vom relativen Tastgrad ins Register (meson-PWM)

Der Treiber setzt den relativen Tastgrad; die eigentliche Hardware-Registerform
schreibt `drivers/pwm/pwm-meson.c` [4]. Dort gilt (Auszug):

```c
if (duty == period) { channel->hi = cnt; channel->lo = 0; }        /* 100 % high */
else if (duty == 0) { channel->hi = 0;   channel->lo = cnt; }      /*   0 % high */
else { channel->hi = duty_cnt; channel->lo = cnt - duty_cnt; }
...
value = FIELD_PREP(PWM_HIGH_MASK, channel->hi) |   /* Bits 31..16 */
        FIELD_PREP(PWM_LOW_MASK,  channel->lo);    /* Bits 15..0  */
```

Der Registerinhalt ist also `((cnt - duty_cnt) << 16) | duty_cnt`. Das passt zur
im Repo (nicht hier) gemessenen Form `((period - duty) << 16) | duty`
(`UV-README.md`): das dortige `duty` ist das **Low-Feld** (`lo`). Wegen
`pwm-dutycycle-range = <100 0>` gilt: relativer Tastgrad `100` (= `min_uV`,
680 mV) -> `hi = cnt, lo = 0` -> Repo-`duty = 0`; relativer Tastgrad `0`
(= `max_uV`, 1040 mV) -> `lo = cnt` -> Repo-`duty = period`. Beide Konventionen
sind konsistent; die scheinbare "Inversion" ist die invertierte Logik der
ODROID-Nodes.

> **Hinweis zur Periode.** Die dritte `pwms`-Zelle (N2+: `1500`, N2: `1250`) ist
> eine **Anforderung in Nanosekunden**. Der Treiber rundet sie auf einen mit dem
> PWM-Quelltakt erreichbaren Zaehlerwert (`cnt`) [4]. Die rohe **Zaehlerperiode**
> im Register ist damit nicht identisch mit dem DT-Wert (im Repo wurden z. B.
> 999 / 333 gezaehlt). Das ist kein Widerspruch, sondern erwartete Rundung.

## 3. Beseitigt eine `voltage-table` das Kalibrier-Problem?

**Frage.** Verschiebt eine Aenderung von `regulator-min-microvolt` jeden anderen
Sollwert? Und hilft eine `voltage-table` dagegen?

**Antwort: Im Continuous-Modus ja (es verschiebt), und die `voltage-table`
beseitigt genau diese Kopplung - mit Einschraenkungen.**

Begruendung, direkt aus [2]:

1. **Continuous-Modus ist ein einziger linearer Fit.** Der Tastgrad ist
   `duty_rel(req) = min_uV_duty + (req - min_uV) * diff_duty / diff_uV`. Sowohl
   der **Anker** (`min_uV`) als auch die **Steigung** (`diff_duty/diff_uV`) haengen
   an `regulator-min/max-microvolt`. Setzt man nur `regulator-min-microvolt`
   herab, aendert sich `min_uV` **und** `diff_uV` -> jede Zielspannung bekommt
   einen anderen Tastgrad. Man kann also nicht einen Punkt "nachkalibrieren",
   ohne alle anderen mitzuverschieben. Das ist der Kern des Problems.
   (Konkret ODROID-N2+ mit `680000..1040000`: eine Verschiebung der Untergrenze
   aendert Offset und Steigung der Geraden.)

2. **Voltage-Table-Modus entkoppelt die Punkte.** Hier liest
   `pwm_regulator_init_table()` die `regulator-min/max`-Werte **gar nicht**; jeder
   Selektor traegt sein eigenes `(uV, duty)`-Paar, und `set_voltage_sel` schreibt
   den Tabellen-Tastgrad unveraendert [2]. Eine Aenderung von
   `regulator-min-microvolt` aendert dann **nur**, welche Tabellenpunkte ueberhaupt
   erreichbar sind (Klemme durch das Regulator-Core via
   `regulator_map_voltage_iterate`), **nicht** den Tastgrad der verbleibenden
   Punkte. Das Kalibrier-Problem ist damit weg.

**Einschraenkungen (wichtig, damit die Antwort nicht zu optimistisch klingt):**

- Die `voltage-table` ist weiterhin eine **statische Werkskalibrierung**. Sie
  kalibriert sich nicht selbst; die `(uV, duty)`-Paare muessen pro Board
  (Streuung) ermittelt und eingetragen werden.
- **Keine Zwischenwerte.** Nur die Tabellenpunkte sind anwaehlbar;
  Zwischenwerte werden gerundet und sind laut Binding *"rendered useless"* [1].
  Fuer DVFS mit vielen OPP-Punkten braucht man entsprechend viele Zeilen.
- **Klemmen-Wirkung bleibt.** Hebt man `regulator-min-microvolt` an, fallen tiefe
  Tabellenpunkte weg; senkt man es, sind sie wieder erreichbar. Die
  *Abbildung* ist entkoppelt, die *Zulaessigkeit* nicht.
- **`get_voltage` luegt im Tabellen-Modus nicht ueber die Spannung, aber liefert
  den Nominalwert** des getroffenen Selektors (nicht die reale Ausgangsspannung);
  das ist derselbe "Cache"-Effekt, den `UV-README.md` fuer den Sysfs-Pfad
  beschreibt.

**Fazit.** Fuer ein Undervolt-Projekt, das einzelne Frequenzpunkte gezielt
absenken will, ist der **Voltage-Table-Modus das passende Werkzeug**, weil er die
lineare Kopplung an `regulator-min/max` aufloest. Der Continuous-Modus (so wie
ODROID-N2(+) ihn nutzt) kann das strukturell nicht.

## 4. Amlogic-meson-DVFS: wer steuert Spannung und Takt wirklich?

### 4.1 Hardware-Hintergrund

Amlogic-SoCs enthalten einen kleinen **Co-Prozessor** fuer Power/Clock. In der
Community-Doku ist er als **Cortex-M3 "SCP"** beschrieben, auf dem eine
**proprietaere `bl30.bin`-Firmware** laeuft [18]. U-Boot dokumentiert, dass
`bl30_new.bin` aus **`bl30.bin`** (SCP-Firmware) und **`bl301.bin`** besteht, wobei
`bl301.bin` ein **board-spezifisches Plug-in fuer DVFS/Suspend-Resume-Parameter**
ist [17].

### 4.2 Zwei Wege: SCPI (alt) vs. cpufreq-dt (G12B-Mainline)

**a) SCPI-DVFS (GXBB/GXL/GXM, S905/S912).** Auf diesen aelteren SoCs laeuft DVFS
ueber **SCPI**: Der Kernel-Treiber `drivers/cpufreq/scpi-cpufreq.c` [13] holt die
OPP-Punkte per `scpi_ops->add_opps_to_device()` **von der Firmware** und setzt den
Takt per `clk_set_rate()`. Die Firmware-Seite ist der Co-Prozessor. Die Community
dokumentiert, dass diese Firmware auf Nicht-Hardkernel-Boards Takte **ignoriert
und gefakte Werte zurueckmeldet** ("Amlogic still cheating with clockspeeds") [18].
Das ist die historische Quelle des Misstrauens gegenueber Amlogic-DVFS.

**b) G12B-Mainline benutzt SCPI NICHT.** Im aktuellen Mainline-DT fuer
G12A/G12B (`arch/arm64/boot/dts/amlogic/meson-g12-common.dtsi`) gibt es **keinen
`scpi`-Node** (nur `secure-monitor = "amlogic,meson-gxbb-sm"`). Stattdessen:

- Der **Takt** kommt aus dem meson-Clock-Controller (CCF). Die CPU-Clock-Baeume
  von G12A/G12B wurden in v5.4 **mutable** gemacht, d. h. `clk_set_rate()` aus
  Linux funktioniert [19].
- Die **OPP-Tabelle** steht im DT (`opp-table-0`/`opp-table-1` in
  `meson-g12b-s922x.dtsi` [7] bzw. `meson-g12b-a311d.dtsi` [8]) und wurde laut
  Commit aus dem **Vendor-BSP** uebernommen [10].
- Die **Spannung** liefert der `pwm-regulator` (`vddcpu_a`/`vddcpu_b`) [5][6].

Der generische `cpufreq-dt`-Treiber koppelt OPP-Frequenz und `opp-microvolt` und
schreibt die Spannung ueber den Regulator. **Im Mainline-Pfad setzt Linux also
Frequenz *und* Spannung selbst** - der AO-Co-Prozessor ist daran nicht beteiligt.

**c) Der Secure Monitor kann kein CPU-DVFS.** Der Mainline-Treiber
`drivers/firmware/meson/meson_sm.c` [14] spricht BL31/den Secure Monitor per SMC
an. Die verfuegbaren Kommandos sind ausschliesslich
`SM_EFUSE_READ/WRITE/USER_MAX`, `SM_GET_CHIP_ID`, `SM_THERMAL_CALIB_READ` und
`SM_A1_PWRC_SET/GET` [15] - **kein** DVFS-Kommando.

**d) BL31 (TF-A).** Die TF-A-Ports fuer Meson (GXBB/AXG) liefern **PSCI** (CPU_ON/
OFF, RESET, OFF) und **SCPI**-Support, aber **kein** CPU-DVFS [16]. Auf dem
Mainline-G12B-Pfad ist DVFS Sache des Linux-Kerns.

### 4.3 Vendor-BSP (Kernel 4.9) - der andere Weg

Im Hardkernel/Amlogic-4.9-BSP laeuft DVFS ueber einen eigenen Treiber, der den
AO-Co-Prozessor anspricht. Das zeigt sich in Vendor-dmesg-Zeilen wie
`meson_cpufreq_init: ...` und `dvfs [meson_cpufreq_init] - cluster 0 freq 2016000`
(oeffentlich in CoreELEC/DietPi-Logs zitiert, extern). Die Vendor-OPP-Tabelle
(`arch/arm64/boot/dts/amlogic/meson64_odroidn2.dts` im Hardkernel-Tree [20])
enthaelt die **Sub-1-GHz-Punkte** (100/250/500/667 MHz) mit `opp-microvolt =
<731000>`, die Mainline spaeter wieder **entfernt** hat (siehe 4.4).

### 4.4 Was das fuer DT-basiertes Undervolting heisst

- **Mainline: DT ist autoritativ.** Weil Linux die Spannung aus `opp-microvolt`
  ueber den `pwm-regulator` programmiert, aendert eine Anpassung von
  `opp-microvolt` (bzw. ein Overlay) real die Ausgangsspannung. Das deckt sich
  mit dem Repo-Befund, dass nur der Overlay-Weg dauerhaft haelt und Laufzeit-Werte
  von `cpufreq-dt`/Regler-Cache ueberschrieben werden.
- **Es gibt keine Firmware, die den DT-Wert "ueberschreibt"** - im Mainline-Pfad.
  Das ist der entscheidende Unterschied zum SCPI-Zeitalter und zum Vendor-BSP.
- **Vorsicht, Schlussfolgerung (nicht gemessen):** Der AO-Co-Prozessor existiert
  auch auf G12B weiter und laeuft. Auf Pfaden, die Mainline nicht kontrolliert
  (z. B. Suspend/Resume oder ein gebooteter Vendor-Kernel), kann die Firmware
  VDDCPU weiterhin anfassen. Aus dem Fehlen eines DVFS-SM-Kommandos folgt *nicht*
  automatisch, dass die Firmware VDDCPU nie beruehrt - nur, dass **Mainline** sie
  nicht darum bittet. Wer extreme Undervolts fahrt, sollte das im Hinterkopf
  behalten.
- **Sub-1-GHz-Punkte sind offiziell heikel.** Upstream hat per Commit
  `6c4d636bc00d` ("remove CPU opps below 1GHz for G12B boards") [11] die Punkte
  **entfernt**, weil G12B-Geraete beim Heruntertakten im Idle **CPU-Stalls und
  Wedges** zeigten; Vendor-Kernel entfernten 100-250 MHz, andere Distros zusaetzlich
  500/667 MHz. Das ist der Hintergrund fuer den Repo-Weg, diese Punkte per Overlay
  wieder einzufuegen - mit dem dort dokumentierten Vorsichts-/Watchdog-Konzept.

## 5. Vorhandene Hardkernel-/Armbian-/Community-Doku

- **ODROID-Wiki (Hardware):** `wiki.odroid.com/odroid-n2/hardware` [21] - Board-
  Spezifikation, Spannungsversorgung (12 V/2 A empfohlen), Power-Messwerte. Keine
  CPU-VDDCPU-Reglerdetails.
- **Amlogic S922X Datasheet** [22] - SoC-Uebersicht (A73/A53, GPU). Oeffentlich
  einsehbar; fuer die DVFS-Registerdetails des Co-Prozessors nicht ausreichend.
- **Armbian-Forum "Amlogic still cheating with clockspeeds"** [18] - die zentrale
  Community-Quelle zum `bl30`/Cortex-M-DVFS-Verhalten und dazu, dass Hardkernel
  fuer den ODROID-C2 einen "ehrlichen" Blob erhielt. Sinngemaess: proprietäre
  Firmware steuert Takt, ignoriert teils das cpufreq-Framework und meldet gefakte
  Werte.
- **linux-meson.com (Mainlining-Uebersicht)** [19] - belegt u. a. "G12A/G12B:
  make the CPU clock trees mutable" (v5.4) und "G12B: enable CPU DVFS".
- **Upstream-Commits (Mainline) zum ODROID-N2:**
  - `d14734a04a8a` "arm64: dts: meson-g12b-odroid-n2: enable DVFS" [9] - setzt
    Clock/OPP/Supply je Cluster; nennt explizit "0.73V to 1.01V using 2 distinct
    PWM outputs ... with an inverse duty-cycle".
  - `b96d4e92709b` "support a311d and s922x cpu operating points" [10] - OPP-Punkte
    aus dem Vendor-BSP, Aufteilung S922X (N2) / A311D (N2+).
  - `6c4d636bc00d` "remove CPU opps below 1GHz for G12B boards" [11] - Stalls beim
    Heruntertakten (s. o.).
  - `62183863f708` "Fix the pwm regulator supply properties" [12] - bindet
    VDDCPU_A/B an `main_12v` statt an den Dummy-Regler.
- **U-Boot-Doku** [17] - `bl30_new.bin` = `bl30.bin` + `bl301.bin` (DVFS-Plugin).
- **TF-A-Doku** [16] - Meson-Ports: PSCI + SCPI + SIP, kein CPU-DVFS.

## 6. Bezug zu diesem Repo (ohne neue Messwerte)

- Der laufende Kernel (`7.1.8-edge-meson64`, Armbian) ist ein **Mainline**-Kernel
  -> es greift der Pfad aus 4.2b: `cpufreq-dt` + meson-Clock + `pwm-regulator`.
  Das passt zum Repo-Befund, dass die Regler-Sysfs und die OPP-Tabelle read-only
  sind und nur der Overlay-Weg (OPP-Tabelle) dauerhaft wirkt
  (`UV-README.md`, `LOWFREQ-README.md`).
- Die im Repo vermessenen Regler-Grenzen (`680000..1040000`) und die lineare Form
  `V = 680000 + duty * 360000 / period` sind **genau die Continuous-Modus-Abbildung**
  aus Abschnitt 2.1 mit `pwm-dutycycle-range = <100 0>`. Sie decken sich mit den
  externen N2+-DT-Werten [6].
- **Konsequenz fuer kuenftige Arbeit (Schlussfolgerung):** Ein
  `voltage-table`-basierter Regler im Overlay waere der strukturell richtige
  Hebel, um einzelne Frequenzpunkte unabhaengig zu kalibrieren (Abschnitt 3).
  Fuer die vorhandene, rein frequenzabhaengige Undervolt-Strategie
  (`gen_lowfreq_dts.py --big-mv/--little-mv`) aendert sich dadurch nichts
  Grundsaetzliches - der Overlay-Weg bleibt korrekt.
- Die im Repo gefundene **Teiler-Wahrheit** (nur `2^a * 5^b`-Raten sind real,
  `cpu_clk_dyn`-Mux) ist kompatibel mit 4.2b: der Takt kommt aus dem meson-CCF,
  dessen CPU-Mux genau diese Eltern (`xtal`, `fclk_div2`, `fclk_div3`) und einen
  Teiler anbietet [4][19].

## 7. Quellen

1. pwm-regulator DT-Binding (YAML) -
   https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/regulator/pwm-regulator.yaml
   (Spiegel: https://www.kernel.org/doc/Documentation/devicetree/bindings/regulator/pwm-regulator.yaml)
2. pwm-regulator-Treiber -
   https://github.com/torvalds/linux/blob/master/drivers/regulator/pwm-regulator.c
3. PWM-Core, `pwm_get/set_relative_duty_cycle` -
   https://github.com/torvalds/linux/blob/master/include/linux/pwm.h
4. pwm-meson-Treiber (Registerfelder, Quelltakte) -
   https://github.com/torvalds/linux/blob/master/drivers/pwm/pwm-meson.c
5. ODROID-N2/DVFS-Nodes (`vddcpu_a`/`vddcpu_b`, `pwm-dutycycle-range`, `cpu-supply`) -
   https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/amlogic/meson-g12b-odroid.dtsi
6. ODROID-N2+ Overrides (`680000..1040000`, `pwms ... 1500`) -
   https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/amlogic/meson-g12b-odroid-n2-plus.dts
7. S922X-OPP-Tabelle (`meson-g12b-s922x.dtsi`) -
   https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/amlogic/meson-g12b-s922x.dtsi
8. A311D-OPP-Tabelle (N2+ nutzt diese) -
   https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/amlogic/meson-g12b-a311d.dtsi
9. Commit `d14734a04a8a` "enable DVFS" -
   https://github.com/torvalds/linux/commit/d14734a04a8aea793a5534de39a21ad59b324a11
10. Commit `b96d4e92709b` "support a311d and s922x cpu operating points" -
    https://github.com/torvalds/linux/commit/b96d4e92709b5c4a0ab1494be76415a466c9c0c3
11. Commit `6c4d636bc00d` "remove CPU opps below 1GHz for G12B boards" -
    https://github.com/torvalds/linux/commit/6c4d636bc00dc17c63ffb2a73a0da850240e26e3
12. Commit `62183863f708` "Fix the pwm regulator supply properties" -
    https://github.com/torvalds/linux/commit/62183863f708c2464769e0d477c8ce9f3d326feb
13. scpi-cpufreq-Treiber (SCPI-DVFS-Pfad) -
    https://github.com/torvalds/linux/blob/master/drivers/cpufreq/scpi-cpufreq.c
14. Meson Secure-Monitor-Treiber -
    https://github.com/torvalds/linux/blob/master/drivers/firmware/meson/meson_sm.c
15. Meson SM-Kommandoliste -
    https://github.com/torvalds/linux/blob/master/include/linux/firmware/meson/meson_sm.h
16. Trusted Firmware-A, Meson-Ports (SCPI/PSCI, kein DVFS) -
    https://trustedfirmware-a.readthedocs.io/en/latest/plat/meson-gxbb.html
17. U-Boot, Amlogic SPL (`bl30_new.bin` = `bl30.bin` + `bl301.bin`) -
    https://docs.u-boot-project.org/en/latest/board/amlogic/spl.html
18. Armbian-Forum, "Amlogic still cheating with clockspeeds" -
    https://forum.armbian.com/topic/7042-amlogic-still-cheating-with-clockspeeds/
19. linux-meson.com, Mainlining-Uebersicht -
    https://linux-meson.com/mainlining.html
20. Hardkernel-Vendor-Tree (Kernel 4.9, `meson64_odroidn2.dts`, Sub-1-GHz-OPPs) -
    https://github.com/hardkernel/linux
21. ODROID-Wiki, N2-Hardware -
    https://wiki.odroid.com/odroid-n2/hardware
22. Amlogic S922X Datasheet -
    https://www.scs.stanford.edu/~zyedidia/docs/amlogic/s922x.pdf

Stand der Recherche: 2026-10-17. Mainline-Quellen beziehen sich auf
`torvalds/linux` `master` zum genannten Datum.
