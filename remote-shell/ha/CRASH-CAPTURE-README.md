# Fehleraufzeichnung verbessern (Odroid N2) — damit der naechste Oops eine Spur hat

## Die Theorie, in einem Satz

**Ja.** Ein Kernel-Zeiger zeigte in einen nicht gemappten Bereich, der Kernel
wollte dort einen Befehl holen, fand keine Seitentabelle — und starb. Genau das
sagt der ESR `0x86000006`:

| Feld | Wert | Bedeutung |
|---|---|---|
| EC | `0x21` | Instruction Abort **aus dem Kernel** (EL1) |
| IFSC | `6` | Translation fault, Level 2 = **kein Mapping** auf dieser Ebene |

Und die Zeile `Code: ???????? ???????? ???????? ???????? (????????)` bestaetigt es:
der Kernel konnte die Bytes **an seinem eigenen Programmzaehler** nicht lesen —
der PC zeigte ins Leere. Ein Datenzugriff (Data Abort, EC `0x25`) waere etwas
anderes; ein Userspace-Fehler (EC `0x20`) ebenfalls.

**Was die Theorie nicht beantwortet:** *warum* der PC dort landete. Zwei
Familien, nicht unterscheidbar ohne Backtrace:

1. **Instabilitaet** (wahrscheinlich, board-spezifisch): PLL/Spannung
   aus dem Ruder -> die CPU holt Garbage -> Instruction Abort mit `Code: ????`.
   Passt zum Reset-Grund `hard-reset-unknown` (kein sauberer Panic-Pfad).
2. **Software-Bug**: korrupter Funktions-/Ruecksprungzeiger.

Ein Backtrace entscheidet das in Sekunden. **Deshalb ist die Aufzeichnung die
Reparatur** — nicht die Ursache raten, sondern den naechsten Absturz fangen.

## Warum es bisher keine Spur gab (zwei echte Luecken)

### Luecke 1: Ein Oops ist kein Panic

`panic=10` und **ramoops** haengen beide am **Panic-Pfad**. Ein Oops ist aber
standardmaessig **kein** Panic: der Kernel protokolliert ihn und laeuft weiter
(oder stirbt leise). Ohne `panic_on_oops=1` wird also **weder** ein
ramoops-Record geschrieben **noch** rebootet der Kernel nach 10 s.

Belegt in `RAMOOPS-README.md`: ein echter Panic (`sysrq-trigger c`) hinterliess
sofort einen `dmesg-ramoops-N`; die PLL-Hangs bei N=208/211 hinterliessen
**nichts** — sie erreichen den Panic-Pfad gar nicht.

### Luecke 2: journald syncht zu traege

Default ist `SyncIntervalSec=5min`. Bis zu 5 Minuten Kernel-Log liegen nur im
RAM — bei einem harten Reset sind sie weg. `rsyslog` schreibt zwar `kern.log`,
aber nur, was es **bekommen hat**, und auch das nur mit Verzoegerung.

## Der Reparaturplan (in dieser Reihenfolge)

### Schritt 0 — ramoops (Voraussetzung, PR #45)

Ohne reservierte Panic-Region ist alles Weitere sinnlos. `deploy_ramoops.sh`
erzeugt/validiert das Overlay und ergaenzt `user_overlays` (laesst `lowfreq`
stehen). Pruefen nach dem Reboot:

```sh
dmesg | grep ramoops      # "reserved mem: ... ramoops@50000000"
lsmod | grep ramoops
```

### Schritt 1 — Oops zum Panic machen (dieses Paket)

```sh
deploy_crash_capture.sh
```

Schreibt drei Artefakte (idempotent, reversibel mit `revert_crash_capture.sh`):

* `/etc/sysctl.d/99-crash-capture.conf` — `panic_on_oops=1`, `panic=10`,
  `hung_task_panic=1` (+120 s), `softlockup_panic=1`. Der stille Hang wird so
  als Panic gefangen (Software-Watchdog-Ergaenzung).
* `/etc/systemd/journald.conf.d/99-crash-capture.conf` — `SyncIntervalSec=1s`,
  `Storage=persistent`, `SystemMaxUse=64M`.
* `/boot/armbianEnv.txt` `extraargs` — `panic_on_oops=1 hung_task_panic=1`
  (gilt ab dem ersten Kernel-Zeitpunkt, vor dem Sysctl-Laden).

### Schritt 2 — harte Hangs sichtbar machen (optional: netconsole)

Ein PLL-/Spannungs-Lockup erreicht **keinen** Panic-Pfad. Dann hilft nur ein
Netz-Logger, der die printk-Zeilen sofort per UDP wegschickt:

```sh
deploy_crash_capture.sh 192.168.178.71 eth0 192.168.178.64 aa:bb:cc:dd:ee:ff
```

Braucht einen zweiten Rechner als Empfaenger (`nc -u -l 6666`).

### Schritt 3 — Audit

```sh
python3 crash_capture.py audit        # was fehlt der laufenden Konfiguration?
```

## Was dann passiert

Beim naechsten Oops: `panic_on_oops=1` macht daraus einen Panic -> ramoops
schreibt den vollen Dmesg **mit Backtrace** -> `panic=10` rebootet -> beim Boot
liest `boot_journal.py` den Dump und klassifiziert den Reset als `kernel-panic`
statt `hard-reset-unknown`. Erst **dann** ist die Ursache entscheidbar.

## Was das NICHT loest (ehrlich)

* **Nur Warm-Reset.** Ein Kaltstart/Stromausfall loescht den RAM.
* **Kein Ersatz fuer den Hardware-Watchdog.** ramoops ist Forensik, nicht
  Schutz. Gegen einen Hang schuetzt weiterhin nur der Watchdog (siehe
  `WATCHDOG-GAP.md`).
* Ein Hang, der den Panic-Pfad nicht erreicht, ist nur ueber **netconsole**
  (Schritt 2) sichtbar.
