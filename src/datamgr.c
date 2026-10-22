#include "datamgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LINE_LEN  256

static const struct {
    const char   *name;
    sensor_type_t type;
} TYPE_NAMES[] = {
    { "temperature", SENSOR_TEMPERATURE },
    { "humidity",    SENSOR_HUMIDITY    },
    { "motion",      SENSOR_MOTION      },
    { "door",        SENSOR_DOOR        },
    { "smoke_gas",   SENSOR_SMOKE_GAS   },
};

static bool type_from_name(const char *name, sensor_type_t *out)
{
    for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; ++i) {
        if (strcmp(TYPE_NAMES[i].name, name) == 0) {
            *out = TYPE_NAMES[i].type;
            return true;
        }
    }
    return false;
}

/* At most MAX_SENSORS entries: a linear search is fast enough. */
static datamgr_sensor_t *find_sensor(datamgr_t *dm, sensor_id_t id)
{
    for (size_t i = 0; i < dm->n; ++i)
        if (dm->sensors[i].info.id == id)
            return &dm->sensors[i];
    return NULL;
}

static void sensor_reset(datamgr_sensor_t *s)
{
    bool averaged = s->info.type == SENSOR_TEMPERATURE ||
                    s->info.type == SENSOR_HUMIDITY;
    s->count = s->next = 0;
    s->running_avg = 0.0;
    s->state = averaged ? STATE_COLLECTING : STATE_NORMAL;
    s->level = (s->info.type == SENSOR_DOOR) ? -1 : 0;
}

int datamgr_init(datamgr_t *dm, const char *path)
{
    memset(dm, 0, sizeof *dm);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    char line[LINE_LEN];
    int lineno = 0;
    int rc = 0;
    while (rc == 0 && fgets(line, sizeof line, f) != NULL) {
        ++lineno;
        const char *p = line;
        while (isspace((unsigned char)*p))
            ++p;
        if (*p == '\0' || *p == '#')
            continue;

        unsigned room, id;
        char type_name[32], extra;
        sensor_type_t type;
        if (sscanf(p, "%u %u %31s %c", &room, &id, type_name, &extra) != 3 ||
            room > UINT16_MAX || id > UINT16_MAX) {
            fprintf(stderr, "%s:%d: expected '<room> <sensor_id> <type>'\n",
                    path, lineno);
            rc = -1;
        } else if (!type_from_name(type_name, &type)) {
            fprintf(stderr, "%s:%d: unknown sensor type '%s'\n",
                    path, lineno, type_name);
            rc = -1;
        } else if (find_sensor(dm, (sensor_id_t)id) != NULL) {
            fprintf(stderr, "%s:%d: duplicate sensor ID %u\n", path, lineno, id);
            rc = -1;
        } else if (dm->n == MAX_SENSORS) {
            fprintf(stderr, "%s:%d: more than %d sensors\n",
                    path, lineno, MAX_SENSORS);
            rc = -1;
        } else {
            datamgr_sensor_t *s = &dm->sensors[dm->n++];
            s->info.room = (uint16_t)room;
            s->info.id   = (sensor_id_t)id;
            s->info.type = type;
            sensor_reset(s);
        }
    }
    if (rc == 0 && ferror(f)) {
        perror(path);
        rc = -1;
    }
    fclose(f);

    DEBUG_PRINT("datamgr: %zu sensors loaded from %s", dm->n, path);
    return rc;
}

/* Adds a sample to the ring buffer; true once RUN_AVG_LENGTH samples exist. */
static bool running_avg_push(datamgr_sensor_t *s, double value)
{
    s->window[s->next] = value;
    s->next = (s->next + 1) % RUN_AVG_LENGTH;
    if (s->count < RUN_AVG_LENGTH)
        ++s->count;
    if (s->count < RUN_AVG_LENGTH)
        return false;

    double sum = 0.0;
    for (size_t i = 0; i < RUN_AVG_LENGTH; ++i)
        sum += s->window[i];
    s->running_avg = sum / RUN_AVG_LENGTH;
    return true;
}

/* Running average compared with [min, max]; logs only when the level changes
 * into an alert, so a sensor that stays too hot is reported once. */
static void handle_averaged(datamgr_sensor_t *s, const sensor_data_t *d,
                            double min, double max, const char *quantity,
                            const char *low_msg, const char *high_msg)
{
    if (!running_avg_push(s, d->value))
        return;                                 /* still STATE_COLLECTING */

    int level = s->running_avg < min ? -1 : s->running_avg > max ? 1 : 0;
    s->state = level != 0 ? STATE_ALERT : STATE_NORMAL;
    if (level == s->level)
        return;
    s->level = level;
    if (level != 0)
        printlog(LOG_SRC_DATA,
                 "The sensor node with %u reports it's %s (running avg %s = %.1f)",
                 s->info.id, level < 0 ? low_msg : high_msg, quantity,
                 s->running_avg);
}

static void handle_temperature(datamgr_t *dm, datamgr_sensor_t *s,
                               const sensor_data_t *d)
{
    (void)dm;
    handle_averaged(s, d, TEMP_MIN_C, TEMP_MAX_C, "temperature",
                    "too cold", "too hot");
}

static void handle_humidity(datamgr_t *dm, datamgr_sensor_t *s,
                            const sensor_data_t *d)
{
    (void)dm;
    handle_averaged(s, d, HUMIDITY_MIN, HUMIDITY_MAX, "humidity",
                    "too dry", "too humid");
}

/* Every motion frame is one detection: no state to compare with. */
static void handle_motion(datamgr_t *dm, datamgr_sensor_t *s,
                          const sensor_data_t *d)
{
    (void)dm;
    (void)d;
    printlog(LOG_SRC_DATA, "The sensor node with %u detected motion", s->info.id);
}

static void handle_door(datamgr_t *dm, datamgr_sensor_t *s,
                        const sensor_data_t *d)
{
    (void)dm;
    int level = d->value != 0.0;
    if (level == s->level)
        return;
    s->level = level;
    printlog(LOG_SRC_DATA, "The sensor node with %u reports the door is %s",
             s->info.id, level ? "open" : "closed");
}

/* Instantaneous threshold, no average: an alarm must not be delayed. */
static void handle_smoke_gas(datamgr_t *dm, datamgr_sensor_t *s,
                             const sensor_data_t *d)
{
    (void)dm;
    int level = d->value >= GAS_ALARM_PPM;
    s->state = level ? STATE_ALERT : STATE_NORMAL;
    if (level == s->level)
        return;
    s->level = level;
    if (level)
        printlog(LOG_SRC_DATA,
                 "The sensor node with %u reports a smoke/gas alarm (concentration = %.0f ppm)",
                 s->info.id, d->value);
}

/* Indexed by sensor_type_t: a new sensor type adds one line and one handler. */
static const sensor_handler_t HANDLERS[] = {
    [SENSOR_TEMPERATURE] = handle_temperature,
    [SENSOR_HUMIDITY]    = handle_humidity,
    [SENSOR_MOTION]      = handle_motion,
    [SENSOR_DOOR]        = handle_door,
    [SENSOR_SMOKE_GAS]   = handle_smoke_gas,
};

/* Logged once per ID: a misconfigured node sending many frames per second
 * would otherwise flood gateway.log. */
static void report_invalid(datamgr_t *dm, const sensor_data_t *d)
{
    uint8_t bit = (uint8_t)(1u << (d->id % 8));
    if (dm->invalid_seen[d->id / 8] & bit) {
        DEBUG_PRINT("datamgr: invalid sensor ID %u (type %d) dropped",
                    d->id, (int)d->type);
        return;
    }
    dm->invalid_seen[d->id / 8] |= bit;
    printlog(LOG_SRC_DATA, "Received sensor data with invalid sensor node ID %u",
             d->id);
}

void datamgr_process(datamgr_t *dm, const sensor_data_t *d)
{
    datamgr_sensor_t *s = find_sensor(dm, d->id);
    /* info.type comes from TYPE_NAMES, so it always has a handler. */
    if (s == NULL || s->info.type != d->type) {
        report_invalid(dm, d);
        return;
    }
    HANDLERS[d->type](dm, s, d);
}

void *datamgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_DATA, "Data manager started");

    /* Blocks inside sbuffer_read(); returns SBUF_SHUTDOWN once the buffer
     * is shut down and every remaining measurement has been read. The
     * per-sensor state is touched only by this thread, so needs no lock. */
    sensor_data_t d;
    while (sbuffer_read(&g->sbuf, SBUF_READER_DATA, &d) == SBUF_OK) {
        DEBUG_PRINT("datamgr: id=%u type=%d value=%.2f ts=%lld",
                    d.id, (int)d.type, d.value, (long long)d.ts);
        datamgr_process(&g->data, &d);
    }

    DEBUG_PRINT("datamgr: exiting");
    return NULL;
}
