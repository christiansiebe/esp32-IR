# htopv — CPU-Takt + Spannung live (Odroid N2+)

Werkzeuge, um auf dem Odroid N2+ **Takt pro Kern** und **Spannung pro Cluster**
gleichzeitig zu sehen. Reines Host-Tooling, kein Firmware-Bezug.

## Warum zwei Rails, nicht sechs

Der N2+ hat für die CPU nur **zwei** Spannungsregler (PWM-Regler, siehe
`UV-README.md`). Spannung ist damit **pro Cluster** messbar, nicht pro Kern:

| Rail | Cluster | Kerne | Cortex | cpufreq-Policy | regulator |
|------|---------|-------|--------|----------------|-----------|
| `VDDCPU_A` | big   | cpu2-5 | A73 (`0xd09`) | `policy2` | `regulator.2` |
| `VDDCPU_B` | little | cpu0-1 | A53 (`0xd03`) | `policy0` | `regulator.8` |

**Empirisch bestätigt** (kontrollierter Test, Governor eingefroren, Hotplug
gestoppt, je ein Cluster auf MAX):

```
nur policy0 (little) MAX -> VDDCPU_B = 1040000 uV   (VDDCPU_A bleibt 730400)
nur policy2 (big)    MAX -> VDDCPU_A = 1011200 uV   (VDDCPU_B bleibt 762800)
```

Also **A = big, B = little**. Diese Zuordnung steht auch im Devicetree-Kommentar
von `UV-README.md` (`regulator-vddcpu-a` = große Kerne).

> Achtung: Eine frühere Version der Anzeige hatte A/B vertauscht. Wenn Takt und
> Spannung "nicht zusammenpassen", zuerst diese Tabelle prüfen.

## Dateien

- `cpu-live` — **leichtgewichtiger** Live-Monitor (Takt/Spannung/Temperatur)
  **ohne htop**: liest nur wenige sysfs-Dateien, kein `/proc`-Scan, keine
  Bildschirm-Neuzeichnung. Gemessen **0,3 % eines Kerns bei 1-s-Takt** (htop
  kostete hier ~11-40 %). Optionen: `--once`, `-i SEK`, `-n N`, `--csv FILE`.
- `cpu-volt-watch` — Panel: Tabelle (Cluster, MHz, Volt, online-Kerne) + alle
  thermal zones.
- `cpu-volt-line` — Einzeiler für die tmux-Statuszeile:
  `little 1000MHz/0.763V  big 1704MHz/0.730V  cpu 42C`.
- `htopv` — startet `htop` in einer tmux-Session, deren Statuszeile
  `cpu-volt-line` alle 2 s neu zieht. So sind per-Kern-MHz (htop
  `show_cpu_frequency=1`) und Rails-Spannung gleichzeitig sichtbar.
  **Achtung:** htop erzeugt die eigentliche Last - für Dauerbeobachtung
  `cpu-live` nehmen.
- `htopv.tmux.conf` — tmux-Konfig dafür (`status-interval 2`, rechts `cpu-volt-line`).
- `deploy_htopv.sh` — installiert alles nach `/usr/local/bin` bzw. `/etc` und
  setzt `show_cpu_frequency=1` in `~/.config/htop/htoprc`.
- `test_htopv.py` / `test_cpu_live.py` — Host-Tests mit Fake-sysfs (u. a.
  Rail-Zuordnung, offline-Werte, CSV).

### Beobachten ohne die Messung zu verfälschen

Ein Live-Monitor ist selbst Last. htop zeichnet im Sekundentakt den ganzen
Bildschirm neu und liest `/proc/<pid>/stat` für **jeden** Prozess; auf dem N2+
kostete das gemessen **~11-40 % eines Kerns** und hielt die Kerne oben.
`cpu-live` liest nur `scaling_cur_freq` (2x), `microvolts` (2x) und `temp` (1x)
und schreibt eine einzige Zeile: **0,3 % bei 1 s**.

**Wichtig:** Ein **kontinuierlicher** Beobachter im Sekundentakt kann die
`schedutil`-Auslastungsschätzung trotzdem leicht anheben. Für reine
Leistungsmessungen daher **ereignisbasiert** messen: ein Wert pro Sekunde per
`cat` aus einem Shell-Loop oder `--once` in einer Schleife, und den Takt danach
aus dem `--csv`-Log auswerten - so läuft kein Dauerprozess.

**Nebenbefund Hotplug:** Der `cpu-hotplug`-Dienst legt idle-Kerne offline. Bei
**einem** online-Kern muss dieser die gesamte Last allein tragen, `schedutil`
fordert dann hohen Takt -> die Kerne fallen **nicht** auf 250 MHz. Mit allen
sechs online verteilt sich dieselbe Last und der Takt fällt bis **250 MHz**.
Wer den Tief-Takt sehen will, darf die Kerne also nicht zu stark wegnehmen.


## Installieren

```bash
apt-get install -y htop tmux        # Voraussetzungen
sh deploy_htopv.sh                  # installiert + setzt htoprc
htopv                               # startet die Anzeige
```

Einzelaufruf ohne htop: `cpu-live` (schlank), `cpu-volt-watch` bzw. `cpu-volt-line`.
Für Dauerbeobachtung ohne Eigenlast `cpu-live` nehmen, nicht `htopv`.

## Testbarkeit

`cpu-volt-watch`, `cpu-volt-line` und `cpu-live` lesen sysfs über die
Umgebungsvariablen `CPU_SYS`, `REG_SYS`, `THERMAL_SYS`. Ohne sie nutzen sie die
echten `/sys`-Pfade; die Host-Tests setzen sie auf einen Fake-Baum und führen die
Skripte als echte Subprozesse aus (`python3 test_htopv.py`,
`python3 test_cpu_live.py`).

## Nicht verwechseln

- `scaling_cur_freq` ist der **aktuelle** Takt (schedutil regelt laufend), nicht
  das Maximum. Für das Limit `scaling_max_freq` lesen.
- `scaling_max_freq` wird von **keinem** Dienst verwaltet (nur `min_freq` und
  der Governor kommen aus `cpu-freq-guard` / `rc.local`). Ein Test, der
  `scaling_max_freq` senkt, muss es danach wieder auf `cpuinfo_max_freq`
  zurücksetzen — sonst bleibt der Cluster gedeckelt (Little-Cluster auf 250 MHz
  = alles staut sich, Last steigt).
