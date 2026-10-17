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
