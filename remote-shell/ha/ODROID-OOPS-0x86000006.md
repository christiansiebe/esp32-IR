# Kernel-Oops `0x86000006` auf odroidn2 — Analyse (2026-10-20)

## Der Befund

```
Message from syslogd@odroidn2 at Oct 20 08:42:51 ...
 kernel:[ 7909.546250] Internal error: Oops: 0000000086000006 [#1]  SMP
 kernel:[ 7909.844037] Code: ???????? ???????? ???????? ???????? (????????)
```

### ESR dekodiert

`0x86000006` ist der ESR_EL1-Wert, nicht der Fehlercode:

| Feld | Wert | Bedeutung |
|------|------|-----------|
| EC (bits 31:26) | `0x21` | **Instruction Abort, aus dem aktuellen EL** — also aus dem Kernel (EL1), nicht aus Userspace |
| IL (bit 25) | 1 | 32-bit Instruktion |
| ISS (bits 24:0) | `0x6` | **IFSC = 6 = Translation fault, level 2** |

Zusammengesetzt: **der Kernel wollte einen Befehl von einer Adresse holen, fuer die
es keine gueltige Seitentabelle gibt** (Translation fault L2 = kein Mapping auf
dieser Ebene). Das passt exakt zur `Code:`-Zeile: der Dump der Bytes *an PC* ist
`????????` — der Kernel konnte den Code am Programmzaehler selbst nicht lesen, weil
der PC ins Leere zeigt.

Das ist **kein** Datenzugriff (Data Abort) und **kein** Userspace-Absturz
(IABT_LOW waere EC `0x20`). Es ist ein Sprung des Kernels auf eine ungueltige
Adresse — entweder ein korrumpierter Rücksprung-/Funktionszeiger, oder die CPU
hat nach einer Instabilitaet Garbage geholt.

## Kein Trace — und warum

Die Rohdaten sind verloren. Geprueft ueber den read-only-Kanal und durch den
Board-Agenten:

* `dmesg` — kein `Internal error`
* `journalctl -k` (alle Boots) — kein Treffer; `journalctl -u kernel` = „No entries"
* `/var/log/kern.log` — kein Treffer
* `/sys/fs/pstore/` — **leer** (kein ramoops deployt)
* Boot-Journal: der Absturz erscheint nur als `reason=hard-reset-unknown`
  (boot #42, `prev_uptime=10549.9s`)

Der Grund ist dokumentiert (siehe `RAMOOPS-README.md`): ohne ramoops bricht das
Journal beim Freeze ab (journald flusht nicht), `watchdog0/bootstatus` ist auf
Amlogic immer 0, und pstore bleibt leer. Ein Kernel-Panic ist damit **nicht**
diagnostizierbar — nur der Reset-Grund `hard-reset-unknown` bleibt uebrig.

## Hypothesen (nicht unterscheidbar ohne Trace)

1. **Instabilitaet durch OC/UV/Clock** (wahrscheinlichste, board-spezifisch).
   Dieses Board hat eine belegte Historie: PLL-Overclock (N=211/2532 MHz haengt),
   Undervolt (680 mV haengt bei 2.4 GHz), Lowfreq-Overlay, CPU-Hotplug.
   Ein PLL-/Spannungsausflug laesst die CPU genau so Garbage holen ->
   Instruction Abort mit `Code: ????`. Der Reset war `hard-reset-unknown`,
   d. h. **kein** sauberer Panic-Pfad, was zu einer Hardware-/Clock-Stoerung passt.
   Aktuell laufen `cpu-hotplug.service` und `cpu-freq-guard.service` (Clock-Floor).
2. **Software-Bug**: korrumpierter Funktionszeiger / Race beim Modul-Entladen.
   Laesst sich ohne Backtrace nicht bestaetigen oder ausschliessen.

Die `Code: ????`-Signatur allein ist **charakteristisch fuer Variante 1**
(PC zeigt in nicht gemappten Speicher) — bei einem reinen NULL-Deref gaebe es
einen lesbaren Code-Dump und meist EC `0x25` (Data Abort).

## Empfehlung

**Ohne Trace ist jede weitere Reparatur Blindflug.** Reihenfolge:

1. **ramoops/pstore deployen** (PR #45, `deploy_ramoops.sh`) — damit landet der
   Panic-Dmesg im reservierten RAM und `boot_journal.py` klassifiziert den Reset
   als `kernel-panic` statt `hard-reset-unknown`. Das ist die Voraussetzung fuer
   jeden weiteren Schritt.
2. **`panic_on_oops=1` + `panic=10`** in der Kernel-Cmdline: der Oops wird dann
   sofort zum Panic (kein Weiterlaufen mit korruptem Zustand) und der Watchdog
   rebootet sauber — und der Panic landet in ramoops.
3. Erst danach: OC/UV-Sweep gegen den Absturz korrelieren (Uptime bei Absturz,
   `debugfs`-Clock vs. gesetzte Spannung). Falls sich Hypothese 1 bestaetigt,
   ist der Fix das Zuruecknehmen des aggressivsten Clock-/Spannungspunkts.

## Was der Watchdog tut (und was nicht)

`watchdog-guard.service` laeuft (alle ~2 min, „fail-safe reboot if HW watchdog
lost") und der Board rebootet nach dem Oops auch wieder. Der offene Befund aus
dem PLL-OC (Hardware-Watchdog greift nach einem Hang **nicht** zuverlaessig)
bleibt davon unberuehrt: der Guard prueft nur, ob der Watchdog noch *scharf* ist,
nicht ob er *ausgeloest* hat. Punkt 2 oben schliesst diese Luecke fuer den
Oops-Fall, weil `panic=10` den Reset selbst ausloest.

## Datenerhebung (read-only, jederzeit reproduzierbar)

```bash
# ueber den read-only-Kanal (Board-Allowlist: id/uptime/uname/free/df/
#   systemctl status <unit>/journalctl -u <unit>):
/ro?token=..&c=uptime
/ro?token=..&c=journalctl+-u+boot-journal.service+-n+200+--no-pager
/ro?token=..&c=systemctl+status+cpu-hotplug.service+--no-pager
```

Kernel-Logs (`dmesg`, `journalctl -k`) sind auf dem Board **bewusst gesperrt**
(fail-closed-Allowlist, `cline/0f7jc38h`). Das ist richtig so — es bedeutet aber,
dass die Oops-Analyse bis zum ramoops-Deploy auf die syslogd-Weiterleitung
(`Message from syslogd@...`) angewiesen bleibt.
