# ramoops / pstore — Kernel-Panics ueberleben den Warm-Reset (Odroid N2+)

Ohne ramoops war ein Kernel-Panic **nicht** diagnostizierbar: das Journal bricht
beim Freeze ab (journald flusht nicht), `watchdog0/bootstatus` ist auf Amlogic
immer 0, und `/sys/fs/pstore` blieb leer. Der Panic landete hoechstens als
`early-reset` oder `hard-reset-unknown` (siehe `BOOT-JOURNAL-README.md`).

ramoops loest das: der Kernel reserviert ein Stueck **RAM**, das ein Warm-Reset
nicht loescht, und legt dort das Panic-Dmesg ab. Nach dem naechsten Boot liest
`boot_journal.py` den Text und klassifiziert den Reset als `kernel-panic`.

## Warum ein Device-Tree-Overlay

- Das Amlogic-G12B-DTB bringt **keinen** ramoops-Knoten mit; der Kernel braucht
  aber eine `reserved-memory`-Region, die er nicht selbst anlegen kann (sonst
  koennte er sie nicht als reserviert markieren).
- Das Projekt nutzt bereits Overlays (`user_overlays=lowfreq`), und U-Boots
  `boot.cmd` stellt bei fehlgeschlagenem `fdt apply` den **Original-DT** wieder
  her — ein kaputtes Overlay kann das Board also **nicht** bricken.

## Speicheradresse (0x50000000)

Die Region darf **nicht** mit Kernel, secmon oder CMA kollidieren. Aus
`/proc/iomem` + memblock des Boards (2026-10-17):

| Bereich | Zweck |
|---------|-------|
| 0x05000000–0x072fffff | secmon / BL31 (reserviert) |
| 0x32000000 | U-Boot `scriptaddr` |
| 0x34000000–0x35fcffff | Kernel code |
| 0x36260000–0x365affff | Kernel data |
| 0xe4c00000–0xf4bfffff | CMA (256 MiB) |
| 0xf4e5b000 | framebuffer |

Freies, nicht reserviertes DRAM-Fenster: **0x40000000–0xdfd2dfff**.

Gewaehlt: **0x50000000, Groesse 0x100000 (1 MiB)** — mitten im freien Fenster,
weit weg von secmon (`0x05000000`, nicht verwechseln!) und vom Framebuffer.
`gen_ramoops_dts.py` erzeugt das Overlay; `test_gen_ramoops_dts.py` prueft die
Adresse gegen die Reservierungsliste (26 Checks).

## Aufbau

- `gen_ramoops_dts.py` — erzeugt das Overlay-DTS (`render()`), optional kompiliert
  (`--compile`). Die Adresse/Reservierungs-Analyse steht im Docstring.
- `ramoops.dts` / `ramoops.dtbo` — erzeugtes Overlay (Fragment auf
  `/reserved-memory`, Knoten `ramoops@50000000`, `compatible = "ramoops"`,
  `record-size`/`console-size` je 128 KiB).
- `deploy_ramoops.sh` — kompiliert, validiert gegen das laufende DTB
  (`fdtoverlay`), installiert nach `/boot/overlay-user/`, ergaenzt
  `user_overlays` (laesst `lowfreq` stehen!), legt
  `/etc/modules-load.d/ramoops.conf` an, `sync`.
- `revert_ramoops.sh` — nimmt `ramoops` wieder aus `user_overlays` (behaelt
  `lowfreq`) und entfernt das Overlay.
- `test_gen_ramoops_dts.py`, `test_boot_journal_pstore.py`,
  `test_boot_journal_flow.py` — Host-Tests.

## Installation

```sh
deploy_ramoops.sh      # installiert Overlay + user_overlays
reboot                 # Overlay greift erst nach dem Neustart
```

Pruefen nach dem Reboot:

```sh
dmesg | grep ramoops           # "reserved mem: ... ramoops@50000000"
lsmod | grep ramoops           # Modul geladen
```

## Wie der Panic gelesen wird

- ramoops schreibt das Panic-Dmesg in die reservierte Region.
- `systemd-pstore.service` archiviert es beim Boot nach
  `/var/lib/systemd/pstore/dmesg-ramoops-N` und **leert** `/sys/fs/pstore`.
  `boot_journal.py` liest **beide** Orte.
- **Dedupe:** ramoops und `systemd-pstore` behalten alte Dumps. `boot_journal`
  merkt sich die mtime des zuletzt verbrauchten Dumps (monoton in
  `consumed_pstore_mtime`) und ignoriert jeden Dump, der nicht juenger ist. So
  traegt nur der echte Panic-Reboot den Text — ein alter Dump klebt nicht an
  spaeteren Boots. Dumps aelter als `PSTORE_MAX_AGE_S` (15 min) zaehlen nicht.

## Grenzen (ehrlich)

- **Nur Warm-Reset.** Ein Kaltstart/Stromausfall loescht den RAM — ramoops hilft
  nur, wenn die Panic-Region den Reset ueberlebt (Warm-Reset/Watchdog). Bei
  echtem Power-Cycle ist die Spur weg.
- `# CONFIG_PSTORE_CONSOLE is not set` in diesem Kernel: ramoops schreibt den
  **Panic-Record** (`dmesg-ramoops`), nicht den vollen Konsolen-Log. Fuer Panics
  reicht das.
- Das Modul wird ueber den OF-Alias `of:N*T*Cramoops[C]` autoloaded;
  `/etc/modules-load.d/ramoops.conf` ist nur ein Guertel-und-Hosentraeger.

## Verifiziert auf dem Board (2026-10-17)

- `dmesg`: `reserved mem: 0x0000000050000000..0x00000000500fffff (1024 KiB)
  map non-reusable ramoops@50000000`. ✅
- `lsmod`: `ramoops` + `reed_solomon` geladen. ✅
- Panic (`echo c > /proc/sysrq-trigger`): `/var/lib/systemd/pstore/dmesg-ramoops-0`
  (53 KiB) enthaelt `Kernel panic - not syncing: sysrq triggered crash` +
  `Call trace` (`sysrq_handle_crash` …). ✅
- `boot_journal.py show`: Boot `kernel-panic` mit genau diesem Text. ✅
- Dedupe: nach dem Panic zwei saubere Reboots -> beide `clean-reboot` ohne
  Panic-Text, `consumed_pstore_mtime` unveraendert. ✅
