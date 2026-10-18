# cpu-real-clock - ehrlicher CPU-Takt in `scaling_cur_freq`

## Das Problem

Der Amlogic-`cpufreq-dt`-Treiber meldet in `scaling_cur_freq` den OPP, den er
**zu setzen glaubt**. Ein manueller PLL-Overclock (siehe `PLL-OC-README.md`,
Schreiben von HHI `0xff63c000+0x2f4` big / `+0x380` little via `/dev/mem`) ist
fuer den Treiber **unsichtbar**. Folge: alle Anzeigen - `cpu-live`,
`cpu-volt-line`, `htopv`, `htop` (mit `show_cpu_frequency=1`), Home Assistant -
zeigen den gecachten Wert **2400**, obwohl der PLL real mit **2472** laeuft.

Das ist keine Kosmetik: man kann einen Overclock nicht messen, wenn jedes
Werkzeug den alten Wert zeigt.

## Die Loesung

`cpu_real_clock.py` liest pro Cluster den **lebenden PLL-N** und den echten
`scaling_cur_freq`, berechnet daraus den realen Takt und **bind-mountet** eine
kleine tmpfs-Datei mit diesem Wert ueber
`/sys/devices/system/cpu/cpufreq/policy{0,2}/scaling_cur_freq`.

Warum das ueberall wirkt: die Per-Core-Pfade
(`cpuN/cpufreq/scaling_cur_freq`) sind **derselbe Inode** wie der Policy-Pfad.
Ein Mount pro Policy deckt also jeden Leser ab - `htop` liest
`cpu%u/cpufreq/scaling_cur_freq` und bekommt damit ebenfalls den echten Wert.

Der reale Wert wird ueber einen **vor** dem Mount geoeffneten Dateideskriptor
gelesen (`os.pread`), damit der Daemon nie seine eigene Ausgabe zurueckliest.

## Wie der reale Takt berechnet wird

Am Top-OPP setzt der Treiber den PLL direkt auf die OPP-Rate (Teiler 1) - dort
**ist** der PLL der Takt, und ein manueller Overclock erscheint hier. Unterhalb
des Top-OPP haelt der Treiber den PLL hoch (3000 MHz) und **teilt** ihn herunter
(z. B. 1500 = 3000/2), dort ist der PLL **nicht** der Takt und der Wert des
Treibers stimmt bereits.

Unterschieden werden die beiden Faelle ueber das Verhaeltnis: nur am Top-OPP
liegt der PLL innerhalb weniger Prozent **ueber** dem Treiberwert.

| Cluster | Top-OPP | PLL-N | PLL MHz | real |
|---|---|---|---|---|
| big   | 2400 (stock) | 200 | 2400 | 2400 |
| big   | OC           | 206 | 2472 | **2472** |
| little| 2016 (stock) | 168 | 2016 | 2016 |
| little| low OPP      | 250 | 3000 | Treiberwert (geteilt) |

Die Regel steht als reine Funktion `real_khz(pll_n, cached_khz)` in
`cpu_real_clock.py` und ist mit `test_cpu_real_clock.py` abgedeckt.

## Installation

```sh
sh deploy_cpu_real_clock.sh
```

Legt an:
- `/usr/local/bin/cpu-real-clock` (Daemon)
- `/etc/systemd/system/cpu-real-clock.service`
- aktiviert + startet den Dienst (reboot-fest, `Restart=always`)

Deaktivieren: `systemctl disable --now cpu-real-clock`.

## Verhalten

- **Hotplug-fest:** ein Cluster darf offline gehen und wiederkommen; der Mount
  bleibt und der vorab geoeffnete fd liefert beim Wieder-Online den echten Wert.
  Ist ein Cluster offline, bleibt die Datei leer, damit Leser `-` zeigen (nicht
  `0`).
- **Sauberes Stop:** `SIGTERM`/`SIGINT` werden gefangen und alle Overlays
  abgehaengt (`KillSignal=SIGTERM`, `TimeoutStopSec=10`).
- **Kein Puffer:** `-i 1` (Sekunden) aktualisiert den Wert; der Takt wird pro
  Tick frisch aus dem PLL gelesen.

## Verifikation (Odroid N2+, 2026-10-18)

- `htop` zeigt bei echtem N=206 **2472** statt **2400** (Screen-Capture geprueft).
- `cpu-volt-line`: `big 2472MHz/1,040V` bei N=206, `big 2400MHz` bei N=200.
- `scaling_cur_freq` selbst liefert `2472000` bei N=206.
- Mount ueberlebt `echo 0 > cpu4/online` und `echo 1 > cpu4/online`.
- `systemctl stop` -> `mounts_after_stop=0`, Datei wieder real.
- Host-Test `test_cpu_real_clock.py`: 10 Checks, 0 Fehler.

## Dateien

- `cpu_real_clock.py` - der Daemon (auch `cpu-real-clock`).
- `cpu-real-clock.service` - systemd-Unit.
- `deploy_cpu_real_clock.sh` - Installation auf dem Board.
- `test_cpu_real_clock.py` - Host-Test der `real_khz`-Regel.
