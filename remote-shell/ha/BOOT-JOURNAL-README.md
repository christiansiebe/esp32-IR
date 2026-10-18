# Boot-Journal — Reset-Zaehler mit Grund (Odroid N2)

Die Amlogic-Firmware liefert **keinen** Reset-Grund:

- `/sys/class/watchdog/watchdog0/bootstatus` ist **immer 0** (auch nach
  Watchdog-Reset) — die GXBB-Watchdog-Treibervariante meldet ihn nicht.
- `/var/log` ist zram; das Journal ist zwar nach `/var/log.hdd/journal`
  symlinkt (persistent), bricht aber bei jedem Hard-Reset **abrupt** ab — es
  fehlt `Reached target Shutdown`. Ein Auswerten von `journalctl --list-boots`
  allein liefert daher **keinen** Grund, nur die Beobachtung "kein sauberer
  Abschied".
- **Panics werden jetzt aber sicher erfasst:** ein `ramoops`-Overlay
  (`deploy_ramoops.sh`) reserviert RAM bei `0x50000000`, den der Kernel als
  `pstore` nutzt. Das Panic-Dmesg ueberlebt den Warm-Reset und wird nach dem
  Boot nach `/var/lib/systemd/pstore/` archiviert. Details: `RAMOOPS-README.md`.

Deshalb zaehlt dieses Journal die Boots **selbst** und leitet den Grund aus vier
Spuren ab (`boot_journal_lib.classify_reset`, Prioritaet von oben):

| Spur | Grund | Quelle |
|------|-------|--------|
| Clean-Marker gesetzt | `clean-reboot` | `ExecStop` von `boot-journal.service` (`systemd-shutdown`-Pfad) |
| `watchdog0/bootstatus` != 0 | `watchdog-reset` | sysfs (auf Amlogic ungenutzt, portabel mitgenommen) |
| Panic in pstore/ramoops (frisch, noch nicht verbraucht) | `kernel-panic` (+Text) | `/sys/fs/pstore`, `/var/lib/systemd/pstore` |
| "Kernel panic" im vorigen Kernel-Log | `kernel-panic` | `journalctl -b -1 -k` (Fallback) |
| voriger Lauf < 60 s | `early-reset` | Heartbeat (`last_seen_uptime_s`) |
| sonst | `hard-reset-unknown` | Hang / Panic / Stromausfall ohne Spur |

**pstore-Dedupe (wichtig):** ramoops und `systemd-pstore` behalten alte Dumps.
Damit ein alter Panic nicht jedem weiteren Boot erneut angehaengt wird, merkt
sich der Zustand die **mtime** des zuletzt verbrauchten Dumps (monoton) und
ignoriert jeden Dump, der nicht juenger ist. So traegt nur der echte
Panic-Reboot den Text.

## Aufbau

- `boot_journal_lib.py` — reine Logik (Klassifikation + Zustandsformat),
  **host-getestet** (`test_boot_journal_lib.py`, 36 Checks;
  `test_boot_journal_flow.py`, 10 Checks: sauberer Reboot → Panic → Panic-Reboot
  → sauberer Reboot, prueft dass der Panic-Text nicht klebt).
- `boot_journal.py` — sammelt die Spuren und ruft die Lib:
  `start` (Boot), `heartbeat` (Timer, jede Minute), `mark-clean` (Shutdown),
  `show` (Bericht).
- `boot-journal.service` — oneshot, `RemainAfterExit=yes`,
  `ExecStart=... start`, `ExecStop=... mark-clean`.
- `boot-journal-heartbeat.{service,timer}` — Timer `OnBootSec=45s`,
  `OnUnitActiveSec=60s`. Der Heartbeat schreibt die letzte bekannte Uptime, damit
  der **naechste** Boot weiss, wie lange der vorige lief.
- Zustand: `/var/lib/boot-journal/state.json` (atomar via `os.replace` + `fsync`).

## Installation

```sh
deploy_boot_journal.sh
```

Reboot-fest (`enable`), `sync` am Ende (ext4 `commit=120`).

## Abfrage

```sh
python3 /usr/local/sbin/boot_journal.py show
cat /var/lib/boot-journal/state.json
```

## Grenzen (ehrlich)

- Der **Kernel-Log-Pfad** (`journalctl -b -1 -k`) greift nur, wenn der Panic
  zufaellig noch geflusht wurde - journald flusht das Kernel-Log nicht vor dem
  Freeze. Die **primaere** Panic-Spur ist daher jetzt pstore/ramoops
  (`RAMOOPS-README.md`), das das Panic-Dmesg aus dem reservierten RAM liest.
- Ein **Stromausfall** sieht wie ein Hard-Reset aus (kein Marker) - das ist
  unvermeidbar ohne Hardware-Reset-Register.
- Der Clean-Marker stammt aus `ExecStop`; wird der Dienst per `SIGKILL`
  abgeschossen, fehlt er (dann faelschlich "hard-reset"). Bei einem echten
  Reboot/Shutdown laeuft `ExecStop` zuverlaessig.
- pstore-Dumps verfallen nach `PSTORE_MAX_AGE_S` (15 min) - ein Panic-Reboot
  sollte also zeitnah erfolgen, damit der Dump als frisch gilt.

## Verifiziert auf dem Board (2026-10-17)

- Sauberer Reboot (`systemctl reboot`) -> `clean-reboot`,
  `prev_uptime=2323s` (exakt der letzte Heartbeat).
- Kernel-Panic (`echo c > /proc/sysrq-trigger`, `panic=10`) -> Boot
  `kernel-panic` mit Text `Kernel panic - not syncing: sysrq triggered crash`
  aus pstore.
- **Dedupe real:** Panic-Reboot -> `kernel-panic` (Text). Danach **zwei**
  saubere Reboots -> beide `clean-reboot` **ohne** Panic-Text; die
  `consumed_pstore_mtime` blieb unveraendert (der alte Dump ueberschrieb die
  neuere Marke nicht mehr - genau der Bug, den der Mehrfach-Dump-Fall
  `dmesg.txt` (aelter) neben `dmesg-ramoops-0` (neuer) ausgeloest hatte).
