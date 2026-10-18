# Brother HL-3040CN print server for ARM (Odroid N2+, arm64).
#
# The HL-3040CN is a host-based (GDI) printer: it understands neither PCL6 nor
# PostScript, so generic drivers only emit blank pages. Brother ships its Linux
# driver for i386/x86_64 only (no ARM, no source), so the proprietary filter
# cannot be ported. This image runs that i386 filter under qemu user emulation
# (binfmt_misc) inside an i386 CUPS container - the only working path on ARM.
#
# The image downloads the official Brother debs at build time, so no
# proprietary blob is vendored in this repository.
FROM i386/debian:bookworm-slim

ARG DEB_LPR=https://download.brother.com/welcome/dlf005905/hl3040cnlpr-1.1.2-1.i386.deb
ARG DEB_CUPS=https://download.brother.com/welcome/dlf005907/hl3040cncupswrapper-1.1.2-2.i386.deb

# CUPS + the filter chain the Brother wrapper shells out to:
#   brlpdwrapperhl3040cn -> filterhl3040cn -> psconvertij2 (ghostscript) | brhl3040cnfilter
# psconvertij2 needs gs; filterhl3040cn needs `file` and a2ps for non-PS input;
# CUPS needs pdftops (poppler-utils) to turn PDFs into PS before the filter runs.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        cups cups-daemon cups-client cups-filters \
        ghostscript a2ps file psutils poppler-utils \
        ca-certificates curl; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    curl -fsSL -A "Mozilla/5.0" -o /tmp/lpr.deb  "$DEB_LPR"; \
    curl -fsSL -A "Mozilla/5.0" -o /tmp/cups.deb "$DEB_CUPS"; \
    dpkg-deb -x /tmp/lpr.deb  /; \
    dpkg-deb -x /tmp/cups.deb /; \
    rm -f /tmp/lpr.deb /tmp/cups.deb; \
    /opt/brother/Printers/hl3040cn/cupswrapper/cupswrapperhl3040cn || true; \
    test -x /usr/lib/cups/filter/brlpdwrapperhl3040cn; \
    test -f /usr/share/ppd/Brother/brother_hl3040cn_printer_en.ppd

COPY cupsd.conf /etc/cups/cupsd.conf
COPY entrypoint.sh /usr/local/bin/brother-cups-entrypoint.sh
RUN chmod 0755 /usr/local/bin/brother-cups-entrypoint.sh

ENV PRINTER_IP=192.168.178.50 \
    PRINTER_PORT=9100 \
    QUEUE=HL3040CN

EXPOSE 631
ENTRYPOINT ["/usr/local/bin/brother-cups-entrypoint.sh"]
