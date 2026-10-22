/* byteorder.h - read/write multi-byte fields of wire frames byte by byte.
 *
 * Decoding byte by byte instead of casting a buffer to a struct is
 * independent of padding, alignment and the host byte order.
 */
#ifndef BYTEORDER_H
#define BYTEORDER_H

#include <stdint.h>

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif /* BYTEORDER_H */
