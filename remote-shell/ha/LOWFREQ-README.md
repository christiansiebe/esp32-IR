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

# 4. Untergrenze dauerhaft setzen (z.B. auf die tiefste Rate)
sh /root/lowfreq_set_floor.sh auto
```

## Messergebnis (2026-10-17)

Alle 14 Raten je Cluster (15.625 MHz … 666.666 MHz) unter Last geprueft:
**stabil, 0 Rechenfehler**, der gemessene Zeitfaktor entspricht der erwarteten
Rate (z.B. x20 bei 50 MHz). Mit gesenkter Untergrenze waehlt `schedutil` die
tiefen Punkte **von selbst**: im Leerlauf little 249 MHz / big 124-499 MHz,
Temperatur 41 °C statt 66-68 °C unter Volllast.

## Ruecknahme

```bash
sh /root/revert_lowfreq.sh
reboot
```

Entfernt `user_overlays=lowfreq`, das Boot-Arg, den Dienst und die `.dtbo`.
`/boot/armbianEnv.txt.bak-lowfreq` wird dabei nicht angetastet.
