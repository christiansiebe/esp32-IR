#!/usr/bin/env python3
"""Pure classifier for the Brother HL-3040CN print server.

Why this exists
---------------
The HL-3040CN is a host-based (GDI) printer: it understands neither PCL6 nor
PostScript, only Brother's own proprietary stream. The print server therefore
has to run Brother's i386-only filter under qemu-i386 inside an i386 CUPS
container. When any link in that chain breaks, the printer receives data it
cannot interpret - and the page comes out **blank**. The queue can also look
like it is "spinning" (jobs accepted, nothing printed) when the container is
down or the printer is unreachable.

The failure modes all look identical from the outside ("blank pages"), so this
module turns the raw probe signals into one concrete cause plus the fix. It is
deliberately free of any I/O so the mapping is host-tested
(``remote-shell/ha/test_print_health.py``); ``diagnose_print.sh`` gathers the
signals on the Odroid and feeds them in.

Signals (all optional; a missing key is treated as "unknown"):

    container_running   bool  docker inspect -> State.Running
    container_restarts  int   docker inspect -> RestartCount
    binfmt_ok           bool  /proc/sys/fs/binfmt_misc/qemu-i386 is enabled
    filter_present      bool  /usr/lib/cups/filter/brlpdwrapperhl3040cn in the container
    ppd_present         bool  brother_hl3040cn_printer_en.ppd in the container
    queue_state         str   lpstat -p -> "idle" / "disabled" / ""
    accepting           bool  lpstat -a -> accepting requests
    printer_reachable   bool  TCP connect to <printer>:9100
    cups_log            str   tail of /var/log/cups/error_log inside the container
    last_stream_bytes   int   size of the Brother stream of the last completed job
    last_stream_pjl     bool  that stream starts with "@PJL" (real Brother data)
"""
import re

# A job that produced this few bytes cannot be a rendered page; the smallest
# real Brother page observed on the board was ~112 kB (see de304ff).
MIN_STREAM_BYTES = 1024


def _known(value):
    return value is not None


def classify(sig):
    """Map probe signals to (verdict, cause, fixes).

    Returns a dict:
        verdict  short one-line summary
        cause    the single most likely root cause
        fixes    ordered list of concrete next steps
    """
    fixes = []

    # 1. Container down: the queue accepts jobs but nothing drains them -> the
    #    job "spins" and, if a raw fallback ever runs, blank pages.
    if sig.get("container_running") is False:
        return {
            "verdict": "print container is DOWN",
            "cause": ("the brother-cups container is not running, so CUPS never "
                      "renders the job; the queue stalls ('spinning')."),
            "fixes": [
                "docker start brother-cups   # or re-run deploy_brother_cups.sh",
                "docker logs --tail 40 brother-cups   # why it exited",
            ],
        }

    # 2. qemu-i386 not registered: the i386 filter cannot be executed, CUPS
    #    falls through to an empty/raw stream -> blank page. This is the classic
    #    post-reboot / post-kernel-update failure.
    if sig.get("binfmt_ok") is False:
        return {
            "verdict": "qemu-i386 emulation is NOT registered",
            "cause": ("binfmt_misc has no qemu-i386 handler, so the i386 Brother "
                      "filter cannot run; CUPS emits nothing usable and the page "
                      "is blank."),
            "fixes": [
                "apt-get install -y qemu-user-static binfmt-support",
                "systemctl restart systemd-binfmt binfmt-support",
                "ls /proc/sys/fs/binfmt_misc/qemu-i386   # must exist and be 'enabled'",
                "docker restart brother-cups",
            ],
        }

    # 3. Filter missing from the image: jobs pass through unrendered -> blank.
    if sig.get("filter_present") is False:
        return {
            "verdict": "Brother filter is missing in the container",
            "cause": ("/usr/lib/cups/filter/brlpdwrapperhl3040cn is absent, so CUPS "
                      "has no way to turn the job into Brother's stream -> blank."),
            "fixes": [
                "docker rm -f brother-cups && re-run deploy_brother_cups.sh "
                "(the image fetches the Brother debs at build time)",
            ],
        }

    # 4. PPD missing: CUPS falls back to a generic/raw driver -> blank.
    if sig.get("ppd_present") is False:
        return {
            "verdict": "Brother PPD is missing in the container",
            "cause": ("brother_hl3040cn_printer_en.ppd is absent, so the queue uses "
                      "a generic driver the GDI printer cannot understand -> blank."),
            "fixes": [
                "docker rm -f brother-cups && re-run deploy_brother_cups.sh",
            ],
        }

    log = (sig.get("cups_log") or "")
    low = log.lower()

    # 5. The CUPS log is the most direct evidence when present.
    if "exec format error" in low or "cannot execute binary file" in low:
        return {
            "verdict": "CUPS could not execute the i386 filter",
            "cause": ("the CUPS log shows an 'exec format error': the i386 filter "
                      "ran without qemu-i386, so no Brother stream was produced "
                      "(blank page)."),
            "fixes": [
                "ls /proc/sys/fs/binfmt_misc/qemu-i386   # re-register if missing",
                "apt-get install -y qemu-user-static binfmt-support && "
                "systemctl restart systemd-binfmt",
                "docker restart brother-cups",
            ],
        }
    if "unable to open ppd" in low or "no ppd" in low:
        return {
            "verdict": "CUPS has no PPD for the queue",
            "cause": "the CUPS log reports a missing PPD -> generic driver -> blank.",
            "fixes": ["docker rm -f brother-cups && re-run deploy_brother_cups.sh"],
        }
    if re.search(r"filter failed|filter error|filter .* crashed", low):
        return {
            "verdict": "the CUPS filter chain failed",
            "cause": ("a filter in the chain (gs / psconvertij2 / brhl3040cnfilter) "
                      "reported failure, so the job was truncated -> blank page."),
            "fixes": [
                "docker logs --tail 60 brother-cups",
                "docker exec brother-cups tail -n 80 /var/log/cups/error_log",
            ],
        }

    # 6. Printer unreachable: the queue holds the job and retries -> "spinning".
    if sig.get("printer_reachable") is False:
        return {
            "verdict": "printer is unreachable on the LAN",
            "cause": ("TCP 9100 to the printer does not connect, so CUPS keeps the "
                      "job queued and retries - it looks like the server spins."),
            "fixes": [
                "ping 192.168.178.50   # is the printer powered and on the LAN?",
                "check the printer's own IP (its web UI) - it may have a new DHCP lease",
            ],
        }

    # 7. Queue disabled / not accepting: jobs stay queued.
    if sig.get("queue_state") == "disabled" or sig.get("accepting") is False:
        return {
            "verdict": "the print queue is disabled or not accepting",
            "cause": "the HL3040CN queue was stopped or is rejecting jobs.",
            "fixes": [
                "docker exec brother-cups cupsenable HL3040CN",
                "docker exec brother-cups cupsaccept HL3040CN",
            ],
        }

    # 8. The last job produced a stream, but it is not real Brother data.
    if _known(sig.get("last_stream_bytes")):
        nbytes = sig.get("last_stream_bytes") or 0
        if nbytes < MIN_STREAM_BYTES:
            return {
                "verdict": "the last job produced (almost) no data",
                "cause": (f"only {nbytes} B of Brother stream were generated for the "
                          "last job; the printer got an empty page."),
                "fixes": [
                    "ls /proc/sys/fs/binfmt_misc/qemu-i386   # filter may be dead",
                    "docker restart brother-cups",
                ],
            }
        if sig.get("last_stream_pjl") is False:
            return {
                "verdict": "the last job was NOT a Brother stream",
                "cause": ("the data sent to the printer does not start with '@PJL', "
                          "i.e. it is raw/PostScript the GDI printer cannot render "
                          "-> blank page. The job bypassed the Brother filter."),
                "fixes": [
                    "print through the CUPS queue (lp -h <odroid>:631 -d HL3040CN FILE), "
                    "not directly to the printer's port 9100",
                    "do NOT use the printer's PCL_P1/POSTSCRIPT_P1 'fake' ports",
                ],
            }

    return {
        "verdict": "no fault detected in the print chain",
        "cause": ("the container, qemu-i386, filter, PPD, queue and printer all look "
                  "healthy; a blank page now would point at the client (generic "
                  "driver / raw job) or a firmware-side issue."),
        "fixes": [
            "reprint a known-good PDF through the CUPS queue and watch "
            "docker logs -f brother-cups",
        ],
    }
