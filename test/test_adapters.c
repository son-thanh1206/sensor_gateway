/*
 * test_adapters.c - unit tests of every protocol adapter's parse() and
 * build_reply(), using the example frames of assignment section 3.4.
 *
 *   make test-adapters
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adapters.h"
#include "crc16.h"

static int failures;

#define CHECK(cond) do {                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

#define TS_EXAMPLE 1767225600

/* Section 3.4 examples */
static const uint8_t TBP_EX[] = {
    0x0A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x35, 0x40,
    0x00, 0xB9, 0x55, 0x69, 0x00, 0x00, 0x00, 0x00,
};
static const char    HLP_EX[]    = "HUM 21 54.3 1767225600\n";
static const uint8_t MOTION_EX[] = { 0x01, 0x06, 0x00, 0x1E, 0x69, 0x55, 0xB9, 0x00 };
static const uint8_t DOOR_EX[]   = { 0x02, 0x07, 0x00, 0x28, 0x01, 0x69, 0x55, 0xB9, 0x00 };
static const uint8_t SGP_EX[]    = { 0x7E, 0x00, 0x32, 0x00, 0x78,
                                     0x69, 0x55, 0xB9, 0x00, 0xAC, 0xF5 };

static parse_result_t parse(const protocol_adapter_t *a, const void *buf, size_t len,
                            sensor_data_t *out, size_t *consumed)
{
    *consumed = 0;
    return a->parse(buf, len, out, consumed);
}

/* Every strict prefix of a valid frame must ask for more bytes. */
static void check_prefixes(const protocol_adapter_t *a, const uint8_t *frame, size_t len)
{
    sensor_data_t d;
    size_t c;
    for (size_t n = 0; n < len; ++n)
        CHECK(parse(a, frame, n, &d, &c) == PARSE_NEED_MORE);
}

/* Two frames glued together: the first parse takes exactly one frame. */
static void check_glued(const protocol_adapter_t *a, const uint8_t *frame, size_t len)
{
    uint8_t two[128];
    memcpy(two, frame, len);
    memcpy(two + len, frame, len);
    sensor_data_t d;
    size_t c;
    CHECK(parse(a, two, 2 * len, &d, &c) == PARSE_OK && c == len);
    CHECK(parse(a, two + c, 2 * len - c, &d, &c) == PARSE_OK && c == len);
}

static void test_crc(void)
{
    CHECK(crc16_ccitt_false((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(crc16_ccitt_false(SGP_EX + 1, 8) == 0xACF5);
}

static void test_tbp(void)
{
    sensor_data_t d;
    size_t c;
    CHECK(parse(&TBP_ADAPTER, TBP_EX, sizeof TBP_EX, &d, &c) == PARSE_OK);
    CHECK(c == TBP_FRAME_LEN);
    CHECK(d.id == 10 && d.type == SENSOR_TEMPERATURE && d.value == 21.5 && d.ts == TS_EXAMPLE);
    check_prefixes(&TBP_ADAPTER, TBP_EX, sizeof TBP_EX);
    check_glued(&TBP_ADAPTER, TBP_EX, sizeof TBP_EX);

    uint8_t bad[TBP_FRAME_LEN];
    memcpy(bad, TBP_EX, sizeof bad);
    bad[9] = 0x7F;                              /* huge double */
    CHECK(parse(&TBP_ADAPTER, bad, sizeof bad, &d, &c) == PARSE_ERROR);
    CHECK(c == TBP_FRAME_LEN && strcmp(adapter_last_error(), "bad value") == 0);
    CHECK(TBP_ADAPTER.port_offset == 0);
    CHECK(TBP_ADAPTER.build_reply(PARSE_OK, bad, sizeof bad) == 0);
}

static void expect_hlp_error(const char *line, const char *reason)
{
    sensor_data_t d;
    size_t c;
    CHECK(parse(&HLP_ADAPTER, line, strlen(line), &d, &c) == PARSE_ERROR);
    CHECK(c == strlen(line));
    if (strcmp(adapter_last_error(), reason) != 0) {
        fprintf(stderr, "FAIL HLP '%s': reason '%s', expected '%s'\n",
                line, adapter_last_error(), reason);
        ++failures;
    }
}

static void test_hlp(void)
{
    sensor_data_t d;
    size_t c;
    size_t len = strlen(HLP_EX);
    CHECK(parse(&HLP_ADAPTER, HLP_EX, len, &d, &c) == PARSE_OK);
    CHECK(c == len);
    CHECK(d.id == 21 && d.type == SENSOR_HUMIDITY && fabs(d.value - 54.3) < 1e-9 &&
          d.ts == TS_EXAMPLE);
    check_prefixes(&HLP_ADAPTER, (const uint8_t *)HLP_EX, len);
    check_glued(&HLP_ADAPTER, (const uint8_t *)HLP_EX, len);

    expect_hlp_error("TMP 21 54.3 1767225600\n", "bad format");
    expect_hlp_error("HUM 21  54.3 1767225600\n", "bad format");   /* two spaces */
    expect_hlp_error("HUM 21 54,3 1767225600\n", "bad format");    /* comma */
    expect_hlp_error("HUM 21 54.3 1767225600 \n", "bad format");   /* trailing */
    expect_hlp_error("HUM 21 54.3\n", "bad format");
    expect_hlp_error("HUM 70000 54.3 1767225600\n", "bad format"); /* id > uint16 */
    expect_hlp_error("HUM 21 120.0 1767225600\n", "bad value");

    /* 64 bytes without LF: an over-long line. */
    char longline[100];
    memset(longline, 'A', sizeof longline);
    CHECK(parse(&HLP_ADAPTER, longline, HLP_MAX_LINE - 1, &d, &c) == PARSE_NEED_MORE);
    CHECK(parse(&HLP_ADAPTER, longline, HLP_MAX_LINE, &d, &c) == PARSE_ERROR);
    CHECK(c == HLP_MAX_LINE && strcmp(adapter_last_error(), "bad length") == 0);
}

static void test_evp(void)
{
    sensor_data_t d;
    size_t c;
    CHECK(parse(&EVP_ADAPTER, MOTION_EX, sizeof MOTION_EX, &d, &c) == PARSE_OK);
    CHECK(c == sizeof MOTION_EX);
    CHECK(d.id == 30 && d.type == SENSOR_MOTION && d.value == 1.0 && d.ts == TS_EXAMPLE);
    check_prefixes(&EVP_ADAPTER, MOTION_EX, sizeof MOTION_EX);
    check_glued(&EVP_ADAPTER, MOTION_EX, sizeof MOTION_EX);

    CHECK(parse(&EVP_ADAPTER, DOOR_EX, sizeof DOOR_EX, &d, &c) == PARSE_OK);
    CHECK(c == sizeof DOOR_EX);
    CHECK(d.id == 40 && d.type == SENSOR_DOOR && d.value == 1.0 && d.ts == TS_EXAMPLE);
    check_prefixes(&EVP_ADAPTER, DOOR_EX, sizeof DOOR_EX);
    check_glued(&EVP_ADAPTER, DOOR_EX, sizeof DOOR_EX);

    /* Unknown type in front of a valid frame: resync on the valid header. */
    uint8_t buf[32] = { 0x07, 0x06, 0x00 };
    memcpy(buf + 3, DOOR_EX, sizeof DOOR_EX);
    CHECK(parse(&EVP_ADAPTER, buf, 3 + sizeof DOOR_EX, &d, &c) == PARSE_ERROR);
    CHECK(c == 3 && strcmp(adapter_last_error(), "bad type") == 0);
    CHECK(parse(&EVP_ADAPTER, buf + c, sizeof DOOR_EX, &d, &c) == PARSE_OK);

    /* Known type, wrong length. */
    const uint8_t badlen[] = { 0x01, 0x05, 0x00, 0x1E, 0x00 };
    CHECK(parse(&EVP_ADAPTER, badlen, sizeof badlen, &d, &c) == PARSE_ERROR);
    CHECK(c >= 1 && strcmp(adapter_last_error(), "bad length") == 0);

    /* Door state other than 0/1: the whole frame is dropped. */
    uint8_t badstate[sizeof DOOR_EX];
    memcpy(badstate, DOOR_EX, sizeof badstate);
    badstate[4] = 2;
    CHECK(parse(&EVP_ADAPTER, badstate, sizeof badstate, &d, &c) == PARSE_ERROR);
    CHECK(c == sizeof DOOR_EX && strcmp(adapter_last_error(), "bad value") == 0);

    CHECK(EVP_ADAPTER.port_offset == 2);
}

static void test_sgp(void)
{
    sensor_data_t d;
    size_t c;
    CHECK(parse(&SGP_ADAPTER, SGP_EX, sizeof SGP_EX, &d, &c) == PARSE_OK);
    CHECK(c == SGP_FRAME_LEN);
    CHECK(d.id == 50 && d.type == SENSOR_SMOKE_GAS && d.value == 120.0 && d.ts == TS_EXAMPLE);
    check_prefixes(&SGP_ADAPTER, SGP_EX, sizeof SGP_EX);
    check_glued(&SGP_ADAPTER, SGP_EX, sizeof SGP_EX);

    uint8_t bad[2 * SGP_FRAME_LEN];
    memcpy(bad, SGP_EX, SGP_FRAME_LEN);
    bad[10] ^= 0x01;                            /* bad CRC */
    CHECK(parse(&SGP_ADAPTER, bad, SGP_FRAME_LEN, &d, &c) == PARSE_ERROR);
    CHECK(c == SGP_FRAME_LEN && strcmp(adapter_last_error(), "bad CRC") == 0);

    /* Garbage before a frame: skipped up to the next magic byte. */
    bad[0] = 0x00;
    bad[1] = 0x11;
    memcpy(bad + 2, SGP_EX, SGP_FRAME_LEN);
    CHECK(parse(&SGP_ADAPTER, bad, 2 + SGP_FRAME_LEN, &d, &c) == PARSE_ERROR);
    CHECK(c == 2 && strcmp(adapter_last_error(), "bad magic") == 0);
    CHECK(parse(&SGP_ADAPTER, bad + 2, SGP_FRAME_LEN, &d, &c) == PARSE_OK);

    uint8_t reply[4];
    CHECK(SGP_ADAPTER.build_reply(PARSE_OK, reply, sizeof reply) == 1 && reply[0] == SGP_ACK);
    CHECK(SGP_ADAPTER.build_reply(PARSE_ERROR, reply, sizeof reply) == 1 && reply[0] == SGP_NAK);
    CHECK(SGP_ADAPTER.build_reply(PARSE_NEED_MORE, reply, sizeof reply) == 0);
    CHECK(SGP_ADAPTER.port_offset == 3);
}

int main(void)
{
    test_crc();
    test_tbp();
    test_hlp();
    test_evp();
    test_sgp();
    if (failures) {
        printf("test_adapters: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_adapters: all checks passed\n");
    return 0;
}
