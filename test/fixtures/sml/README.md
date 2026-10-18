# SML test fixtures

Real smart-meter SML telegrams used as golden decode fixtures.

## `ehz_bin.upstream.h`

- Meter: EMH eHZ (SML/EDL-21 optical interface).
- Source: the public `devZer0/libsml-testing` corpus, carried verbatim in
  `olliiiver/sml_parser`'s example directory (`examples/arduino/src/ehz_bin.h`).
  That parser is the same one vendored under `components/sml_parser`.
- License note: used here only as test data. Keep attribution if the file is
  ever reformatted or moved. See the project LICENSE decision in `TODO.md`.
- Contents: a 319-byte stream (preamble `1b 1b 1b 1b 01 01 01 01`, two SML
  messages) carrying `1-0:1.8.0`, `1-0:1.8.1`, `1-0:1.8.2` (Wh) and
  `1-0:15.7.0` (W). No `2.8.0`/`16.7.0` in this telegram.
- Golden values asserted by `test/test_sml_parser_golden.cpp`:
  `1-0:1.8.0 = 0x74ea`, `1-0:1.8.1 = 0x12d687`, `1-0:15.7.0 = 0x2f65` W, and a
  flipped payload byte must end in `SML_CHECKSUM_ERROR`.

## `*.bin` — real single-telegram fixtures

One complete SML telegram per file (preamble `1b1b1b1b01010101` through the
`1b1b1b1b1a01` end marker and the two checksum bytes), extracted verbatim from
the public `devZer0/libsml-testing` corpus. They drive
`test/test_sml_golden_bin.cpp` (golden OBIS decode + `crc_valid`) and
`test/test_sml_crc16.c` (CRC parametrisation). The `.bin` files are committed on
purpose — `.gitignore` carries a `!test/fixtures/sml/*.bin` exception.

| File | Meter | Notes |
|------|-------|-------|
| `emh_ehz_hw8e2a5l0ek2p.bin` | EMH eHZ HW8E2A5L0EK2P | `1-0:1.8.0` only |
| `emh_ehz_hw8e2a5l0ek2p_crc_error.bin` | (same, corrupted) | one payload byte flipped, checksum untouched → must be `crc_valid=false` |
| `easymeter_q3a.bin` | EasyMeter Q3A | import + export energy, active power |
| `holley_dtz541.bin` | Holley DTZ541-ZDBA | the meter the TODO suspected needs a Kermit CRC; it is a plain X-25 telegram |
| `iskra_mt691.bin` | ISKRA MT691 eHZ | import energy + active power |

Golden CRC finding: across all 232 telegrams of the corpus (EMH, EasyMeter,
Holley DTZ541, ISKRA, DZG, ITRON, eBZ, DrNeuhaus) **every** telegram validates
with CRC-16/X-25 (`init 0xFFFF`, final XOR `0xFFFF`), none with Kermit. The
parser's single X-25 check is therefore correct; see
`components/sml_parser/src/sml_crc16.h`.
