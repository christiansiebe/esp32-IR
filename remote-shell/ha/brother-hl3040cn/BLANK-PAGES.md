# Brother HL-3040CN: why the print server prints blank pages

The Odroid N2+ runs the HL-3040CN as an i386 CUPS container
(`brother-hl3040cn`, see `deploy_brother_cups.sh`). The printer is
**host-based (GDI)**: it understands neither PCL6 nor PostScript, only
Brother's own proprietary stream. Everything that turns a PDF into that stream
happens in the container, on top of qemu-i386. If any link in that chain
breaks, the printer gets data it cannot render and the page comes out blank.
The queue can also look like it "spins" (jobs accepted, nothing printed) when
the container is down or the printer is unreachable.

## One command to find the cause

Run this on the Odroid (needs docker + the container; read-only):

```sh
remote-shell/ha/brother-hl3040cn/diagnose_print.sh [PRINTER_IP]
```

It inspects the container, the qemu-i386 binfmt registration, the CUPS filter /
PPD / queue and the printer's reachability, then prints **VERDICT / CAUSE /
FIX**. The mapping from signals to cause lives in the pure, host-tested
`print_health.py` (`remote-shell/ha/test_print_health.py`).

## The failure modes it distinguishes

| Symptom | Cause | Fix |
|---------|-------|-----|
| blank pages, queue "spins" | container `brother-cups` not running | `docker start brother-cups` |
| blank pages | **qemu-i386 binfmt lost** (reboot/kernel update) -> i386 filter cannot run | install `qemu-user-static binfmt-support`, restart `systemd-binfmt`, restart container |
| blank pages | filter `brlpdwrapperhl3040cn` missing from the image | rebuild via `deploy_brother_cups.sh` |
| blank pages | PPD missing -> generic driver | rebuild via `deploy_brother_cups.sh` |
| CUPS log `Exec format error` | i386 filter ran without qemu-i386 | re-register binfmt, restart container |
| CUPS log `Filter failed` | `gs`/`psconvertij2`/`brhl3040cnfilter` failed | read `docker exec brother-cups tail /var/log/cups/error_log` |
| job "spins", nothing prints | printer unreachable on TCP 9100 | check the printer's power + IP (DHCP lease) |
| job "spins" | queue disabled / not accepting | `cupsenable` / `cupsaccept HL3040CN` |
| blank page | last stream < 1 kB (empty) | restart container; check binfmt |
| blank page | last stream does **not** start with `@PJL` (raw/PostScript job) | print through the CUPS queue on port 631, **not** to the printer's 9100 |

## Verified-good baseline

`de304ff` proved the chain end to end on the board: a 121 B PostScript page
became **112,587 B** of real Brother data (`@PJL JOB ... UEL`) and the printer
completed the job. A stream that is far smaller than that, or that does not
begin with `@PJL`, did not go through the Brother filter - which is exactly
what a blank page looks like.

## Two operational notes

- **Host networking is required** (also `--network=host` for the *build*). On
  the default bridge the container cannot reach the printer at all (the host
  FORWARD policy drops it), and the build cannot resolve `download.brother.com`.
- **The queue is unauthenticated by design** (LAN-only print server). Printing
  directly to the printer's own port 9100, or to its `PCL_P1`/`POSTSCRIPT_P1`
  "fake" ports, bypasses the filter and yields blank pages.

## Investigating from the sandbox (why this is a script, not a remote run)

The Odroid is reachable from the sandbox only through the reverse channel
(`remote-shell/`). That channel's agent runs with a **fail-closed allowlist**
(`cmd_policy.py` + `odroid_allowlist.txt`), which permits only a small set of
read-only probes (`hostname`, `id`, `uptime`, `uname -a`, `systemctl status`,
`journalctl -u`, ...). `docker`, `lpstat` and the CUPS log are **blocked**, so
the container internals cannot be inspected from here. `diagnose_print.sh`
therefore runs on the Odroid itself, where those tools are available; this
document and `print_health.py` are what the sandbox could establish without
them.
