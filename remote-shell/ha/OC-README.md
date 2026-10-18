# CPU-Overclock ueber 2400 MHz (Odroid N2+, big-Cluster)

Der Werks-DTB deckelt den big-Cluster (4x Cortex-A73, `opp-table-1`,
`policy2`) bei **2400 MHz**. Der big-PLL kann hoeher; dieser Punkt wird als
**umkehrbares Overlay** bereitgestellt. Getrennt von den Tief-Takten
(`LOWFREQ-README.md`), damit der Standardlauf `lowfreq.dts` nicht anfasst.

> **Das ist ein Experiment, kein Dauerbetrieb.** Oberhalb von 2400 MHz laeuft
> der Cluster **unterspannt** (s. u.). Es gibt keinen Spannungsspielraum, um
> einen hoeheren Takt abzusichern.

## Register (gemessen)

Der big-Cluster-CPU-PLL liegt im HHI-Block `0xff63c000`, Register `+0x2f4`
(little: `+0x380`). Die Bits `[8:0]` halten den ganzzahligen Multiplikator **N**.
Quelle ist der feste 24-MHz-xtal, der DCO laeuft hinter einem festen
`/2`-Nachteiler:

```
sys_pll (big) = N * 12 MHz
```

| Frequenz | N (Bits [8:0]) | reg 0x2f4 |
|---|---|---|
| 2400 MHz (Werksmaximum) | 200 | 0xd80104c8 |
| 2448 MHz | 204 | 0xd80104cc |
| 2460 MHz | 205 | — |
| **2496 MHz (stabil)** | **208** | — |
| 2508 MHz | 209 | — |
| 2520 MHz | 210 | — |
| 2532 MHz | 211 | — |

**2450 MHz ist nicht darstellbar** (N = 204,17). Der PLL erzeugt nur Vielfache
von 12 MHz. `gen_oc_dts.py` lehnt solche Raten ab.

`scaling_cur_freq` und `clk_rate` melden nach einem rohen Register-Schreibzugriff
weiter die **gecachte** Rate (2400). Der ehrliche Nachweis ist die Wanduhrzeit
fuer feste Arbeit (`perf_counter`, Arch-Timer unabhaengig vom CPU-PLL): die
Messergebnisse skalieren linear mit dem Takt (2448 -> +2,06 %, 2460 -> +2,43 %).

## Stabilitaet (gemessen, `test_n.py`)

`test_n.py` setzt N, faehrt eine deterministische, hash-gepruefte Schleife und
vergleicht den Hash mit einer Referenz bei Werks-2400. Zu wenig Spannung erzeugt
zuerst stille Bitfehler, also aendert sich der Hash.

| N | MHz | Ergebnis |
|---|---|---|
| 204 | 2448 | PASS |
| 205 | 2460 | PASS, 25 s Soak, 59 Laeufe, 0 Fehler |
| 206 | 2472 | PASS |
| 207 | 2484 | PASS |
| 208 | **2496** | **PASS, 120 s Soak, 570 Laeufe, 0 Fehler** |
| 209 | 2508 | einmal PASS (10 s), **Hang im 90-s-Soak** |
| 210 | 2520 | grenzwertig: einmal Hang, danach 2x PASS |
| 211 | 2532 | **Hang** (Watchdog-Reboot) |

Verlaessliche Obergrenze: **2496 MHz (N=208)**. 2508-2520 grenzwertig,
2532 faellt aus. Bei 2460 MHz bleibt das Board kuehl (<= 39 °C unter Last).

## Risiko: die Spannung ist am Anschlag

Bei Werks-2400 sitzt der PWM-Regler bereits auf **100 % duty** (999/999 big,
333/333 little) = **1040 mV** (ADC-gemessen 1027 mV). Jeder Punkt ueber 2400 MHz
laeuft daher **unterspannt** relativ zur OPP-Kurve - es gibt keinen Spielraum,
die Spannung zur Absicherung zu erhoehen. Deshalb:

- `gen_oc_dts.py` traegt fuer den OC-Punkt die **Regler-Obergrenze** ein
  (Standard 1040 mV) und lehnt Werte unter dem Regler-Boden 680 mV ab.
- Der Punkt ist ein Einzelexperiment; er wird **nicht** als Dauerbetrieb
  empfohlen. 2496 MHz ist gemessen stabil, aber ohne Spannungsreserve.

Ein roher Register-Poke ist zudem **nicht** die sichere Sequenz des Treibers:
der Treiber muxed auf einen glitchfreien Parent, programmiert um und muxed
zurueck. Ein Hang nach einem rohen Poke ist daher kein Beweis, dass das Silizium
bei diesem Takt versagt - das Overlay geht den Treiber-Weg (neuer OPP-Punkt).

## Dateien

| Datei | Zweck |
|---|---|
| `gen_oc_dts.py` | erzeugt `oc.dts` mit dem OC-Punkt (Standard 2496 MHz) |
| `deploy_oc_safe.sh` | Test-Overlay: ein Boot aktiv, danach automatisch zurueck; `--promote` dauerhaft |
| `restore-stock-oc.sh` | idempotente Ruecknahme von Overlay + Waechtern |
| `oc-freq-guard.sh` / `.service` | deckelt `scaling_max_freq` auf den Werkswert |
| `test_gen_oc_dts.py` | Host-Tests der Erzeugung (kein Board) |
| `test_oc_safe.sh` | Host-Tests des Ein-Boot-Fensters + Ruecknahme |

Die Tief-Takte bleiben in `gen_lowfreq_dts.py` / `lowfreq.dts`. Beide Generatoren
teilen sich die Rendering-Funktion `render()`; der Standardlauf von
`gen_lowfreq_dts.py` ist **byte-identisch** zu `lowfreq.dts` (im Test geprueft).

## Erzeugen

```bash
# nur der OC-Punkt (Standard: 2496 MHz @ 1040 mV)
python3 gen_oc_dts.py

# OC-Punkt UND die Tief-Takte unter 1 GHz in einem Overlay
python3 gen_oc_dts.py --with-lowfreq --big-mv 700 --little-mv 700

# anderer (gemessener) Punkt
python3 gen_oc_dts.py --rate-mhz 2460 --mv 1040
```

`--rate-mhz` muss ein Vielfaches von 12 MHz sein (PLL-Grenze), `--mv` mindestens
680 (Regler-Boden). Ein Lauf oberhalb von 2496 MHz gibt eine Warnung aus.

## Boot-Sicherheit (umkehrbar)

Das Overlay ist beim ersten Reboot **nicht** aktiv und verschwindet nach dem
Testboot von selbst - eine instabile Frequenz darf das Board nicht dauerhaft in
eine Boot-Schleife schicken. Ablauf wie bei `deploy_lowfreq_safe.sh`
(arm -> test -> auto-revert):

1. `deploy_oc_safe.sh` nimmt `oc` aus `user_overlays` **heraus** (andere Namen,
   z. B. `lowfreq`, bleiben stehen) und setzt den Marker `/etc/oc-test.pending`.
   Das laufende System behaelt Werkswerte.
2. `oc-test-hold.service` laeuft einmal pro Boot nach `sysinit`:
   - Marker da -> `oc` in `user_overlays` aufnehmen, Marker loeschen (scharf).
   - Marker weg -> `oc` wieder aus `user_overlays` entfernen (zurueck zu
     Werkswerten; andere Overlays bleiben).
3. Reboot-Folge:

   | Reboot | Zustand |
   |---|---|
   | 1 | Werkswerte (Unit traegt `oc` ein) |
   | 2 | **ein** Testboot mit dem OC-Punkt (Unit traegt es wieder aus) |
   | 3 | Werkswerte |

Zusaetzlicher Schutz waehrend der Testboot:

- `cpufreq.default_governor=performance` (Boot-Arg) haelt den fruehen Governor
  auf **maximaler** Frequenz - kein stiller Sprung auf den unterspannten Punkt.
- `oc-freq-guard.service` deckelt `scaling_max_freq` nach `sysinit` auf den
  Werkswert **2400 MHz**. Der OC-Punkt ist damit nur durch ein **ausdrueckliches**
  Anheben von `scaling_max_freq` erreichbar - cpufreq-dt wuerde sonst das
  Policy-Maximum auf den hoechsten OPP setzen und direkt hineinbooten.
- `cpu-freq-guard.service` (aus der Lowfreq-Arbeit) pinnt `scaling_min_freq`
  auf 1.0 GHz.

Der systemd-Hardware-Watchdog (`RuntimeWatchdogSec=60`) faengt einen echten Hang.

## Ablauf

```bash
# 1. Overlay erzeugen und Test scharf machen (auf dem Board)
python3 /root/ha/gen_oc_dts.py --out /root/oc.dts
sh /root/ha/deploy_oc_safe.sh
reboot            # Reboot 1: Werkswerte, Overlay wird scharf gemacht
reboot            # Reboot 2: Testboot mit 2496 MHz
reboot            # Reboot 3: automatisch wieder Werkswerte

# 2. waehrend Reboot 2 pruefen: ist der Punkt ueberhaupt da?
cat /sys/devices/system/cpu/cpufreq/policy2/scaling_available_frequencies
#    -> 2496000 muss auftauchen (zusaetzlich zu 2400000)

# 3. den OC-Punkt ausdruecklich anwaehlen (nur Testboot!)
echo 2496000 > /sys/devices/system/cpu/cpufreq/policy2/scaling_max_freq
cat /sys/devices/system/cpu/cpufreq/policy2/scaling_cur_freq

# 4. dauerhaft uebernehmen (optional, auf eigenes Risiko)
sh /root/ha/deploy_oc_safe.sh --promote

# 5. komplett zuruecknehmen
sh /root/ha/restore-stock-oc.sh && reboot
```

## Ruecknahme

`restore-stock-oc.sh` nimmt `oc` aus `user_overlays`, laesst andere Overlays
(z. B. `lowfreq`) unberuehrt und entfernt den Boot-Governor nur, wenn kein
anderes Overlay mehr aktiv ist. Es loescht `oc.dtbo`, den Marker und beide
OC-Dienste, und setzt `scaling_max_freq` auf `cpuinfo_max_freq` bzw.
`scaling_min_freq` auf 1.0 GHz. Idempotent, danach Reboot noetig.

## Herkunft der Messwerte

Die Zahlen stammen aus `PLL-OC-README.md` und `test_n.py` (bereits gemessen,
Werkzeuge `pll_oc_probe.py`, `test_n.py`, `soak_oc.py`, `sweep_oc.py`,
`regdump.py`). Diese Dateien wurden fuer die Overlay-Arbeit **nicht** angefasst.
