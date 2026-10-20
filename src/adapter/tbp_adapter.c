/* tbp_adapter.c - TBP: Temperature Binary Protocol
 *
 *   offset 0  : id    uint16, little-endian
 *   offset 2  : value IEEE 754 double, little-endian
 *   offset 10 : ts    int64, little-endian
 */
#include "adapters.h"

#include <math.h>
#include <string.h>

#define TBP_TEMP_MIN  -40.0
#define TBP_TEMP_MAX   85.0

/* Decode byte by byte instead of casting buf to a struct: independent of
 * padding, alignment and the host byte order. */
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

static parse_result_t tbp_parse(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    if (len < TBP_FRAME_LEN)
        return PARSE_NEED_MORE;

    /* Fixed-length frame: a bad one is dropped as a whole. */
    *consumed = TBP_FRAME_LEN;

    uint64_t bits = get_le64(buf + 2);
    double value;
    memcpy(&value, &bits, sizeof value);
    if (!isfinite(value) || value < TBP_TEMP_MIN || value > TBP_TEMP_MAX)
        return PARSE_ERROR;

    out->id    = get_le16(buf);
    out->type  = SENSOR_TEMPERATURE;
    out->value = value;
    out->ts    = (time_t)(int64_t)get_le64(buf + 10);
    return PARSE_OK;
}

static size_t tbp_build_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    (void)result;
    (void)reply;
    (void)cap;
    return 0;                       /* TBP has no reply */
}

const protocol_adapter_t TBP_ADAPTER = {
    .name        = "TBP",
    .port_offset = 0,
    .parse       = tbp_parse,
    .build_reply = tbp_build_reply,
};
