#!/usr/bin/env python3
"""Dump a physical register block via /dev/mem (read-only helper).

Usage: regdump.py <base-hex> [size-hex]
The file object is kept referenced: an inline open().fileno() is garbage
collected at once, which closes the fd before mmap() can use it.
"""
import mmap
import struct
import sys

base = int(sys.argv[1], 16)
size = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1000

fd = open('/dev/mem', 'rb+', buffering=0)
m = mmap.mmap(fd.fileno(), size, offset=base)
for off in range(0, size, 4):
    print('%04x %08x' % (off, struct.unpack('<I', m[off:off + 4])[0]))
m.close()
fd.close()
