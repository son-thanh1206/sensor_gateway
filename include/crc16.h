/* crc16.h - CRC-16/CCITT-FALSE, shared by the SGP adapter and sensor_node.
 *
 *   polynomial 0x1021, init 0xFFFF, no bit reflection, no final XOR
 *   check value: crc16_ccitt_false("123456789", 9) == 0x29B1
 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

static inline uint16_t crc16_ccitt_false(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

#endif /* CRC16_H */
