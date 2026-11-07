#!/usr/bin/env python3
"""Read, modify (preserving) and write back an ESP32 NVS partition image.

Lets you put the Wi-Fi credentials of the ESP32-C6 receiver straight into the
``nvs`` partition over USB - ``esptool write-flash 0x9000 <img>`` - instead of
re-flashing the whole firmware or bringing up the power-hungry setup AP.

Stdlib only (argparse/struct/zlib) - no ``esp-idf-nvs-partition-gen``, no
``pyusb``, no ``rich_click``. The on-flash layout is verified against a real
IDF-written image (page header CRC = ``zlib.crc32(hdr[4:28], 0xFFFFFFFF)``,
string entries carry a NUL and are padded with 0xFF).

Layout (NVS v2, 4096 B pages, 32 B entries):

  Page header (32 B): state@0 (u32, ACTIVE=0xFFFFFFFE / FULL=0xFFFFFFFC /
    EMPTY=0xFFFFFFFF), seq@4, version@8 (0xFE = v2), CRC32(header[4:28])@28.
  Entry bitmap (32 B) @32: 2 bits/entry, 0b10 = written, 0b11 = erased.
  Entries from offset 64, at most 126 per page. Entry: byte0 = namespace index,
    byte1 = type (0x01 u8/namespace, 0x21 string), byte2 = span (entries incl.
    data), byte3 = chunk index (0xFF), CRC32(bytes[0:4]+bytes[8:32])@4,
    key (NUL-padded, 16 B)@8. For a string: datalen (u16)@24, CRC32(data)@28;
    the NUL-terminated data follows, 0xFF-padded to ``span-1`` entries.

Usage:
    python3 tools/nvs_provision.py dump nvs.bin
    python3 tools/nvs_provision.py set nvs.bin metercfg \\
        wifi_ssid=Wohnzimmer wifi_pass=secret --out nvs-new.bin

Then: esptool --chip esp32c6 -p /dev/ttyACM0 write-flash 0x9000 nvs-new.bin
"""

import argparse
import os
import struct
import sys
import zlib

PAGE_SIZE = 4096
HEADER_SIZE = 32
BITMAP_OFFSET = 32
BITMAP_BYTES = 32
ENTRY_OFFSET = 64
ENTRY_SIZE = 32
MAX_ENTRIES = 126

STATE_EMPTY = 0xFFFFFFFF
STATE_ACTIVE = 0xFFFFFFFE
STATE_FULL = 0xFFFFFFFC

VERSION1 = 0xFF
VERSION2 = 0xFE

TYPE_U8 = 0x01
TYPE_I8 = 0x11
TYPE_U16 = 0x02
TYPE_I16 = 0x12
TYPE_U32 = 0x04
TYPE_I32 = 0x14
TYPE_U64 = 0x08
TYPE_I64 = 0x18
TYPE_SZ = 0x21
TYPE_BLOB = 0x41
TYPE_BLOB_DATA = 0x42
TYPE_BLOB_IDX = 0x48
CHUNK_ANY = 0xFF

ENTRY_ERASED = 0b11
ENTRY_WRITTEN = 0b10

MAX_KEY_LEN = 15
MAX_NS_LEN = 15
MAX_STRING_LEN = 4000  # v2 string limit

_PRIMITIVES = {
    TYPE_U8: ("<B", 1),
    TYPE_I8: ("<b", 1),
    TYPE_U16: ("<H", 2),
    TYPE_I16: ("<h", 2),
    TYPE_U32: ("<I", 4),
    TYPE_I32: ("<i", 4),
    TYPE_U64: ("<Q", 8),
    TYPE_I64: ("<q", 8),
}


def nvs_crc32(data):
    """CRC32 as used by NVS for page headers, entry headers and string data."""
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def entry_header_crc(entry):
    return nvs_crc32(bytes(entry[0:4]) + bytes(entry[8:32]))


def slot_state(page, index):
    return (page[BITMAP_OFFSET + index // 4] >> ((index % 4) * 2)) & 0b11


def set_slot_state(page, index, state):
    byte = BITMAP_OFFSET + index // 4
    shift = (index % 4) * 2
    page[byte] = (page[byte] & ~(0b11 << shift)) | (state << shift)


class PageError(Exception):
    pass


class Entry(object):
    __slots__ = ("page", "index", "namespace", "type", "span", "chunk",
                 "key", "crc_ok", "value", "raw")

    def __repr__(self):
        return (f"<Entry p{self.page} i{self.index} ns={self.namespace} "
                f"type=0x{self.type:02x} span={self.span} key={self.key!r}>")


def parse_entry(page_buf, page_no, index):
    raw = bytes(page_buf[ENTRY_OFFSET + index * ENTRY_SIZE:
                         ENTRY_OFFSET + (index + 1) * ENTRY_SIZE])
    entry = Entry()
    entry.page = page_no
    entry.index = index
    entry.namespace = raw[0]
    entry.type = raw[1]
    entry.span = raw[2]
    entry.chunk = raw[3]
    entry.crc_ok = struct.unpack_from("<I", raw, 4)[0] == entry_header_crc(raw)
    entry.key = raw[8:24].split(b"\x00", 1)[0].decode("utf-8", "replace")
    entry.raw = raw
    entry.value = _decode_value(page_buf, index, raw)
    return entry


def _decode_value(page_buf, index, raw):
    typ = raw[1]
    span = raw[2]
    if typ in _PRIMITIVES:
        fmt, _ = _PRIMITIVES[typ]
        return struct.unpack_from(fmt, raw, 24)[0]
    if typ == TYPE_SZ:
        datalen = struct.unpack_from("<H", raw, 24)[0]
        data_crc = struct.unpack_from("<I", raw, 28)[0]
        data = bytes(page_buf[ENTRY_OFFSET + (index + 1) * ENTRY_SIZE:
                              ENTRY_OFFSET + (index + 1) * ENTRY_SIZE + datalen])
        value = data.split(b"\x00", 1)[0].decode("utf-8", "replace")
        return _StrValue(value, datalen, data_crc, nvs_crc32(data))
    if typ == TYPE_BLOB_IDX:
        datalen = struct.unpack_from("<I", raw, 24)[0]
        return _BlobValue(datalen, raw[28], raw[29])
    return None


class _StrValue(object):
    __slots__ = ("text", "datalen", "stored_crc", "calc_crc")

    def __init__(self, text, datalen, stored_crc, calc_crc):
        self.text = text
        self.datalen = datalen
        self.stored_crc = stored_crc
        self.calc_crc = calc_crc

    @property
    def crc_ok(self):
        return self.stored_crc == self.calc_crc


class _BlobValue(object):
    __slots__ = ("datalen", "chunk_count", "chunk_start")

    def __init__(self, datalen, chunk_count, chunk_start):
        self.datalen = datalen
        self.chunk_count = chunk_count
        self.chunk_start = chunk_start


class Page(object):
    def __init__(self, buf, number):
        self.number = number
        self.buf = buf
        self.state, self.seq = struct.unpack_from("<II", buf, 0)
        self.version = buf[8]
        self.header_crc = struct.unpack_from("<I", buf, 28)[0]
        self.header_crc_ok = (self.header_crc == nvs_crc32(buf[4:28]))

    @property
    def is_active(self):
        return self.state == STATE_ACTIVE

    @property
    def is_empty(self):
        return self.state == STATE_EMPTY

    @property
    def is_full(self):
        return self.state == STATE_FULL

    def written_slots(self):
        return sum(1 for i in range(MAX_ENTRIES)
                   if slot_state(self.buf, i) == ENTRY_WRITTEN)

    def entries(self):
        """Yield every written entry of this page (follows the spans)."""
        i = 0
        while i < MAX_ENTRIES:
            if slot_state(self.buf, i) != ENTRY_WRITTEN:
                i += 1
                continue
            entry = parse_entry(self.buf, self.number, i)
            yield entry
            i += max(1, entry.span)

    def free_run(self, span):
        """Index of the first run of `span` free slots, or None."""
        run = 0
        for i in range(MAX_ENTRIES):
            if slot_state(self.buf, i) == ENTRY_WRITTEN:
                run = 0
                continue
            run += 1
            if run == span:
                return i - span + 1
        return None


class NvsImage(object):
    def __init__(self, data):
        if len(data) % PAGE_SIZE:
            raise PageError(f"image size {len(data)} is not a multiple of "
                            f"{PAGE_SIZE}")
        self.data = bytearray(data)
        self.page_count = len(data) // PAGE_SIZE
        # memoryview slices are views: writes through page.buf reach self.data.
        self._mv = memoryview(self.data)
        self.pages = [Page(self._mv[p * PAGE_SIZE:(p + 1) * PAGE_SIZE], p)
                      for p in range(self.page_count)]

    @classmethod
    def from_file(cls, path):
        with open(path, "rb") as fh:
            return cls(fh.read())

    def to_file(self, path, in_place=False):
        if in_place:
            tmp = path + ".tmp"
            with open(tmp, "wb") as fh:
                fh.write(self.data)
            os.replace(tmp, path)
        else:
            with open(path, "wb") as fh:
                fh.write(self.data)

    def active_page(self):
        active = [p for p in self.pages if p.is_active]
        if not active:
            raise PageError("no ACTIVE page found")
        return max(active, key=lambda p: p.seq)

    def namespaces(self):
        """Return (index -> name, name -> index) from the namespace entries."""
        idx2name = {}
        for page in self.pages:
            for e in page.entries():
                if e.type == TYPE_U8 and e.namespace == 0 and e.span == 1:
                    idx2name[e.value] = e.key
        return idx2name, {v: k for k, v in idx2name.items()}


def build_string_entry(key, value, ns_index):
    """Return (header_entry, data_entries, span) for an NVS string."""
    data = value.encode("utf-8") + b"\x00"
    datalen = len(data)
    data_entries = (datalen + ENTRY_SIZE - 1) // ENTRY_SIZE
    span = 1 + data_entries
    entry = bytearray(b"\xff" * ENTRY_SIZE)
    entry[0] = ns_index
    entry[1] = TYPE_SZ
    entry[2] = span
    entry[3] = CHUNK_ANY
    key_bytes = key.encode("utf-8")
    entry[8:24] = b"\x00" * 16
    entry[8:8 + len(key_bytes)] = key_bytes
    struct.pack_into("<H", entry, 24, datalen)
    struct.pack_into("<I", entry, 28, nvs_crc32(data))
    struct.pack_into("<I", entry, 4, entry_header_crc(entry))
    pad = data_entries * ENTRY_SIZE - datalen
    return bytes(entry), data + b"\xff" * pad, span


def build_namespace_entry(name, ns_index):
    entry = bytearray(b"\xff" * ENTRY_SIZE)
    entry[0] = 0
    entry[1] = TYPE_U8
    entry[2] = 1
    entry[3] = CHUNK_ANY
    key_bytes = name.encode("utf-8")
    entry[8:24] = b"\x00" * 16
    entry[8:8 + len(key_bytes)] = key_bytes
    struct.pack_into("<B", entry, 24, ns_index)
    struct.pack_into("<I", entry, 4, entry_header_crc(entry))
    return bytes(entry)


def refresh_header_crc(page_buf):
    struct.pack_into("<I", page_buf, 28, nvs_crc32(page_buf[4:28]))


def write_entry(page_buf, start, header, data, span):
    off = ENTRY_OFFSET + start * ENTRY_SIZE
    page_buf[off:off + ENTRY_SIZE] = header
    if data:
        page_buf[off + ENTRY_SIZE:off + ENTRY_SIZE + len(data)] = data
    for k in range(span):
        set_slot_state(page_buf, start + k, ENTRY_WRITTEN)


def allocate_page(image):
    """Mark the active page FULL and return a fresh ACTIVE page buffer."""
    old = image.active_page()
    new_no = None
    for page in image.pages:
        if page.is_empty:
            new_no = page.number
            break
    if new_no is None:
        raise PageError("no empty page left to rotate into")
    old.buf[0:4] = struct.pack("<I", STATE_FULL)
    refresh_header_crc(old.buf)
    new_buf = image._mv[new_no * PAGE_SIZE:(new_no + 1) * PAGE_SIZE]
    new_buf[:] = b"\xff" * PAGE_SIZE
    struct.pack_into("<I", new_buf, 0, STATE_ACTIVE)
    struct.pack_into("<I", new_buf, 4, old.seq + 1)
    new_buf[8] = VERSION2
    refresh_header_crc(new_buf)
    new_page = Page(new_buf, new_no)
    image.pages[new_no] = new_page
    return new_page


def next_namespace_index(image):
    used = set(image.namespaces()[0])
    index = 1
    while index in used:
        index += 1
    return index


def set_string(image, namespace, key, value):
    """Write one string key, preserving everything else. Returns a message."""
    if len(key) > MAX_KEY_LEN:
        raise PageError(f"key {key!r} longer than {MAX_KEY_LEN} bytes")
    if len(value.encode("utf-8")) > MAX_STRING_LEN:
        raise PageError(f"value for {key!r} longer than {MAX_STRING_LEN} bytes")

    _, name2idx = image.namespaces()
    ns_index = name2idx.get(namespace)
    ns_header = None
    if ns_index is None:
        if len(namespace) > MAX_NS_LEN:
            raise PageError(f"namespace {namespace!r} longer than {MAX_NS_LEN}")
        ns_index = next_namespace_index(image)
        ns_header = build_namespace_entry(namespace, ns_index)

    header, data, span = build_string_entry(key, value, ns_index)
    total = span + (1 if ns_header else 0)

    page = image.active_page()
    start = page.free_run(total)
    if start is None:
        page = allocate_page(image)
        start = page.free_run(total)
        if start is None:
            raise PageError("a fresh page cannot hold the entry")

    cursor = start
    if ns_header is not None:
        write_entry(page.buf, cursor, ns_header, b"", 1)
        cursor += 1
    write_entry(page.buf, cursor, header, data, span)

    where = f"page {page.number} @slot {cursor}"
    if ns_header is not None:
        return (f"created namespace {namespace!r} (index {ns_index}) and set "
                f"{key} on {where}")
    return f"set {namespace}/{key} on {where}"


def cmd_dump(args):
    image = NvsImage.from_file(args.image)
    idx2name, _ = image.namespaces()
    print(f"Image : {args.image}  ({len(image.data)} bytes, "
          f"{image.page_count} pages)")
    print("Namespaces:")
    for idx in sorted(idx2name):
        print(f"  [{idx}] {idx2name[idx]}")
    print()
    for page in image.pages:
        state = {STATE_ACTIVE: "ACTIVE", STATE_FULL: "FULL",
                 STATE_EMPTY: "EMPTY"}.get(page.state, f"0x{page.state:08x}")
        crc = "ok" if page.header_crc_ok else "BAD"
        print(f"Page {page.number}: {state} seq={page.seq} "
              f"ver=0x{page.version:02x} hdr_crc={crc} "
              f"written={page.written_slots()}/{MAX_ENTRIES}")
        for e in page.entries():
            ecrc = "ok" if e.crc_ok else "BAD"
            ns = idx2name.get(e.namespace, f"<{e.namespace}>")
            if isinstance(e.value, _StrValue):
                dcrc = "ok" if e.value.crc_ok else "BAD"
                desc = f"str len={e.value.datalen} crc={dcrc} {e.value.text!r}"
            elif isinstance(e.value, _BlobValue):
                desc = (f"blob len={e.value.datalen} "
                        f"chunks={e.value.chunk_count}")
            else:
                desc = f"value={e.value}"
            print(f"  slot {e.index:3d}: {ns}/{e.key} type=0x{e.type:02x} "
                  f"span={e.span} crc={ecrc} {desc}")
    return 0


def cmd_set(args):
    image = NvsImage.from_file(args.image)
    _, name2idx = image.namespaces()
    messages = []
    for assignment in args.assignments:
        if "=" not in assignment:
            print(f"ERROR: expected key=value, got {assignment!r}",
                  file=sys.stderr)
            return 2
        key, value = assignment.split("=", 1)
        if name2idx.get(args.namespace) is not None:
            existing = [e for p in image.pages for e in p.entries()
                        if e.namespace == name2idx[args.namespace]
                        and e.key == key and isinstance(e.value, _StrValue)]
            if existing and existing[-1].value.text == value:
                messages.append(f"{args.namespace}/{key} already set "
                                f"(value unchanged, skipped)")
                continue
        messages.append(set_string(image, args.namespace, key, value))
    for msg in messages:
        print(msg)
    target = args.out or args.image
    image.to_file(target, in_place=not args.out)
    print(f"wrote {target}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Read/modify an ESP32 NVS partition image (stdlib only).")
    sub = ap.add_subparsers(dest="command", required=True)

    p_dump = sub.add_parser("dump", help="print namespaces and keys")
    p_dump.add_argument("image")
    p_dump.set_defaults(func=cmd_dump)

    p_set = sub.add_parser("set", help="set string keys (preserving the rest)")
    p_set.add_argument("image")
    p_set.add_argument("namespace")
    p_set.add_argument("assignments", nargs="+", metavar="key=value")
    p_set.add_argument("-o", "--out", help="write to this image (default: "
                                           "modify in place)")
    p_set.set_defaults(func=cmd_set)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except PageError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
