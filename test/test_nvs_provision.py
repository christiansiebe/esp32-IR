"""Host tests for tools/nvs_provision.py (pure logic, no hardware).

The synthetic image is built with an INDEPENDENT encoder written here (raw
struct/zlib), so the tool's parser is cross-checked against a second
implementation of the documented layout. The real device backup is parsed
read-only when present.
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import nvs_provision as nv  # noqa: E402

BACKUP = "/mnt/ssd/c6-nvs/nvs_backup.bin"

PAGE = 4096
CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


# --- independent encoder (does NOT use the tool) ---------------------------

def _crc(data):
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def _hdr_crc(entry):
    return _crc(bytes(entry[0:4]) + bytes(entry[8:32]))


def _set_bitmap(buf, idx, state):
    byte = 32 + idx // 4
    shift = (idx % 4) * 2
    buf[byte] = (buf[byte] & ~(0b11 << shift)) | (state << shift)


def _place(buf, idx, raw, span, data=b""):
    off = 64 + idx * 32
    buf[off:off + 32] = raw
    if data:
        buf[off + 32:off + 32 + len(data)] = data
    for k in range(span):
        _set_bitmap(buf, idx + k, 0b10)  # written


def enc_nsdef(name, index):
    e = bytearray(b"\xff" * 32)
    e[0], e[1], e[2], e[3] = 0, 0x01, 1, 0xFF
    e[8:24] = b"\x00" * 16
    e[8:8 + len(name)] = name.encode()
    e[24] = index
    e[4:8] = struct.pack("<I", _hdr_crc(e))
    return bytes(e)


def enc_str(ns, key, value):
    data = value.encode() + b"\x00"
    data_entries = (len(data) + 31) // 32
    span = data_entries + 1
    e = bytearray(b"\xff" * 32)
    e[0], e[1], e[2], e[3] = ns, 0x21, span, 0xFF
    e[8:24] = b"\x00" * 16
    e[8:8 + len(key)] = key.encode()
    struct.pack_into("<H", e, 24, len(data))
    struct.pack_into("<I", e, 28, _crc(data))
    e[4:8] = struct.pack("<I", _hdr_crc(e))
    return bytes(e), data + b"\xff" * (data_entries * 32 - len(data)), span


def make_page(number, seq, entries, state=0xFFFFFFFE, version=0xFE):
    buf = bytearray(b"\xff" * PAGE)
    struct.pack_into("<I", buf, 0, state)
    struct.pack_into("<I", buf, 4, seq)
    buf[8] = version
    struct.pack_into("<I", buf, 28, _crc(bytes(buf[4:28])))
    slot = 0
    for raw, data, span in entries:
        _place(buf, slot, raw, span, data)
        slot += span
    return buf


def make_image(pages):
    return bytes(b"".join(pages))


def sample_image():
    """2 pages: ACTIVE page 0 with one ns + two strings, page 1 EMPTY."""
    e0 = enc_nsdef("testns", 1)
    s1 = enc_str(1, "greeting", "hello")
    s2 = enc_str(1, "long", "x" * 70)  # span 4 -> exercises multi-entry data
    p0 = make_page(0, 0, [(e0, b"", 1), (s1[0], s1[1], s1[2]),
                          (s2[0], s2[1], s2[2])])
    p1 = make_page(1, 0xFFFFFFFF, [], state=0xFFFFFFFF, version=0xFF)
    return make_image([p0, p1])


# --- tests -----------------------------------------------------------------

def test_crc_helpers():
    print("test_crc_helpers")
    # NVS CRC is zlib.crc32(data, 0xFFFFFFFF); pin the value as a guard.
    check(nv.nvs_crc32(b"123456789") == 0xD202D277,
          f"crc32 init 0xFFFFFFFF of '123456789' = "
          f"0x{nv.nvs_crc32(b'123456789'):08x}")
    e = bytearray(b"\x01\x21\x02\xff") + b"\x11" * 4 + b"\x00" * 4 + b"k" * 8
    e[4:8] = struct.pack("<I", nv.entry_header_crc(e))
    check(nv.entry_header_crc(e) == struct.unpack_from("<I", e, 4)[0],
          "entry header crc roundtrip")


def test_parse_synthetic():
    print("test_parse_synthetic")
    img = nv.NvsImage(sample_image())
    idx2name, name2idx = img.namespaces()
    check(idx2name == {1: "testns"}, f"namespaces {idx2name}")
    check(name2idx.get("testns") == 1, "name->idx")
    active = img.active_page()
    check(active.number == 0 and active.seq == 0, "active page")
    check(active.header_crc_ok, "active header crc")
    entries = {e.key: e for e in active.entries()}
    check(set(entries) == {"testns", "greeting", "long"}, f"keys {set(entries)}")
    check(entries["greeting"].value.text == "hello", "string value")
    check(entries["greeting"].value.crc_ok, "string data crc")
    check(entries["long"].value.text == "x" * 70, "long string value")
    check(entries["long"].span == 4, f"long span {entries['long'].span}")
    check(entries["testns"].value == 1, "ns index value")
    check(all(e.crc_ok for e in active.entries()), "all entry crcs ok")
    check(img.pages[1].is_empty, "page 1 empty")


def test_set_preserves_and_roundtrips():
    print("test_set_preserves_and_roundtrips")
    data = sample_image()
    img = nv.NvsImage(data)
    before_p0 = bytes(img.pages[0].buf)
    before_p1 = bytes(img.pages[1].buf)
    nv.set_string(img, "wifi", "ssid", "Wohnzimmer")
    nv.set_string(img, "wifi", "pass", "demo-pass-42")
    out = nv.NvsImage(bytes(img.data))

    idx2name, name2idx = out.namespaces()
    check(name2idx.get("wifi") == 2, f"new namespace index {idx2name}")
    check(name2idx.get("testns") == 1, "old namespace preserved")
    active = out.active_page()
    check(active.header_crc_ok, "active header crc after set")
    entries = {(e.namespace, e.key): e for e in active.entries()}
    check((2, "ssid") in entries and (2, "pass") in entries, "new keys present")
    check(entries[(2, "ssid")].value.text == "Wohnzimmer", "ssid value")
    check(entries[(2, "pass")].value.text == "demo-pass-42", "pass value")
    check(entries[(2, "ssid")].value.crc_ok, "ssid data crc")
    check(entries[(1, "greeting")].value.text == "hello", "old string intact")
    check(all(e.crc_ok for e in active.entries()), "all crcs ok after set")
    # nothing was rotated into page 1, and the written page 0 only grew.
    check(bytes(out.pages[1].buf) == before_p1, "empty page untouched")
    check(len(out.data) == len(data), "image size unchanged")
    check(before_p0 != bytes(out.pages[0].buf), "active page changed")


def test_reuse_existing_namespace():
    print("test_reuse_existing_namespace")
    img = nv.NvsImage(sample_image())
    nv.set_string(img, "testns", "greeting", "world")
    out = nv.NvsImage(bytes(img.data))
    idx2name, _ = out.namespaces()
    check(list(idx2name) == [1], f"no new namespace {idx2name}")
    active = out.active_page()
    greetings = [e for e in active.entries()
                 if e.namespace == 1 and e.key == "greeting"]
    check(len(greetings) == 2, f"new + old greeting {len(greetings)}")
    check(greetings[-1].value.text == "world", "new value wins (later slot)")


def test_page_rotation():
    print("test_page_rotation")
    # Fill the active page almost completely, then add an entry that does not
    # fit -> a new page must be allocated and the old one marked FULL.
    entries = []
    slot = 0
    while slot < 125:  # leave one slot free so the new entry cannot fit
        header, data, span = enc_str(1, "k%03d" % slot, "v")
        entries.append((header, data, span))
        slot += span
    p0 = make_page(0, 5, entries)
    p1 = make_page(1, 0xFFFFFFFF, [], state=0xFFFFFFFF, version=0xFF)
    img = nv.NvsImage(make_image([p0, p1]))
    nv.set_string(img, "late", "key", "value")
    out = nv.NvsImage(bytes(img.data))
    old, new = out.pages[0], out.pages[1]
    check(old.is_full, f"old page now FULL (0x{old.state:08x})")
    check(old.header_crc_ok, "old page header crc after FULL")
    check(new.is_active, "new page ACTIVE")
    check(new.seq == 6, f"new seq {new.seq}")
    check(new.header_crc_ok, "new page header crc")
    entries_new = {e.key: e for e in new.entries()}
    check("key" in entries_new, "new key on fresh page")
    check(entries_new["key"].value.text == "value", "new value on fresh page")


def test_dedup_in_cmd_set():
    print("test_dedup_in_cmd_set")
    import contextlib
    import io
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "nvs.bin")
        with open(path, "wb") as fh:
            fh.write(sample_image())
        args = type("A", (), {"image": path, "namespace": "testns",
                              "assignments": ["greeting=hello"], "out": None})()
        with contextlib.redirect_stdout(io.StringIO()):
            nv.cmd_set(args)
        img = nv.NvsImage.from_file(path)
        greetings = [e for e in img.active_page().entries()
                     if e.key == "greeting"]
        check(len(greetings) == 1,
              f"identical value not rewritten {len(greetings)}")


# Golden bytes copied from a real IDF-written NVS image (the Wi-Fi SSID entry,
# namespace 6 = metercfg, page 3 slot 0). The tool must reproduce them exactly;
# only the SSID is pinned here, never the password.
GOLDEN_STR_HEADER = bytes.fromhex(
    "062102ffc7aaaad3776966695f73736964000000000000000b00fffffaa65430")
GOLDEN_STR_DATA = bytes.fromhex(
    "576f686e7a696d6d657200" + "ff" * 21)


def test_golden_entry_bytes():
    print("test_golden_entry_bytes")
    header, data, span = nv.build_string_entry("wifi_ssid", "Wohnzimmer", 6)
    check(header == GOLDEN_STR_HEADER,
          f"header mismatch\n    got {header.hex()}\n    exp "
          f"{GOLDEN_STR_HEADER.hex()}")
    check(data == GOLDEN_STR_DATA, f"data mismatch: {data.hex()}")
    check(span == 2, f"span {span}")
    # And the tool reads its own golden bytes back.
    page = make_page(3, 4, [(GOLDEN_STR_HEADER, GOLDEN_STR_DATA, 2)],
                     state=0xFFFFFFFE)
    e = next(iter(nv.NvsImage(make_image([page])).pages[0].entries()))
    check(e.crc_ok and e.value.crc_ok, "golden entry crcs")
    check(e.value.text == "Wohnzimmer", f"golden value {e.value.text}")


def test_backup_readonly():
    print("test_backup_readonly")
    if not os.path.exists(BACKUP):
        print("  (skipped: %s not present)" % BACKUP)
        return
    img = nv.NvsImage.from_file(BACKUP)
    idx2name, _ = img.namespaces()
    check(idx2name == {1: "misc", 2: "nvs.net80211", 3: "phy", 4: "energy",
                       5: "rtcfg"}, f"backup namespaces {idx2name}")
    check("metercfg" not in idx2name.values(), "metercfg not yet present")
    check(all(p.header_crc_ok for p in img.pages if not p.is_empty),
          "all used backup headers crc ok")
    active = img.active_page()
    check(active.seq == 4, f"active seq {active.seq}")
    check(active.number == 4, f"active page {active.number}")
    # The next free namespace index must be 6.
    check(nv.next_namespace_index(img) == 6,
          f"next ns index {nv.next_namespace_index(img)}")
    # Read-only: the file must not have been touched.
    check(os.path.getsize(BACKUP) == 24576, "backup size unchanged")


def main():
    for fn in (test_crc_helpers, test_parse_synthetic,
               test_set_preserves_and_roundtrips, test_reuse_existing_namespace,
               test_page_rotation, test_dedup_in_cmd_set,
               test_golden_entry_bytes, test_backup_readonly):
        fn()
    print(f"{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
