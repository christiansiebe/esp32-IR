#include "sml_crc16.h"

#include "smlCrcTable.h"

uint16_t sml_crc16_update(uint8_t byte, uint16_t crc)
{
#ifdef ARDUINO
    return pgm_read_word_near(&smlCrcTable[(byte ^ crc) & 0xff]) ^
           ((crc >> 8) & 0xff);
#else
    return smlCrcTable[(byte ^ crc) & 0xff] ^ ((crc >> 8) & 0xff);
#endif
}


uint16_t sml_crc16(const uint8_t *data, size_t len, sml_crc16_variant_t variant)
{
    uint16_t crc = (variant == SML_CRC16_KERMIT) ? 0x0000 : 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = sml_crc16_update(data[i], crc);
    }
    if (variant == SML_CRC16_KERMIT) {
        return crc;
    }
    return crc ^ 0xFFFF;
}

uint16_t sml_crc16_x25(const uint8_t *data, size_t len)
{
    return sml_crc16(data, len, SML_CRC16_X25);
}

uint16_t sml_crc16_kermit(const uint8_t *data, size_t len)
{
    return sml_crc16(data, len, SML_CRC16_KERMIT);
}
