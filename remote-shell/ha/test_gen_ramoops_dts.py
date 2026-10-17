#!/usr/bin/env python3
"""Host-Tests fuer gen_ramoops_dts.py (reine Text-/Adresslogik, kein Board).

Der wichtigste Test ist die ADRESSE: eine syntaktisch gueltige, aber falsche
Adresse kann das Board in einen Boot-Loop schicken. Deshalb prueft der Test
explizit, dass die Default-Adresse im freien DRAM-Fenster liegt und mit keiner
bekannten Reservierung kollidiert (Werte aus /proc/iomem + memblock des Boards).
"""

import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_ramoops_dts as g  # noqa: E402

CHECKS = 0
FAILURES = 0

# Aus /proc/iomem + /sys/kernel/debug/memblock des Odroid N2 (2026-10-17):
# freies, nicht reserviertes DRAM-Fenster.
FREE_START = 0x40000000
FREE_END = 0xDFD2DFFF
# Reservierte Bereiche, die die Adresse NICHT treffen darf.
RESERVED = [
    (0x05000000, 0x072FFFFF),   # secmon / BL31
    (0x34000000, 0x35FCFFFF),   # Kernel code
    (0x36260000, 0x365AFFFF),   # Kernel data
    (0x3EAFC000, 0x3EB1107F),   # diverse
    (0xDFD2E000, 0xF4E5AFFF),   # Kernel-Alloc + CMA (256M)
    (0x32000000, 0x3200FFFF),   # U-Boot scriptaddr
    (0x04080000, 0x0408FFFF),   # fdt_addr_r
]


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def test_address_is_safe():
    a = g.DEFAULT_ADDRESS
    s = g.DEFAULT_SIZE
    check(FREE_START <= a and a + s - 1 <= FREE_END,
          'Default-Adresse 0x%x+0x%x liegt im freien Fenster' % (a, s))
    for lo, hi in RESERVED:
        check(not (a <= hi and a + s - 1 >= lo),
              'Default-Adresse ueberlappt Reservierung 0x%x..0x%x' % (lo, hi))
    # Aligniert und gross genug fuer record+console.
    check(a % 0x10000 == 0, 'Adresse 64K-aligniert')
    check(g.DEFAULT_RECORD + g.DEFAULT_CONSOLE <= s,
          'record+console passen in size')


def test_cells():
    check(g.cells(0x50000000) == '0x0 0x50000000', 'cells 64-bit korrekt')
    check(g.cells(0x100000) == '0x0 0x100000', 'cells kleine Zahl')
    check(g.cells(0x1_00000000) == '0x1 0x0', 'cells > 32 bit')


def test_render_content():
    txt = g.render()
    check('/plugin/;' in txt, 'Plugin-Marker vorhanden')
    check('target-path = "/reserved-memory";' in txt, 'Zielpfad korrekt')
    check('compatible = "ramoops";' in txt, 'compatible gesetzt')
    check('reg = <0x0 0x50000000 0x0 0x100000>;' in txt, 'reg korrekt gerendert')
    check('record-size = <0x20000>;' in txt, 'record-size gesetzt')
    check('console-size = <0x20000>;' in txt, 'console-size gesetzt')
    check('max-reason = <0x2>;' in txt, 'max-reason gesetzt')
    check('#address-cells = <0x2>;' in txt, 'address-cells gesetzt')
    check('#size-cells = <0x2>;' in txt, 'size-cells gesetzt')


def test_render_rejects_oversize():
    try:
        g.render(size=0x1000, record_size=0x2000, console_size=0x2000)
        check(False, 'zu grosses record+console muss ValueError werfen')
    except ValueError:
        check(True, 'zu grosses record+console wird abgelehnt')


def test_dtc_compiles_and_merges():
    """Wenn dtc da ist: uebersetzen und gegen ein Mini-Basis-DTB mergen."""
    if not _have('dtc'):
        print('  info: dtc fehlt, Uebersetzungstest uebersprungen')
        return
    with tempfile.TemporaryDirectory() as d:
        dts = os.path.join(d, 'r.dts')
        dtbo = os.path.join(d, 'r.dtbo')
        with open(dts, 'w') as fh:
            fh.write(g.render())
        try:
            g.compile_dts(dts, dtbo)
        except subprocess.CalledProcessError:
            check(False, 'dtc-Uebersetzung fehlgeschlagen')
            return
        check(os.path.getsize(dtbo) > 0, 'dtbo erzeugt')

        # Mini-Basis-DTB mit /reserved-memory bauen und Overlay anwenden.
        base_dts = os.path.join(d, 'base.dts')
        base_dtb = os.path.join(d, 'base.dtb')
        merged = os.path.join(d, 'merged.dtb')
        with open(base_dts, 'w') as fh:
            fh.write('/dts-v1/;\n/ {\n  #address-cells = <2>;\n'
                     '  #size-cells = <2>;\n  reserved-memory {\n'
                     '    #address-cells = <2>;\n    #size-cells = <2>;\n'
                     '    ranges;\n  };\n};\n')
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', base_dtb,
                        base_dts], check=True)
        if _have('fdtoverlay'):
            subprocess.run(['fdtoverlay', '-i', base_dtb, '-o', merged, dtbo],
                           check=True)
            out = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', merged],
                                 capture_output=True, text=True).stdout
            check('ramoops@50000000' in out, 'ramoops-Knoten nach Merge da')
            check('compatible = "ramoops"' in out, 'compatible nach Merge da')
        else:
            print('  info: fdtoverlay fehlt, Merge-Test uebersprungen')


def _have(tool):
    from shutil import which
    return which(tool) is not None


def main():
    test_address_is_safe()
    test_cells()
    test_render_content()
    test_render_rejects_oversize()
    test_dtc_compiles_and_merges()
    print('gen_ramoops_dts: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
