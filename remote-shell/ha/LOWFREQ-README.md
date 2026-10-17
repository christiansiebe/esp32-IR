# CPU-Taktraten unter 1 GHz (Odroid N2+)

Die OPP-Tabellen des Werks-DTB (`opp-table-0` = Cortex-A53 "little",
`opp-table-1` = Cortex-A73 "big") beginnen bei 1.0 GHz. `cpufreq` kann deshalb
nichts Langsameres waehlen, obwohl der Taktbaum es koennte. Dieses Verzeichnis
entfernt die Sperre mit einem Device-Tree-Overlay und vermisst die neuen Punkte.

## Warum ein Overlay

- Die OPP-Tabelle ist zur Laufzeit **read-only**
  (`/sys/kernel/debug/opp/*/supply-0/u_volt` ist 0444).
- Ein Sysfs-Schreiben von `scaling_min_freq` auf 500 MHz wird **still auf
  1.0 GHz geklemmt** - der Treiber kennt die Rate nicht.
- `CONFIG_OF_OVERLAY` ist **nicht** gesetzt, ein configfs-Overlay zur Laufzeit
  ist also nicht moeglich. Bleibt: Overlay zur Bootzeit ueber
  `armbianEnv.txt` (`user_overlays=lowfreq`, Ablage in `/boot/overlay-user/`).

Der Boot-Weg ist verifiziert: `boot.cmd` laedt `armbianEnv.txt`, wertet
`user_overlays` aus und laedt `dtb/amlogic/overlay/meson-<name>.dtbo` bzw.
`overlay-user/<name>.dtbo`.

## Wichtig: nur Teiler der Form 2^a * 5^b

Der Meson-Taktgeber teilt `fclk_div2` (1.0 GHz) bzw. `fclk_div3` (666.67 MHz)
mit einem 6-Bit-Teiler, kennt aber nur die Teilerfaktoren **2, 4, 5, 8, 10, 16,
20, 25, 32, 40, 50, 64** - also `2^a * 5^b`. Eine Rate mit Faktor 3 im Teiler
ist nicht exakt darstellbar. Gemessen (2026-10-17):

| angefordert | `scaling_cur_freq` | echtes Signal (`cpu_clk`) |
|---|---|---|
| 333333 kHz | 500000 kHz | 499999993 Hz |
| 33333 kHz | 40000 kHz | 40000000 Hz |

**Korrektur:** Frueher stand hier, `scaling_cur_freq` melde die Wunschrate,
waehrend der echte Takt still abgerundet werde. Das war ein Vergleich mit dem
falschen Signal (`cpu_clk_dyn`, s. oben). Tatsaechlich waehlt `cpufreq` die
**naechstgelegene darstellbare** Rate und `scaling_cur_freq` gibt sie korrekt
wieder (hier aufgerundet: 333333 -> 500000).

Solche Punkte sind in `lowfreq.dts` deshalb **nicht** enthalten: eine
angeforderte Rate, die nicht exakt darstellbar ist, laesst `cpufreq` auf einen
anderen Punkt springen, als die OPP-Tabelle vorsieht - die Spannung passt dann
nicht zur Rate. Die Liste enthaelt nur Raten, die der Taktgeber exakt erzeugt.

Der **echte** Takt steht nur in debugfs, und zwar an `cpu_clk` (little) bzw.
`cpub_clk` (big) - eindeutig erkennbar an der Consumer-Spalte (`cpu0` bzw.
`cpu2`):

    little -> /sys/kernel/debug/clk/cpu_clk/clk_rate
    big    -> /sys/kernel/debug/clk/cpub_clk/clk_rate

**Korrektur (2026-10-17):** Frueher stand hier `cpu_clk_dyn`/`cpub_clk_dyn` als
"der CPU-Takt selbst". Das ist **falsch**: `*_clk_dyn` ist der vom
System-Controller (dvfs) verwaltete Zweig und traegt oft einen **veralteten**
Wert. Gegenprobe mit little=2016 MHz, big=1000 MHz:

| Signal | Consumer | Rate |
|---|---|---|
| `cpu_clk` | `cpu0` | 2016000000 (little, korrekt) |
| `cpub_clk` | `cpu2` | 999999985 (big, korrekt) |
| `cpu_clk_dyn` | `ff63c000.system-controller dvfs` | 249999997 (veraltet) |

**Folge fuer den Monitor:** `armbianmonitor -m` liest `scaling_cur_freq`. Fuer
**alle 50 verfuegbaren Raten** beider Cluster stimmt dieser Wert mit dem echten
Signal (`cpu_clk`/`cpub_clk`) innerhalb **< 1 %** ueberein (groesste Abweichung
0,79 % bei 1512000 kHz -> 1500 MHz, eine PLL-Rundung). Der Monitor zeigt also
den echten Takt; der frueher vermutete Faktor-2-Fehler war ein Vergleich mit
dem falschen Signal (`*_clk_dyn`).

## Dateien

| Datei | Zweck |
|---|---|
| `gen_lowfreq_dts.py` | erzeugt `lowfreq.dts` aus der Raten-/Spannungstabelle |
| `lowfreq.dts` | OPP-Eintraege unter 1 GHz (generiert, nicht von Hand editieren) |
| `deploy_lowfreq.sh` | kompiliert, validiert gegen das laufende DTB, installiert, aktiviert |
| `revert_lowfreq.sh` | nimmt Overlay + Sicherheitsnetz vollstaendig zurueck |
| `cpu-freq-guard.sh` / `.service` | setzt beim Boot die Untergrenze und den Governor |
| `lowfreq_set_floor.sh` | setzt die dauerhafte Untergrenze (`<kHz|auto>`) |
| `lowfreq_probe.py` | prueft jeden Punkt unter Last, mit Rechen- und Takt-Verifikation |
| `lowfreq_clk_truth.sh` | zeigt je Rate den echten Endtakt und die Mux-/Teilerwahl |
| `gov_free_test.sh` | laesst den Governor mit gesenkter Untergrenze selbst waehlen |

## Sicherheitsnetz

Ein ungeeigneter niedriger OPP-Punkt kann das Board haengen. Zwei Stufen
verhindern, dass daraus eine Bootschleife wird:

1. `cpufreq.default_governor=performance` (Boot-Arg) laesst den Kernel mit
   **maximaler** Frequenz booten, bevor es eine Untergrenze gibt.
2. `cpu-freq-guard.service` stellt direkt nach `sysinit` `scaling_min_freq` auf
   1.0 GHz und den Governor auf `schedutil` (Reihenfolge im Journal
   verifiziert: laeuft **nach** `armbian-hardware-optimize.service`, gewinnt
   also gegen Armbians `MIN_SPEED=1000000`).

Die neuen Punkte sind damit nur durch einen **ausdruecklichen** Laufzeit-Test
erreichbar; ein Reset stellt die Untergrenze wieder her. Zusaetzlich haelt der
systemd-Hardware-Watchdog (`RuntimeWatchdogSec=60`) einen echten Hang im Griff.

## Leistung: spart das ueberhaupt Strom?

**Ja, aber begrenzt - und der Grund ist die Spannung.** Das Overlay uebernimmt
fuer alle neuen Punkte die Spannung des 1.0-GHz-Punkts (little 0.762 V, big
0.730 V); die Spannung faellt mit dem Takt **nicht**. Bei CMOS ist
P = C * V^2 * f + P_leck, hier sinkt also nur der f-Anteil.

Gemessen (kein Stromsensor am Board, kein INA2xx - Temperatur als Proxy):

| Zustand | Takt little | Temp |
|---|---|---|
| Volllast, 2 Loops auf cpu0,1, Untergrenze 1.0 GHz | 999 MHz | 37 °C |
| dieselbe Last, Untergrenze 24 MHz | 24 MHz | 36 °C |
| Leerlauf, Untergrenze 1.0 GHz | 1000 MHz | 36 °C |
| Leerlauf, Untergrenze 15.6 MHz (schedutil waehlt 250 MHz) | 250 MHz | 35.7 °C |

Bei gleicher **Dauerlast** unterscheiden sich 1.0 GHz und 24 MHz nur um ~1 °C.
Der Grund: unter Last schaltet der Regler die Spannung auf `VDDCPU_A` = 1.011 V
(1.0 GHz) bzw. 0.950 V (250 MHz) hoch, und die Leckleistung des A73/A53 bei
dieser Spannung dominiert den f-Anteil. Der Nutzen liegt daher nicht im
Volllastbetrieb, sondern **im Leerlauf**: dort waehlt `schedutil` von selbst
250 MHz statt 1.0 GHz und spart den Taktanteil, ohne dass etwas fehlt.

Ehrliche Einordnung: einen echten Leistungsmesser gibt es am Board nicht, die
Temperatur ist nur ein Proxy. Der belastbare Effekt ist der Idle-Anteil; die
~1 °C unter Last liegen im Rauschen.

## Leerlauf: Kerne schlafen nicht (cpuidle fehlt)

`/sys/devices/system/cpu/cpuidle/current_driver` = **`none`**, das DTB enthaelt
**keine** `idle-states`. Die Kerne gehen im Leerlauf also **nicht** in einen
Schlafzustand; nur der Takt faellt. Das ist ein Kernel-/DT-Limit des Meson-G12B
und nicht ohne Risiko aenderbar.

Was stattdessen hilft: **CPU-Hotplug**. `cpu_hotplug.py` + `cpu-hotplug.service`
nehmen ungenutzte Kerne ueber `/sys/devices/system/cpu/cpuN/online` ganz aus dem
Takt. Gemessen im Leerlauf: **nur cpu0 online**, cpu1-5 aus; unter Last holt
der Dienst sie zurueck. Ein offline genommener Kern verbraucht keinen Takt und
keine Leckleistung mehr.

Regeln des Dienstes:
- **cpu0 bleibt immer online** (Boot-CPU; ihr Wieder-Onlinen ist nicht
  garantiert).
- Stufen: Last < 0.5 -> **1 Kern**, < 1.5 -> 2, < 3 -> 4, sonst alle.
- Hysterese: eine Aenderung erst nach 6 Messungen (30 s) mit gleicher Stufe.
- Beim Beenden (SIGTERM) werden alle Kerne wieder online genommen.
- Reine Entscheidung `plan_online()`, host-getestet
  (`test_cpu_hotplug.py`, 134 Checks).

Auf dem Board verifiziert (Journal des Dienstes):

    cpu-hotplug: 6 Kerne, Stufen [(0.5, 1), (1.5, 2), (3.0, 4), (6.0, 6)]
    Last 0.36 -> online [0] (aus: [1, 2, 3, 4, 5])
    Last 0.53 -> online [0, 1] (aus: -, an: [1])
    Last 0.38 -> online [0] (aus: [1], an: -)

Ein-Kern-Betrieb ist auch mit Home Assistant + Docker stabil (getestet): das
Board lief mit `online: 0` weiter, alle Dienste blieben aktiv.

Nicht verwechseln: Hotplug ist kein Schlaf. Ein offline genommener Kern ist
stromlos, aber das Wieder-Onlinen kostet Zeit (Boot der Sekundaer-CPU), deshalb
die Hysterese.

## Wichtig: Peripherie haengt NICHT am CPU-Takt

`mmc` (SD/eMMC), `usb` und `ethernet` haengen an `fclk_div2` (1.0 GHz) bzw.
`fclk_div3` (666.7 MHz), **nicht** am CPU-Mux. Der CPU-Takt ist damit isoliert:
die tiefen Raten verlangsamen weder SD noch USB noch Netz. Die CPU-internen
AXI/ATB/APB-Takte (`cpub_clk_axi/atb/apb`) sind zwar Teiler des CPU-Takts, aber
das ist das vorgesehene Design (der Speichercontroller des SoC laeuft weiter
ueber `dmc`/`fclk_div2`).

## Wichtig: zwei Stellen fuer die Untergrenze

Armbians `/usr/lib/armbian/armbian-hardware-optimization` liest `MIN_SPEED` aus
`/etc/default/cpufrequtils` und setzt `scaling_min_freq` damit **noch nach**
`cpu-freq-guard.service` (das Skript laeuft als Kind weiter, im Journal bei
8.1 s und 9.5 s). Wer nur `/etc/default/cpu-freq-guard` setzt, sieht nach dem
Boot wieder 1.0 GHz. `lowfreq_set_floor.sh` schreibt daher **beide** Dateien.
Das war ein realer Fehler: der Reboot-Test zeigte `min=1000000`, obwohl
`cpu-freq-guard` = 15625 war.

## Undervolting der neuen Tief-Takte (2026-10-17, gemessen)

Die neuen OPP-Punkte tragen die **Spannung des 1.0-GHz-Punkts** (big 730 mV,
little 763 mV), obwohl der Chip bei 24-250 MHz viel weniger braucht. Dort ist
also Undervolt-Spielraum. Gemessen mit `uv_lowfreq.py` (Rechenergebnisse per
SHA-256-Hash geprueft - zu wenig Spannung erzeugt zuerst stille Bitfehler):

| Cluster | Werksspannung | stabil bis | Absenkung |
|---|---|---|---|
| big (A73) | 730 mV | **680 mV** | -50 mV |
| little (A53) | 763 mV | **680 mV** | -83 mV |

Geprueft bei 24 / 50 / 100 / 250 MHz, je Stufe 2 s Volllast (6 Prozesse):
**alle acht Punkte bis zum Regler-Boden 680 mV stabil, 0 Rechenfehler.** Der
Boden ist keine Instabilitaet, sondern das PWM-Limit
(`regulator.{2,8}/min_microvolts` = 680 mV) - tiefer kann der Regler nicht.

**Grenztest am absoluten Boden (2026-10-17):** `floor2.py` faehrt **alle**
Raten bis 1.0 GHz auf `duty 0` (Regler-Boden) und prueft mit Hash: **30 Punkte
(15 je Cluster), 0 Rechenfehler**. Der Boden selbst ist also stabil - begrenzt
wird er vom Regler, nicht vom Silizium.

**Zwei Fallstricke bei solchen Tests** (beide real passiert):

1. **Spannung NIE vor der Frequenz senken.** Der erste Versuch schrieb `duty 0`
   (680 mV) schon bei der Kanal-Suche, als noch `schedutil` lief und der Takt
   bei 2.4 GHz lag - das Board **hing** und der Watchdog rebootete (~43 s).
   Regel: erst Frequenz per `userspace` festnageln, **dann** Spannung senken;
   vor jedem Schreibzugriff den Punkt per `fsync` protokollieren.
2. **ext4 `commit=120`:** `/` ist mit `commit=120` gemountet. Nach einem
   harten Reset (Watchdog) gehen bis zu **120 s** nicht geflushte
   Schreibzugriffe **verloren** - ein per `install` deploytes Skript war danach
   wieder auf dem alten Stand. Nach jedem Deploy `sync` aufrufen.

Harte Nachpruefung:

- **Soak:** 60 s Volllast bei 24 MHz und 680 mV -> **0 Rechenfehler**.
- **Schnellwechsel** ueber die Tief-Takte (309 Spruenge, 680 mV): 0 Fehler.
- **Lehre aus einem Fehlversuch:** `--rapid-full` springt die GANZE Tabelle
  (bis 2.4 GHz) mit 680 mV an - dort braucht der Chip 1040 mV, das Board
  **haengt** und der Watchdog rebootet. Undervolt muss also **frequenzabhaengig**
  sein; die Werksspannung der hohen Takte darf nicht angetastet werden.

### Persistenter Weg (Overlay)

Ein Undervolt zur Laufzeit haelt nicht (der Regler-Cache und `cpufreq-dt`
ueberschreiben ihn). Dauerhaft geht es nur ueber die OPP-Tabelle im Overlay:

```bash
python3 gen_lowfreq_dts.py --big-mv 690 --little-mv 700   # Reserve lassen!
sh deploy_lowfreq.sh && reboot
```

Der Generator lehnt Werte unter 680 mV ab. **Vorsicht Boot-Loop:** seit die
Untergrenze dauerhaft auf 15.625 MHz steht, erreicht `schedutil` die Tief-Takte
schon kurz nach dem Boot. Ist die Spannung zu knapp, rebootet der Watchdog in
dieselbe Lage. Empfehlung: mindestens ~15-20 mV Reserve ueber dem gemessenen
Boden, also big >= 695, little >= 695.

### Braucht man die Reserve, wenn man unter dem HW-Limit bleibt?

**Fuer die Tief-Takte (< 1 GHz) ist die Reserve nicht noetig, um Stabilitaet zu
erreichen - aber sie bleibt sinnvoll als Boot-Schutz.** Begruendung aus den
Messungen:

- Am **absoluten Regler-Boden** (`duty 0`, Modell 680 mV) liefen **alle 30
  Punkte** bis 1.0 GHz mit **0 Rechenfehlern**. Das Silizium braucht dort also
  weniger, als der Regler hergibt; die Grenze ist der Regler, nicht der Chip.
  Eine Reserve *fuer die Rechenstabilitaet* bringt an dieser Stelle nichts.
- Die Reserve schuetzt vor dem **Boot-Loop**: mit `scaling_min_freq = 15.625 MHz`
  erreicht `schedutil` die Tief-Takte sofort nach dem Boot. Ist der Punkt dort
  grenzwertig (Chip-Streuung, Temperatur, Alterung), rebootet der Watchdog in
  dieselbe Lage - ohne Netz nicht mehr zu retten. Die Reserve deckt also die
  **Streuung ueber Chips und Temperaturen** ab, nicht den gemessenen Mittelwert.
- **Versteckte Marge schon vorhanden:** der ADC zeigt am Modell-Boden (680 mV)
  real **~722-727 mV**. Der Modellwert ist konservativ; zwischen Modell und
  echter Spannung liegen bereits ~45 mV. Wer im Overlay 695 mV (Modell) setzt,
  liegt real bei ~730-740 mV.

**Hohe Takte (> 1.9 GHz) sind anders:** dort ist die Grenze das **Silizium**
(bei 1.908 MHz hing das Board bei ~700 mV). Dort ist eine Reserve zwingend -
und die Werksspannung darf ohnehin nicht angetastet werden.

Kurz: unter dem HW-Limit (Tief-Takte) ist die Reserve **kein Stabilitaets-,
sondern ein Boot-Sicherheits-Polster**. Fuer eine unwiederbringliche
Ferninstallation ist sie trotzdem die richtige Wahl.

## Undervolting aller Takte (2026-10-17, gemessen)

`uv_lowfreq.py --all` faehrt jeden Taktpunkt beider Cluster an, senkt die
Spannung in 20-mV-Stufen und prueft die Rechenergebnisse per SHA-256-Hash
(zu wenig Spannung erzeugt zuerst stille Bitfehler, nicht sofort einen Hang).

| Cluster | Takt | Werksspannung | stabil bis | Absenkung |
|---|---|---|---|---|
| big (A73) | 15.625 - 1.000.000 kHz | 730 mV | 680 mV (Boden) | -50 mV |
| big | 1.200.000 | 752 mV | 691 mV | -61 mV |
| big | 1.398.000 - 1.512.000 | 770 mV | 690 mV | -80 mV |
| big | 1.608.000 | 780 mV | 681 mV | -99 mV |
| big | 1.704.000 | 791 mV | 691 mV | -100 mV |
| big | 1.800.000 | 831 mV | 711 mV | -120 mV |
| big | 1.908.000 | 860 mV | **~720 mV (Grenze)** | ~-140 mV |
| big | 2.016.000 - 2.400.000 | 910-1040 mV | nicht sauber messbar | s. unten |
| little (A53) | 15.625 - 1.000.000 kHz | 763 mV | 680 mV (Boden) | -83 mV |
| little | 1.200.000 | 781 mV | 682 mV | -99 mV |
| little | 1.398.000 | 809 mV | 689 mV | -120 mV |
| little | 1.512.000 - 2.016.000 | 860-1040 mV | -120 mV noch stabil | >= -120 mV |

Zwei Muster:

- **Tief-Takte (bis 1.0 GHz): der Regler-Boden 680 mV ist die Grenze, nicht das
  Silizium.** Am Boden liefen alle Punkte mit 0 Fehlern, auch 8 Wiederholungen
  bei 24 und 666 MHz - die Kerne haben dort noch Reserve, nur kann der PWM-Regler
  nicht tiefer (s. "Koennte man unter 680 mV?").
- **Hohe Takte: echte Silizium-Grenze.** Sie steigt mit dem Takt. Bei 1.908 MHz
  hing das Board bei ~700 mV und der Watchdog rebootete - dort ist die
  Absenkung also durch das Silizium begrenzt, nicht durch den Regler.

## Koennte man unter 680 mV?

**Nein, nicht ohne Hardware-Eingriff - aber die Grenze ist weich, nicht das
Silizium.** Zwei Gruende:

1. Der PWM-Regler bildet die Spannung als Tastverhaeltnis ab:
   `V = 680 mV + duty * 360 mV / period`. Bei 680 mV ist `duty = 0` (gemessen:
   `uv_to_duty('big', 680000) == 0`, `('little', ...) == 0`) - weniger Tastgrad
   gibt es nicht, also ist 680 mV das untere Ende des Reglers.
2. 680 mV steht zusaetzlich als `regulator-min-microvolt = <0xa6040>` im
   Device Tree (Node `regulator-vddcpu-a`/`-b`). Das ist eine Software-Schranke,
   die man im Overlay senken koennte - sie wuerde aber nichts bringen, weil
   Punkt 1 greift: der Regler selbst endet bei duty 0.

Fuer die **Tief-Takte** ist das schade: sie liefen am Boden mit 0 Fehlern, ein
Teil der Reserve bleibt also ungenutzt. Um wirklich tiefer zu kommen, muesste
man die Regler-Hardware aendern (andere PWM-Periode oder ein anderer
VDDCPU-Zweig) - das ist ein Eingriff an der Platine und wird hier nicht
empfohlen. Die **hohen** Takte sind ohnehin durch das Silizium begrenzt und
nicht durch den Regler, dort aendert der Boden nichts.

## Sicherheit: nur ein Lauf gleichzeitig

`uv_lowfreq.py` nimmt eine `flock`-Sperre (`/root/uv_lowfreq.lock`). Ohne sie
sind zwei gleichzeitige Instanzen gefaehrlich: die eine setzt beim `revert()` den
Governor zurueck auf `schedutil` (-> 2.4 GHz), waehrend die andere die Spannung
noch abgesenkt hat - das Board haengt und der Watchdog rebootet. Genau so ist es
zweimal passiert (10:18 und 10:37), bevor die Sperre eingebaut war.

## PWM-Periode als Spannungshebel? (2026-10-17, gemessen: NEIN)

Frage: laesst sich die Ausgangsspannung ueber die PWM-**Periode** senken (analog
zum CPU-Takt, wo der Teiler die Ausgabe ist)? Antwort: **nein, gemessen.**

Die Periode ist beschreibbar (250…15984, Faktor 64, Board stabil), aber sie
aendert nur die **Schaltfrequenz**, nicht die Spannung. Bei duty=0 gemessen:

| Periode | PWM-Frequenz | gemessene Spannung (ADC ch4) |
|---|---|---|
| 999 | 667 kHz | 733,9 mV |
| 1998 | 334 kHz | 733,0 mV |
| 3996 | 167 kHz | 735,2 mV |
| 7992 | 83 kHz | 729,1 mV |
| 15984 | 42 kHz | 733,4 mV |

Periode 16-fach variiert -> Spannung im Rauschen (±3 mV). Auch bei festem
Tastverhaeltnis identisch. Der Grund: die Spannung folgt `duty/period`, und bei
duty=0 ist dieser Bruch 0, unabhaengig von der Periode.

**Nebenbefund (wichtig):** der Test bei 1.908 MHz mit wachsender Periode und
festem duty (222) **hing das Board** bei P=5994 (Modellwert 693 mV), waehrend
P=3996 (700 mV) noch stabil war. Das ist aber die bekannte **Silizium-Grenze**
des hohen Takts, kein Perioden-Effekt (die Periode aendert die Spannung nicht).
Werkzeuge: `pwm_period_test.py`, `pwm_shift_test.py` (nicht persistent).

## Spannung erstmals MESSBAR (ADC, 2026-10-17)

Es gibt keinen Spannungsregler-Sensor, aber der SARADC (`meson-g12a-saradc`) hat
einen Kanal, der VDDCPU_A monoton folgt: **ch4**. Selbstkalibrierend ueber die
Kanaele `gnd`=0, `0.25vdd`=449,6, `0.5vdd`=900,0, `0.75vdd`=1349,6 mV
(1,8 V Referenz, `scale` = 0,439453125 mV/LSB). Werkzeug: `adc_voltage.py`.

Gemessene Kennlinie (big, 15,625 MHz, Periode 999):

| Ziel (Modell) | duty | gemessen |
|---|---|---|
| 731 mV | 142 | 766,8 mV |
| 700 mV | 56 | 746,6 mV |
| 680 mV | 0 | 722,0 mV |
| 675 mV (geklemmt) | 0 | 728,2 mV |
| 660 mV (geklemmt) | 0 | 733,9 mV |
| 640 mV (geklemmt) | 0 | 720,3 mV |

**Der echte Boden liegt bei ~730 mV, nicht bei den 680 mV aus dem DT.** Die
DT-Angabe ist konservativ. Alle Ziele unter 680 mV klemmen auf duty 0 und
aendern die gemessene Spannung nicht. Die Absenkung 731 -> 680 mV (Modell) ist
real ~767 -> 722 mV, also **~45 mV echte Ersparnis** - mehr geht nicht.

## Ablauf

```bash
# 1. installieren (auf dem Board, lowfreq.dts + Skripte in /root)
sh /root/deploy_lowfreq.sh
reboot

# 2. verifizieren
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies
sh /root/lowfreq_clk_truth.sh          # echter Takt je Rate

# 3. jeden Punkt unter Last pruefen
python3 /root/lowfreq_probe.py --iters 400000 --dwell 0.4

# 4. Untergrenze dauerhaft setzen (z.B. auf die tiefste Rate).
#    Schreibt BEIDE Dateien (cpu-freq-guard + cpufrequtils MIN_SPEED).
sh /root/ha/lowfreq_set_floor.sh 15625

# 5. Hotplug-Governor installieren (Kerne im Leerlauf aus)
sh /root/ha/deploy_hotplug.sh

# 6. optional: Undervolt der Tief-Takte (Reserve lassen, s. unten)
python3 /root/ha/gen_lowfreq_dts.py --big-mv 700 --little-mv 700
sh /root/ha/deploy_lowfreq.sh && reboot

# 7. Undervolt-Grenze selbst nachmessen
python3 /root/ha/uv_lowfreq.py --seconds 2 --workers 6 --soak
```

## Messergebnis (2026-10-17)

Alle 14 Raten je Cluster (15.625 MHz … 666.666 MHz) unter Last geprueft:
**stabil, 0 Rechenfehler**, der gemessene Zeitfaktor entspricht der erwarteten
Rate (z.B. x20 bei 50 MHz). Mit gesenkter Untergrenze waehlt `schedutil` die
tiefen Punkte **von selbst**: im Leerlauf little 249 MHz / big 124-499 MHz,
Temperatur 41 °C statt 66-68 °C unter Volllast.

Reboot-Test (2026-10-17): nach dem Reboot `scaling_min_freq` = 15625 auf beiden
Clustern, `cpu-freq-guard` + `cpu-hotplug` aktiv, schedutil waehlt im Leerlauf
little 250 MHz / big 200 MHz, 35 °C. Hotplug: idle cpu0-1, unter Last zurueck
auf cpu0-5.

## Ruecknahme

```bash
sh /root/revert_lowfreq.sh
reboot
```

Entfernt `user_overlays=lowfreq`, das Boot-Arg, den Dienst und die `.dtbo`.
`/boot/armbianEnv.txt.bak-lowfreq` wird dabei nicht angetastet.
