# SML Parser component

This component vendors `olliiiver/sml_parser` version 0.29 from commit
`b68d0e0ff2b61f0fb225d16a5d6a00c0db1703d2`. The upstream source and LGPL-2.1
license are retained in this directory.

The only upstream parser change is `smlReset()`, used to discard partial parser
state after a detected ESP-NOW transport sequence gap. `sml_adapter.cpp` feeds
the receiver's reassembled raw payload one byte at a time and publishes the
three configured meter OBIS values after the library recognizes their lists.
