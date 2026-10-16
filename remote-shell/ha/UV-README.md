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
- Volllast (6 Prozesse × 25 s) bei -20/-60/-100/-140 mV jeweils stabil.
- Empfohlener Startwert: **-100 mV** @ 1.8 GHz (big 831→731, little 1000→900).
- Watchdog: `RuntimeWatchdogUSec=1min`, `watchdog-guard.timer` aktiv,
  `kernel.panic=10` → ein Hang rebootet das Board, danach ist alles auf Werkswert.

## Nicht persistent

Nichts wird gespeichert. Nach einem Reboot ist der Governor wieder `schedutil`
und die Spannung auf Werkswert. Genau so gewünscht.
