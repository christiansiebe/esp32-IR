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
wird **still abgerundet**, und `scaling_cur_freq` meldet trotzdem die
Wunschrate. Gemessen:

| angefordert | `scaling_cur_freq` | echter Takt (`cpu_clk_dyn`) |
|---|---|---|
| 333333 kHz | 333333 kHz | **250000 kHz** |
| 33333 kHz | 33333 kHz | **25000 kHz** |

Solche Punkte sind in `lowfreq.dts` deshalb **nicht** enthalten: `cpufreq`
wuerde die falsche Frequenz melden und die Spannung zum falschen OPP-Punkt
waehlen. Die Liste enthaelt nur Raten, die der Taktgeber exakt erzeugt.

Der **echte** Takt steht nur in debugfs:
little -> `/sys/kernel/debug/clk/cpu_clk_dyn/clk_rate`,
big -> `/sys/kernel/debug/clk/cpub_clk_dyn/clk_rate`.
(`cpu_clk`/`cpub_clk` sind die Eltern von AXI/ATB/APB; `cpu_clk_dyn` ist der
CPU-Takt selbst.)

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
Takt. Gemessen im Leerlauf: nur cpu0+cpu1 online, cpu2-5 aus; unter Last holt
der Dienst sie zurueck. Ein offline genommener Kern verbraucht keinen Takt und
keine Leckleistung mehr.

Regeln des Dienstes:
- **cpu0 bleibt immer online** (Boot-CPU; ihr Wieder-Onlinen ist nicht
  garantiert).
- Stufen: Last < 0.5 -> 2 Kerne, < 1.5 -> 3, < 3 -> 4, sonst alle.
- Hysterese: eine Aenderung erst nach 6 Messungen (30 s) mit gleicher Stufe.
- Beim Beenden (SIGTERM) werden alle Kerne wieder online genommen.
- Reine Entscheidung `plan_online()`, host-getestet
  (`test_cpu_hotplug.py`, 134 Checks).

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
