/* evp_adapter.c - EVP: Event TLV Protocol
 *
 *   type(1) length(1) payload(length), multi-byte fields big-endian
 *   0x01 Motion  length 6: id uint16, ts uint32
 *   0x02 Door    length 7: id uint16, state uint8 (0 closed, 1 open), ts uint32
 */
#include "adapters.h"
#include "byteorder.h"

static int valid_header(uint8_t type, uint8_t length)
{
    return (type == EVP_TYPE_MOTION && length == EVP_LEN_MOTION) ||
           (type == EVP_TYPE_DOOR   && length == EVP_LEN_DOOR);
}

/* There is no start marker: after a bad header, resync on the next offset
 * that holds a valid (type, length) pair. If none is found, keep the last
 * byte, it may be the type of a header whose length is still in flight. */
static size_t resync(const uint8_t *buf, size_t len)
{
    for (size_t i = 1; i + 1 < len; ++i)
        if (valid_header(buf[i], buf[i + 1]))
            return i;
    return len - 1;
}

static parse_result_t evp_parse(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    if (len < EVP_HDR_LEN)
        return PARSE_NEED_MORE;

    uint8_t type = buf[0], length = buf[1];
    if (!valid_header(type, length)) {
        *consumed = resync(buf, len);
        adapter_set_error(type == EVP_TYPE_MOTION || type == EVP_TYPE_DOOR
                          ? "bad length" : "bad type");
        return PARSE_ERROR;
    }

    size_t frame_len = EVP_HDR_LEN + (size_t)length;
    if (len < frame_len)
        return PARSE_NEED_MORE;
    *consumed = frame_len;

    const uint8_t *p = buf + EVP_HDR_LEN;
    out->id = get_be16(p);
    if (type == EVP_TYPE_MOTION) {
        out->type  = SENSOR_MOTION;
        out->value = 1.0;                     /* motion detected */
        out->ts    = (time_t)get_be32(p + 2);
    } else {
        uint8_t state = p[2];
        if (state > 1) {
            adapter_set_error("bad value");
            return PARSE_ERROR;
        }
        out->type  = SENSOR_DOOR;
        out->value = state;
        out->ts    = (time_t)get_be32(p + 3);
    }
    return PARSE_OK;
}

static size_t evp_build_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    (void)result;
    (void)reply;
    (void)cap;
    return 0;                       /* EVP has no reply */
}

const protocol_adapter_t EVP_ADAPTER = {
    .name        = "EVP",
    .port_offset = 2,
    .parse       = evp_parse,
    .build_reply = evp_build_reply,
};
