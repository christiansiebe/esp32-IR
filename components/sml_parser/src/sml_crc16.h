#ifndef SML_CRC16_H
#define SML_CRC16_H

/* Reflected CRC-16 for SML (poly 0x8408, i.e. x^16 + x^12 + x^5 + 1).
 *
 * Every SML meter in the public devZer0/libsml-testing corpus (232 telegrams
 * across EMH, EasyMeter, Holley DTZ541, ISKRA, DZG, ITRON, eBZ, DrNeuhaus)
 * validates with the SML standard parametrisation, which is CRC-16/X-25:
 *
 *   init 0xFFFF, final XOR 0xFFFF, reflected in/out   -> check("123456789") 0x906E
 *
 * A few meters are reported to use the "Kermit" parametrisation over the same
 * reflected table instead (sml2mqtt defaults to trying x25 *and* kermit):
 *
 *   init 0x0000, no final XOR, reflected in/out       -> check("123456789") 0x2189
 *
 * The two differ only in init/final-XOR, never in the polynomial, so they share
 * one 256-entry table (smlCrcTable.h). sml_crc16_x25() is what the vendored
 * parser uses; sml_crc16_kermit() is provided (and host-tested) so a future
 * meter can be supported with a verified building block instead of a blind fix.
 *
 * Byte order: sml_crc16_x25() returns the value exactly as the parser compares
 * it, i.e. the little-endian reading of the two on-wire checksum bytes
 * (`low | (high << 8)`). That is *not* the byte-swapped value smllib returns.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SML_CRC16_X25 = 0, /* init 0xFFFF, final XOR 0xFFFF (SML standard) */
    SML_CRC16_KERMIT = 1, /* init 0x0000, no final XOR */
} sml_crc16_variant_t;

/* Running update over a single byte; the caller owns the state. Feed bytes in
 * stream order, seeding the state with the variant's init value. */
uint16_t sml_crc16_update(uint8_t byte, uint16_t crc);

/* One-shot over a whole buffer. */
uint16_t sml_crc16(const uint8_t *data, size_t len, sml_crc16_variant_t variant);
uint16_t sml_crc16_x25(const uint8_t *data, size_t len);
uint16_t sml_crc16_kermit(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
