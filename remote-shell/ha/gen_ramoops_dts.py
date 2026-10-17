#!/usr/bin/env python3
"""Erzeugt ramoops.dts: reserviert einen RAM-Bereich fuer pstore/ramoops.

Warum: Die Amlogic-Hardware liefert keinen Reset-Grund (`watchdog0/bootstatus`
ist immer 0, pstore leer) und ein Kernel-Panic schreibt sich nicht ins Journal
(journald flusht das Kernel-Log nicht vor dem Freeze). ramoops legt das
Panic-Dmesg in einen reservierten RAM-Bereich, der einen Warm-Reset
(Watchdog/Panic/`reboot`) ueberlebt - danach steht der Grund in /sys/fs/pstore.

Adresswahl (kritisch - eine falsche, aber syntaktisch gueltige Adresse kann das
Board in einen Boot-Loop schicken):

  * memblock zeigt 0x40000000..0xdfd2dfff als frei (kein reserved-Eintrag).
  * boot.scr laedt U-Boot-Images nur bei 0x32000000 (script), 0x34000000
    (kernel), ~0x3e000000 (initrd) und 0x4080000 (fdt) - alle < 0x40000000.
  * secmon/BL31: 0x5000000..0x72fffff; CMA: 0xe4c00000..0xf4bfffff.
  * Default 0x50000000 (1.25 GB) liegt mitten im freien Bereich und kollidiert
    mit nichts.

Das Overlay wendet U-Boot an; scheitert `fdt apply`, stellt boot.cmd das
Original-DT wieder her (`overlay_error`) - ein kaputtes Overlay brickt also
nicht. Nur eine falsche, aber gueltige Adresse ist riskant, deshalb steht sie
hier zentral und wird von test_gen_ramoops_dts.py geprueft.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_ADDRESS = 0x50000000
DEFAULT_SIZE = 0x100000
DEFAULT_RECORD = 0x20000
DEFAULT_CONSOLE = 0x20000
DEFAULT_MAX_REASON = 2  # KMSG_DUMP_OOPS (schliesst PANIC mit ein)


def cells(value, count=2):
    """Zerlegt einen 64-bit-Wert in `count` 32-bit-Zellen (Big-Endian)."""
    return " ".join("0x%x" % ((value >> (32 * (count - 1 - i))) & 0xffffffff)
                    for i in range(count))


def render(address=DEFAULT_ADDRESS, size=DEFAULT_SIZE,
           record_size=DEFAULT_RECORD, console_size=DEFAULT_CONSOLE,
           max_reason=DEFAULT_MAX_REASON):
    if record_size + console_size > size:
        raise ValueError("record+console (%d) > size (%d)" %
                         (record_size + console_size, size))
    reg = "%s %s" % (cells(address), cells(size))
    return "\n".join([
        "/dts-v1/;",
        "/plugin/;",
        "",
        "/*",
        " * Reserves RAM for pstore/ramoops so a kernel panic survives a warm",
        " * reset. 0x%x lies in the free DRAM window 0x40000000..0xdfd2dfff" % address,
        " * (see the generator docstring for the collision analysis).",
        " */",
        "/ {",
        "    fragment@0 {",
        '        target-path = "/reserved-memory";',
        "        __overlay__ {",
        "            #address-cells = <0x2>;",
        "            #size-cells = <0x2>;",
        "",
        "            ramoops@%x {" % address,
        '                compatible = "ramoops";',
        "                reg = <%s>;" % reg,
        "                record-size = <0x%x>;" % record_size,
        "                console-size = <0x%x>;" % console_size,
        "                max-reason = <0x%x>;" % max_reason,
        "            };",
        "        };",
        "    };",
        "};",
        "",
    ])


def compile_dts(dts_path, dtbo_path):
    subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb",
                    "-o", dtbo_path, dts_path], check=True)


def main():
    ap = argparse.ArgumentParser(description="ramoops-Overlay erzeugen")
    ap.add_argument("--address", type=lambda s: int(s, 0),
                    default=DEFAULT_ADDRESS)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=DEFAULT_SIZE)
    ap.add_argument("--record-size", type=lambda s: int(s, 0),
                    default=DEFAULT_RECORD)
    ap.add_argument("--console-size", type=lambda s: int(s, 0),
                    default=DEFAULT_CONSOLE)
    ap.add_argument("--out", default=os.path.join(HERE, "ramoops.dts"))
    ap.add_argument("--compile", action="store_true",
                    help="zusaetzlich mit dtc nach .dtbo uebersetzen")
    ap.add_argument("--print", dest="to_stdout", action="store_true")
    args = ap.parse_args()

    text = render(address=args.address, size=args.size,
                  record_size=args.record_size, console_size=args.console_size)
    if args.to_stdout:
        sys.stdout.write(text)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("geschrieben: %s (reg 0x%x +0x%x)" % (args.out, args.address, args.size))
    if args.compile:
        dtbo = os.path.splitext(args.out)[0] + ".dtbo"
        compile_dts(args.out, dtbo)
        print("kompiliert:  %s" % dtbo)
    return 0


if __name__ == "__main__":
    sys.exit(main())
