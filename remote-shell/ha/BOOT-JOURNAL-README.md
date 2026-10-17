# Boot-Journal — Reset-Zaehler mit Grund (Odroid N2)

Die Amlogic-Firmware liefert **keinen** Reset-Grund:

- `/sys/class/watchdog/watchdog0/bootstatus` ist **immer 0** (auch nach
  Watchdog-Reset) — die GXBB-Watchdog-Treibervariante meldet ihn nicht.
- `pstore`/`ramoops` ist nicht eingerichtet (`/sys/fs/pstore` bleibt leer).
- `/var/log` ist zram; das Journal ist zwar nach `/var/log.hdd/journal`
  symlinkt (persistent), bricht aber bei jedem Hard-Reset **abrupt** ab — es
  fehlt `Reached target Shutdown`. Ein Auswerten von `journalctl --list-boots`
  allein liefert daher **keinen** Grund, nur die Beobachtung "kein sauberer
  Abschied".

Deshalb zaehlt dieses Journal die Boots **selbst** und leitet den Grund aus drei
Spuren ab (`boot_journal_lib.classify_reset`, Prioritaet von oben):

| Spur | Grund | Quelle |
|------|-------|--------|
| Clean-Marker gesetzt | `clean-reboot` | `ExecStop` von `boot-journal.service` (`systemd-shutdown`-Pfad) |
| `watchdog0/bootstatus` != 0 | `watchdog-reset` | sysfs (auf Amlogic ungenutzt, portabel mitgenommen) |
| "Kernel panic" im vorigen Kernel-Log | `kernel-panic` | `journalctl -b -1 -k` |
| voriger Lauf < 60 s | `early-reset` | Heartbeat (`last_seen_uptime_s`) |
| sonst | `hard-reset-unknown` | Hang / Panic / Stromausfall ohne Spur |

## Aufbau

- `boot_journal_lib.py` — reine Logik (Klassifikation + Zustandsformat),
  **host-getestet** (`test_boot_journal_lib.py`, 22 Checks).
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

- Ein **Kernel-Panic schreibt sich nicht ins Journal**: journald flusht das
  Kernel-Log nicht vor dem Freeze. Der Panic landet daher als `early-reset`
  (wenn der Lauf kurz war) oder `hard-reset-unknown` — **nicht** als
  `kernel-panic`. Die `kernel-panic`-Stufe greift nur, wenn der Panic zufaellig
  noch geflusht wurde. Wer Panics sicher zuordnen will, braucht `ramoops`
  (reserved-memory im DTB + `pstore`), das hier nicht vorhanden ist.
- Ein **Stromausfall** sieht wie ein Hard-Reset aus (kein Marker) — das ist
  unvermeidbar ohne Hardware-Reset-Register.
- Der Clean-Marker stammt aus `ExecStop`; wird der Dienst per `SIGKILL`
  abgeschossen, fehlt er (dann faelschlich "hard-reset"). Bei einem echten
  Reboot/Shutdown laeuft `ExecStop` zuverlaessig.

## Verifiziert auf dem Board (2026-10-17)

- Sauberer Reboot (`systemctl reboot`) -> Boot #2 `clean-reboot`,
  `prev_uptime=2323s` (exakt der letzte Heartbeat). ✅
- Kernel-Panic (`echo c > /proc/sysrq-trigger`, `panic=10`) -> Boot #3
  `early-reset`, `prev_uptime=47s`. Kein Panic im Journal (siehe Grenzen). ✅
