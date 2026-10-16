# Laufzeit-Undervolting Odroid N2+ (`uv.py`)

Bewusst **nicht persistent** und durch den Hardware-Watchdog abgesichert.

## Warum kein Sysfs-Weg

Am Board (Kernel 7.1.8-edge-meson64) vermessen:

- Die CPU-Spannung erzeugt ein **PWM-Regler**: `regulator-vddcpu-a` (große Kerne,
  `policy2`) und `regulator-vddcpu-b` (kleine Kerne, `policy0`).
- Die Regler-Sysfs (`/sys/class/regulator/*/microvolts`) und die OPP-Tabelle
  (`/sys/kernel/debug/opp/*/supply-0/u_volt`) sind **read-only** (0444). Ein
  Spannungswert pro Frequenz lässt sich zur Laufzeit also **nicht** setzen.
- Der einzige Laufzeit-Weg ist das **PWM-Tastverhältnis** direkt im Register.

## Beziehung (am Board vermessen, linear)

```
V[uV] = 680000 + duty * 360000 / period
Registerwert = ((period - duty) << 16) | duty
```

| Cluster | Registerbasis | Offset | Periode | Regler |
|---|---|---|---|---|
| big (policy2) | `0xffd1b000` | `0x00` | 999 | regulator.2 (VDDCPU_A) |
| little (policy0) | `0xff802000` | `0x04` | 333 | regulator.8 (VDDCPU_B) |

`/sys/.../microvolts` zeigt danach **weiterhin den alten Wert** — der
Regler-Framework-Cache merkt vom Register-Schreibzugriff nichts. Maßgeblich ist
`duty_uv` aus `uv.py status`.

## Voraussetzung: feste Frequenz

Der `cpufreq-dt`-Treiber besitzt den Regler und setzt die Spannung bei jedem
Frequenzwechsel neu. Undervolting hält daher nur mit dem **userspace**-Governor
und einer festgenagelten Frequenz (`--big-freq` / `--little-freq`).

## Benutzung

```bash
python3 uv.py status                                   # aktueller Zustand
python3 uv.py apply --offset-mv 100                    # -100 mV @ 1.8 GHz
python3 uv.py apply --big-freq 1800000 --little-freq 1800000 --offset-mv 100
python3 uv.py apply --big-mv 731 --little-mv 900       # absolute Zielwerte
python3 uv.py revert                                   # zurück auf schedutil
```

Sicherheitsgrenze: nie unter **700 mV** (`SAFE_MIN_UV`).

## Verifikation am Board (2026-10-15)

- Schreibpfad per No-Op-Write bestätigt.
- **Feste Frequenz** (1.8 GHz), Volllast (6×25 s): stabil bei -20/-60/-100/-140 mV.
- **Unter Frequenzwechsel** (`uvstress.py`, ganze OPP-Tabelle, 2 Zyklen, 6 Prozesse,
  Ergebnisprüfung per Hash):

| Absenkung | Ergebnis |
|---|---|
| -40 mV | STABIL |
| -60 mV | STABIL |
| -80 mV | STABIL |
| -100 mV | STABIL |
| -110 mV | **Hang → Watchdog-Reboot** |
| -120 mV | **Hang → Watchdog-Reboot** |

- Grenze unter Frequenzwechsel liegt also zwischen **100 und 110 mV**. Empfohlen
  mit Reserve: **-80 mV** (big 831→751, little 1000→920).
- Der frühere Absturz bei -100/-40 mV war ein **Fehler im Test**, nicht die
  Hardware: der Regler-Cache programmiert nur bei geänderter Zielspannung neu,
  wodurch sich die Absenkung bei gleicher Werksspannung aufsummierte (siehe
  `stock_duties()` in `uvstress.py`).
- Watchdog: `RuntimeWatchdogUSec=1min`, `watchdog-guard.timer` aktiv,
  `kernel.panic=10` → ein Hang rebootet das Board, danach ist alles auf Werkswert.
  Alle Hänger wurden so automatisch aufgefangen.

## Erwartete Ersparnis (ehrlich, nicht gemessen)

Keine Leistungsmess-Hardware vorhanden, daher nur Abschätzung über P ∝ V²·f:

- Big-Cluster bei -100 mV: (731/831)² = 0,77 → ~23 % weniger **dynamische**
  Kernleistung. Little bei -100 mV: (900/1000)² = 0,81 → ~19 %.
- Das betrifft nur die CPU-Kern-Rails. Am gesamten Board (RAM, USB, WLAN, SSD,
  HDMI) sind die Kerne nur ein Teil → unter Volllast grob **5–10 % der
  Board-Leistung**.
- Im **Idle kann es schlechter werden**: der userspace-Governor nagelt die
  Frequenz fest (z. B. 1.8 GHz), schedutil würde auf 1.0 GHz heruntertakten.
- Fazit: echter, aber **moderater** Effekt — kein großer Sprung. Und nur
  sinnvoll unter Dauerlast, nicht im Leerlauf.

## Nicht persistent

Nichts wird gespeichert. Nach einem Reboot ist der Governor wieder `schedutil`
und die Spannung auf Werkswert. Genau so gewünscht.
