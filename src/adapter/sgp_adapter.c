/* sgp_adapter.c - SGP: Smoke/Gas Protocol
 *
 *   offset 0 : magic 0x7E
 *   offset 1 : id    uint16, big-endian
 *   offset 3 : ppm   uint16, big-endian
 *   offset 5 : ts    uint32, big-endian
 *   offset 9 : crc16 uint16, big-endian, CRC-16/CCITT-FALSE of bytes 1..8
 *
 * The gateway answers every frame with ACK (0x06) or NAK (0x15).
 */
#include "adapters.h"
#include "byteorder.h"
#include "crc16.h"

#include <string.h>

static parse_result_t sgp_parse(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    if (len == 0)
        return PARSE_NEED_MORE;

    if (buf[0] != SGP_MAGIC) {
        /* Resync on the next magic byte; drop everything if there is none. */
        const uint8_t *m = memchr(buf + 1, SGP_MAGIC, len - 1);
        *consumed = m ? (size_t)(m - buf) : len;
        adapter_set_error("bad magic");
        return PARSE_ERROR;
    }

    if (len < SGP_FRAME_LEN)
        return PARSE_NEED_MORE;

    /* A bad CRC drops the whole frame: TCP keeps the stream aligned, and
     * the sensor resends the frame after the NAK. */
    *consumed = SGP_FRAME_LEN;
    if (crc16_ccitt_false(buf + 1, 8) != get_be16(buf + 9)) {
        adapter_set_error("bad CRC");
        return PARSE_ERROR;
    }

    out->id    = get_be16(buf + 1);
    out->type  = SENSOR_SMOKE_GAS;
    out->value = get_be16(buf + 3);
    out->ts    = (time_t)get_be32(buf + 5);
    return PARSE_OK;
}

static size_t sgp_build_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    if (result == PARSE_NEED_MORE || cap < 1)
        return 0;
    reply[0] = (result == PARSE_OK) ? SGP_ACK : SGP_NAK;
    return 1;
}

const protocol_adapter_t SGP_ADAPTER = {
    .name        = "SGP",
    .port_offset = 3,
    .parse       = sgp_parse,
    .build_reply = sgp_build_reply,
};
