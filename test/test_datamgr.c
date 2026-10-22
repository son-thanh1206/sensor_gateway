/*
 * test_datamgr.c - unit tests of the data manager: room-sensor file loading,
 * ID/type validation and the per-type handlers (assignment Step 5).
 *
 *   make test-datamgr
 *
 * printlog() is stubbed here, so the log messages can be checked exactly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "datamgr.h"
#include "logger.h"

#define MAP_PATH  "test/out/test_room_sensor.map"

static int failures;

#define CHECK(cond) do {                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static int  n_logs;
static char last_log[LOG_MSG_MAX + 1];

int printlog(log_src_t src, const char *fmt, ...)
{
    (void)src;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    ++n_logs;
    return 0;
}

static datamgr_t dm;

/* Feeds one measurement; returns the number of log lines it produced. */
static int feed(sensor_id_t id, sensor_type_t type, double value)
{
    sensor_data_t d = { .id = id, .type = type, .value = value, .ts = 0 };
    int before = n_logs;
    last_log[0] = '\0';
    datamgr_process(&dm, &d);
    return n_logs - before;
}

static int write_map(const char *content)
{
    FILE *f = fopen(MAP_PATH, "w");
    if (f == NULL) {
        perror(MAP_PATH);
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return datamgr_init(&dm, MAP_PATH);
}

static void test_load(void)
{
    CHECK(write_map("# room sensor_id type\n"
                    "\n"
                    "501 10 temperature\n"
                    "  501 21 humidity   \n"
                    "602 30 motion\n"
                    "703 40 door\n"
                    "804 50 smoke_gas\n") == 0);
    CHECK(dm.n == 5);
    CHECK(dm.sensors[1].info.room == 501 && dm.sensors[1].info.id == 21 &&
          dm.sensors[1].info.type == SENSOR_HUMIDITY);
    CHECK(dm.sensors[0].state == STATE_COLLECTING);
    CHECK(dm.sensors[3].level == -1);

    CHECK(write_map("501 10 pressure\n") == -1);       /* unknown type   */
    CHECK(write_map("501 10 door\n502 10 door\n") == -1); /* duplicate ID */
    CHECK(write_map("501 10\n") == -1);                /* missing field  */
    CHECK(write_map("501 10 door extra\n") == -1);     /* extra field    */
    CHECK(write_map("501 70000 door\n") == -1);        /* ID too large   */
    CHECK(datamgr_init(&dm, "test/out/does_not_exist.map") == -1);
}

static void reload(void)
{
    write_map("501 10 temperature\n501 21 humidity\n602 30 motion\n"
              "703 40 door\n804 50 smoke_gas\n");
}

static void test_temperature(void)
{
    reload();
    for (int i = 0; i < 4; ++i)
        CHECK(feed(10, SENSOR_TEMPERATURE, 25.0) == 0);  /* collecting */
    CHECK(dm.sensors[0].state == STATE_COLLECTING);
    CHECK(feed(10, SENSOR_TEMPERATURE, 25.0) == 1);
    CHECK(strcmp(last_log, "The sensor node with 10 reports it's too hot "
                           "(running avg temperature = 25.0)") == 0);
    CHECK(dm.sensors[0].state == STATE_ALERT);
    CHECK(feed(10, SENSOR_TEMPERATURE, 26.0) == 0);      /* not repeated */

    for (int i = 0; i < 5; ++i)
        CHECK(feed(10, SENSOR_TEMPERATURE, 17.0) == 0);  /* back to normal */
    CHECK(dm.sensors[0].state == STATE_NORMAL);

    int logs = 0;
    for (int i = 0; i < 5 && logs == 0; ++i)
        logs += feed(10, SENSOR_TEMPERATURE, 10.0);
    CHECK(logs == 1 && strstr(last_log, "reports it's too cold") != NULL);
    CHECK(feed(10, SENSOR_TEMPERATURE, 10.0) == 0);
    CHECK(dm.sensors[0].level == -1);
}

static void test_humidity(void)
{
    reload();
    for (int i = 0; i < 4; ++i)
        CHECK(feed(21, SENSOR_HUMIDITY, 50.0) == 0);
    CHECK(feed(21, SENSOR_HUMIDITY, 50.0) == 0);         /* normal: no log */
    CHECK(dm.sensors[1].state == STATE_NORMAL);

    int logs = 0;
    for (int i = 0; i < 5 && logs == 0; ++i)
        logs += feed(21, SENSOR_HUMIDITY, 95.0);
    CHECK(logs == 1 && strstr(last_log, "too humid (running avg humidity = ") != NULL);

    logs = 0;
    for (int i = 0; i < 5 && logs == 0; ++i)
        logs += feed(21, SENSOR_HUMIDITY, 5.0);
    CHECK(logs == 1 && strstr(last_log, "reports it's too dry") != NULL);
}

static void test_smoke_gas(void)
{
    reload();
    CHECK(feed(50, SENSOR_SMOKE_GAS, 120.0) == 0);
    CHECK(feed(50, SENSOR_SMOKE_GAS, 350.0) == 1);       /* no averaging */
    CHECK(strcmp(last_log, "The sensor node with 50 reports a smoke/gas alarm "
                           "(concentration = 350 ppm)") == 0);
    CHECK(feed(50, SENSOR_SMOKE_GAS, 400.0) == 0);
    CHECK(feed(50, SENSOR_SMOKE_GAS, 100.0) == 0);
    CHECK(dm.sensors[4].state == STATE_NORMAL);
    CHECK(feed(50, SENSOR_SMOKE_GAS, GAS_ALARM_PPM) == 1); /* >= threshold */
}

static void test_events(void)
{
    reload();
    CHECK(feed(30, SENSOR_MOTION, 1.0) == 1);
    CHECK(strcmp(last_log, "The sensor node with 30 detected motion") == 0);
    CHECK(feed(30, SENSOR_MOTION, 1.0) == 1);            /* every event */

    CHECK(feed(40, SENSOR_DOOR, 1.0) == 1);
    CHECK(strcmp(last_log, "The sensor node with 40 reports the door is open") == 0);
    CHECK(feed(40, SENSOR_DOOR, 1.0) == 0);              /* no change */
    CHECK(feed(40, SENSOR_DOOR, 0.0) == 1);
    CHECK(strcmp(last_log, "The sensor node with 40 reports the door is closed") == 0);
}

static void test_invalid(void)
{
    reload();
    CHECK(feed(99, SENSOR_TEMPERATURE, 18.0) == 1);
    CHECK(strcmp(last_log, "Received sensor data with invalid sensor node ID 99") == 0);
    CHECK(feed(99, SENSOR_TEMPERATURE, 18.0) == 0);      /* once per ID */

    CHECK(feed(10, SENSOR_HUMIDITY, 50.0) == 1);         /* type mismatch */
    CHECK(strcmp(last_log, "Received sensor data with invalid sensor node ID 10") == 0);
    CHECK(dm.sensors[0].count == 0);                     /* not processed */
}

int main(void)
{
    test_load();
    test_temperature();
    test_humidity();
    test_smoke_gas();
    test_events();
    test_invalid();

    if (failures == 0)
        printf("test_datamgr: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
