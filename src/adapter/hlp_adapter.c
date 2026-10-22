/* hlp_adapter.c - HLP: Humidity Line Protocol
 *
 *   "HUM <id> <value> <ts>\n"   ASCII, fields separated by exactly one space,
 *                               at most 64 bytes including the LF
 */
#include "adapters.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HLP_HUM_MIN    0.0
#define HLP_HUM_MAX  100.0

/* A field starts right after one separator: no sign, no leading space
 * (strto* would silently skip whitespace). */
static int field_start_ok(const char *p)
{
    return (*p >= '0' && *p <= '9');
}

static parse_result_t hlp_parse(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    size_t scan = len < HLP_MAX_LINE ? len : HLP_MAX_LINE;
    const uint8_t *lf = memchr(buf, '\n', scan);
    if (lf == NULL) {
        if (len < HLP_MAX_LINE)
            return PARSE_NEED_MORE;
        /* 64 bytes without LF: drop them; the rest of the over-long line
         * fails again as a bad line, after which framing is back on LF. */
        *consumed = len;
        adapter_set_error("bad length");
        return PARSE_ERROR;
    }

    size_t line_len = (size_t)(lf - buf);
    *consumed = line_len + 1;                 /* line and its LF */

    char line[HLP_MAX_LINE];
    memcpy(line, buf, line_len);
    line[line_len] = '\0';
    if (strlen(line) != line_len) {           /* embedded NUL byte */
        adapter_set_error("bad format");
        return PARSE_ERROR;
    }

    if (strncmp(line, "HUM ", 4) != 0 || !field_start_ok(line + 4)) {
        adapter_set_error("bad format");
        return PARSE_ERROR;
    }

    char *p = line + 4, *end;
    errno = 0;
    unsigned long id = strtoul(p, &end, 10);
    if (errno != 0 || id > UINT16_MAX || *end != ' ' || !field_start_ok(end + 1)) {
        adapter_set_error("bad format");
        return PARSE_ERROR;
    }

    p = end + 1;
    errno = 0;
    double value = strtod(p, &end);
    if (errno != 0 || *end != ' ' || !field_start_ok(end + 1)) {
        adapter_set_error("bad format");
        return PARSE_ERROR;
    }

    p = end + 1;
    errno = 0;
    long long ts = strtoll(p, &end, 10);
    if (errno != 0 || *end != '\0') {
        adapter_set_error("bad format");
        return PARSE_ERROR;
    }

    if (!isfinite(value) || value < HLP_HUM_MIN || value > HLP_HUM_MAX) {
        adapter_set_error("bad value");
        return PARSE_ERROR;
    }

    out->id    = (sensor_id_t)id;
    out->type  = SENSOR_HUMIDITY;
    out->value = value;
    out->ts    = (time_t)ts;
    return PARSE_OK;
}

static size_t hlp_build_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    (void)result;
    (void)reply;
    (void)cap;
    return 0;                       /* HLP has no reply */
}

const protocol_adapter_t HLP_ADAPTER = {
    .name        = "HLP",
    .port_offset = 1,
    .parse       = hlp_parse,
    .build_reply = hlp_build_reply,
};
