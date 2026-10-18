#!/usr/bin/env python3
"""Host test for the Brother HL-3040CN container config (no board, no docker).

The one thing that decides whether this works is the filter chain: CUPS must
route the job through the proprietary i386 filter, and every helper that filter
shells out to (ghostscript, a2ps, file, poppler for PDF) must be present. These
checks pin that down from the source files so a config edit cannot silently
break printing.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
checks = 0
fails = 0


def ok(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL %s" % what)


def read(name):
    with open(os.path.join(HERE, "brother-hl3040cn", name)) as fh:
        return fh.read()


dockerfile = read("Dockerfile")
cupsd = read("cupsd.conf")
entry = read("entrypoint.sh")
deploy = read("deploy_brother_cups.sh")

# --- Dockerfile: i386 base + the whole filter toolchain ---
ok("FROM i386/debian" in dockerfile, "image is i386 (filter is an i386 ELF)")
ok("cups" in dockerfile, "CUPS installed")
ok("ghostscript" in dockerfile, "ghostscript installed (psconvertij2 calls gs)")
ok("a2ps" in dockerfile, "a2ps installed (filterhl3040cn non-PS path)")
ok("file" in dockerfile, "file installed (filter detects input type)")
ok("poppler-utils" in dockerfile, "poppler-utils installed (PDF -> PS before filter)")
ok("dlf005905/hl3040cnlpr" in dockerfile, "LPR deb (contains brhl3040cnfilter) fetched")
ok("dlf005907/hl3040cncupswrapper" in dockerfile, "CUPSwrapper deb fetched")
# No proprietary blob vendored: both debs come from Brother at build time.
ok("COPY hl3040cn" not in dockerfile and "COPY *.deb" not in dockerfile,
   "no proprietary blob vendored in the repo")
# The install step must fail loudly if the filter did not land.
ok("/usr/lib/cups/filter/brlpdwrapperhl3040cn" in dockerfile,
   "build asserts the wrapper filter exists")
ok("brother_hl3040cn_printer_en.ppd" in dockerfile,
   "build asserts the PPD exists")

# --- cupsd.conf: reachable on the LAN, no auth by design ---
ok("Listen 0.0.0.0:631" in cupsd, "cupsd listens on all interfaces")
ok("ServerAlias *" in cupsd, "reachable under any client hostname")
ok("Allow all" in cupsd, "LAN clients allowed (print server, by design)")
# dnssd would need avahi in the container; we do not ship it.
ok("BrowseLocalProtocols none" in cupsd, "no avahi dependency for browsing")

# --- entrypoint: correct device URI, PPD, PDF path ---
ok("socket://${PRINTER_IP}:${PRINTER_PORT}" in entry, "uses socket:// to the printer")
ok("brother_hl3040cn_printer_en.ppd" in entry, "uses the Brother PPD")
ok("pdftops-renderer-default=gs" in entry,
   "PDFs rendered by gs (needed for the filter chain)")
ok("cupsd -f &" in entry and "wait " in entry,
   "cupsd runs in the foreground (single instance)")

# --- deploy script: emulation prerequisite + persistence ---
ok("qemu-user-static" in deploy and "binfmt-support" in deploy,
   "deploy installs the i386 emulator")
ok("--platform linux/386" in deploy, "deploy tests i386 emulation before building")
ok("docker build --network=host" in deploy,
   "build uses host network (bridge DNS is broken on the board)")
ok("--network=host" in deploy, "container uses host networking (bridge cannot reach the LAN)")
ok("--restart unless-stopped" in deploy, "container restarts automatically")

print("test_brother_cups: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
