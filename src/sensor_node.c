#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "adapters.h"
#include "byteorder.h"
#include "crc16.h"

#define FRAME_MAX       64      /* largest frame: an HLP line */
#define GLUE_FRAMES      3      /* frames per send() with --glue */
#define CORRUPT_ONE_IN  10      /* --corrupt: about 1 frame in 10 is bad */
#define FRAGMENT_GAP_MS 20      /* pause between the pieces of a frame */

#define SGP_ACK_TIMEOUT_MS 1000
#define SGP_MAX_RETRIES       3

typedef struct {
    const char *type;
    uint16_t    id;
    const char *host;
    uint16_t    base_port;
    long        period_ms;
    bool        fragment, glue, corrupt;
} node_cfg_t;

/* Simulated physical state; each kind uses the fields it needs. */
typedef struct {
    double  value;
    uint8_t door_open;
} node_state_t;

/* Writes one frame into buf and returns its length. With corrupt set, the
 * frame is made invalid in a way typical of the protocol. */
typedef size_t (*encode_fn)(node_state_t *st, uint16_t id, int64_t ts,
                            bool corrupt, uint8_t *buf);

typedef struct {
    const char *name;
    uint16_t    port_offset;
    bool        event;          /* sends on events at random intervals */
    bool        acked;          /* waits for ACK/NAK and resends (SGP) */
    double      start, step, low, high;   /* random walk of the value */
    encode_fn   encode;
} sensor_kind_t;

static volatile sig_atomic_t stop;

static void on_signal(int signo)
{
    (void)signo;
    stop = 1;
}

/* Random walk: both normal and alert values appear. */
static double walk(double v, const sensor_kind_t *k)
{
    v += ((double)rand() / RAND_MAX - 0.5) * 2.0 * k->step;
    if (v < k->low)
        v = k->low;
    if (v > k->high)
        v = k->high;
    return v;
}

static size_t tbp_encode(node_state_t *st, uint16_t id, int64_t ts,
                         bool corrupt, uint8_t *buf)
{
    double value = corrupt ? 999.0 : st->value;     /* out of range */
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    put_le16(buf, id);
    put_le64(buf + 2, bits);
    put_le64(buf + 10, (uint64_t)ts);
    return TBP_FRAME_LEN;
}

static size_t hlp_encode(node_state_t *st, uint16_t id, int64_t ts,
                         bool corrupt, uint8_t *buf)
{
    int n = corrupt
        ? snprintf((char *)buf, FRAME_MAX, "HUM %u %.1f,%lld\n", id, st->value, (long long)ts)
        : snprintf((char *)buf, FRAME_MAX, "HUM %u %.1f %lld\n", id, st->value, (long long)ts);
    return (size_t)n;
}

static size_t motion_encode(node_state_t *st, uint16_t id, int64_t ts,
                            bool corrupt, uint8_t *buf)
{
    (void)st;
    buf[0] = corrupt ? 0x07 : EVP_TYPE_MOTION;      /* unknown type */
    buf[1] = EVP_LEN_MOTION;
    put_be16(buf + 2, id);
    put_be32(buf + 4, (uint32_t)ts);
    return EVP_HDR_LEN + EVP_LEN_MOTION;
}

static size_t door_encode(node_state_t *st, uint16_t id, int64_t ts,
                          bool corrupt, uint8_t *buf)
{
    st->door_open = !st->door_open;                 /* each event toggles */
    buf[0] = corrupt ? 0x07 : EVP_TYPE_DOOR;
    buf[1] = EVP_LEN_DOOR;
    put_be16(buf + 2, id);
    buf[4] = st->door_open;
    put_be32(buf + 5, (uint32_t)ts);
    return EVP_HDR_LEN + EVP_LEN_DOOR;
}

static size_t sgp_encode(node_state_t *st, uint16_t id, int64_t ts,
                         bool corrupt, uint8_t *buf)
{
    buf[0] = SGP_MAGIC;
    put_be16(buf + 1, id);
    put_be16(buf + 3, (uint16_t)(st->value + 0.5));
    put_be32(buf + 5, (uint32_t)ts);
    put_be16(buf + 9, crc16_ccitt_false(buf + 1, 8));
    if (corrupt) {
        if (rand() % 2)
            buf[9] ^= 0x01;                         /* bad CRC */
        else
            buf[0] = 0x00;                          /* bad magic */
    }
    return SGP_FRAME_LEN;
}

static const sensor_kind_t KINDS[] = {
    { "temperature", 0, false, false, 18.0,  1.0, 10.0,  26.0, tbp_encode    },
    { "humidity",    1, false, false, 50.0,  3.0, 20.0,  80.0, hlp_encode    },
    { "motion",      2, true,  false,  0.0,  0.0,  0.0,   0.0, motion_encode },
    { "door",        2, true,  false,  0.0,  0.0,  0.0,   0.0, door_encode   },
    { "smoke_gas",   3, false, true, 100.0, 30.0, 50.0, 400.0, sgp_encode    },
};

static const sensor_kind_t *find_kind(const char *name)
{
    for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; ++i)
        if (strcmp(KINDS[i].name, name) == 0)
            return &KINDS[i];
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <type> <id> <host> <base port P> <period ms> [options]\n"
            "  type:    temperature | humidity | motion | door | smoke_gas\n"
            "  options: --fragment --glue --corrupt\n"
            "  e.g. %s temperature 10 127.0.0.1 1234 1000\n", prog, prog);
}

static bool parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return false;
    *out = v;
    return true;
}

static bool parse_args(int argc, char *argv[], node_cfg_t *cfg)
{
    if (argc < 6) {
        usage(argv[0]);
        return false;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->type = argv[1];
    cfg->host = argv[3];

    long v;
    if (!parse_long(argv[2], 0, 65535, &v)) {
        fprintf(stderr, "Invalid sensor id: '%s'\n", argv[2]);
        return false;
    }
    cfg->id = (uint16_t)v;
    if (!parse_long(argv[4], 1, 65535 - 3, &v)) {
        fprintf(stderr, "Invalid base port: '%s'\n", argv[4]);
        return false;
    }
    cfg->base_port = (uint16_t)v;
    if (!parse_long(argv[5], 1, 3600L * 1000, &cfg->period_ms)) {
        fprintf(stderr, "Invalid period: '%s'\n", argv[5]);
        return false;
    }

    for (int i = 6; i < argc; ++i) {
        if (strcmp(argv[i], "--fragment") == 0)
            cfg->fragment = true;
        else if (strcmp(argv[i], "--glue") == 0)
            cfg->glue = true;
        else if (strcmp(argv[i], "--corrupt") == 0)
            cfg->corrupt = true;
        else {
            fprintf(stderr, "Unknown option: '%s'\n", argv[i]);
            return false;
        }
    }

    if (find_kind(cfg->type) == NULL) {
        fprintf(stderr, "Unknown sensor type: '%s'\n", cfg->type);
        usage(argv[0]);
        return false;
    }
    return true;
}

static void now_str(char *buf, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static int connect_to(const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%u", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo %s: %s\n", host, gai_strerror(rc));
        errno = EINVAL;
        return -1;
    }

    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1)
        errno = saved_errno;
    return fd;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    /* Interrupted by SIGINT/SIGTERM (no SA_RESTART): the loop checks stop. */
    nanosleep(&ts, NULL);
}

/* send() may write fewer bytes than asked: loop until all are sent. */
static int send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* --fragment: split the bytes into 2 or 3 pieces with a pause in between,
 * so the gateway's recv() sees partial frames. */
static int send_bytes(int fd, const uint8_t *buf, size_t len, bool fragment)
{
    if (!fragment || len < 3)
        return send_all(fd, buf, len);

    size_t pieces = 2 + (size_t)(rand() % 2);
    size_t off = 0;
    for (size_t i = 1; i <= pieces; ++i) {
        size_t end = (i == pieces) ? len : off + 1 + (size_t)rand() % (len - off - (pieces - i));
        if (send_all(fd, buf + off, end - off) == -1)
            return -1;
        off = end;
        if (i < pieces)
            sleep_ms(FRAGMENT_GAP_MS);
    }
    return 0;
}

/* Waits for the SGP reply. Returns 1 on ACK, 0 on NAK/timeout, -1 if the
 * connection is gone. */
static int wait_reply(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc = poll(&pfd, 1, SGP_ACK_TIMEOUT_MS);
    if (rc <= 0)
        return 0;                   /* timeout, or EINTR: stop is checked */
    uint8_t b;
    ssize_t n = recv(fd, &b, 1, 0);
    if (n == 0)
        errno = ECONNRESET;         /* gateway closed the connection */
    if (n <= 0)
        return -1;
    return b == SGP_ACK;
}

/* Sends one SGP frame and resends it, up to SGP_MAX_RETRIES times, until it
 * is ACKed. A resend carries the original, uncorrupted frame, as a real
 * sensor would after noise on the line. Returns -1 if the connection is gone. */
static int send_acked(int fd, const uint8_t *good, const uint8_t *first,
                      size_t len, bool fragment, uint16_t id)
{
    const uint8_t *frame = first;
    for (int attempt = 0; attempt <= SGP_MAX_RETRIES && !stop; ++attempt) {
        if (send_bytes(fd, frame, len, fragment) == -1)
            return -1;
        int r = wait_reply(fd);
        if (r == -1)
            return -1;
        if (r == 1)
            return 0;
        printf("sensor %u: no ACK (attempt %d), resending\n", id, attempt + 1);
        frame = good;
    }
    printf("sensor %u: frame given up after %d retries\n", id, SGP_MAX_RETRIES);
    return 0;
}

int main(int argc, char *argv[])
{
    node_cfg_t cfg;
    if (!parse_args(argc, argv, &cfg))
        return EXIT_FAILURE;
    const sensor_kind_t *kind = find_kind(cfg.type);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Line-buffered even when redirected to a file by the test script. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)(time(NULL) ^ getpid() ^ cfg.id));

    if (kind->acked && cfg.glue)
        fprintf(stderr, "Note: --glue is ignored for smoke_gas, each frame waits for its ACK\n");
    bool glue = cfg.glue && !kind->acked;

    uint16_t port = (uint16_t)(cfg.base_port + kind->port_offset);
    char ts_buf[32];
    int fd = connect_to(cfg.host, port);
    now_str(ts_buf, sizeof ts_buf);
    if (fd == -1) {
        printf("%s sensor %u %s connect failed (%s)\n",
               ts_buf, cfg.id, cfg.type, strerror(errno));
        return EXIT_FAILURE;
    }
    printf("%s sensor %u %s connected to %s:%u\n",
           ts_buf, cfg.id, cfg.type, cfg.host, port);

    node_state_t st = { .value = kind->start, .door_open = 0 };
    unsigned long n_frames = 0;
    int exit_code = EXIT_SUCCESS;
    while (!stop) {
        uint8_t buf[GLUE_FRAMES * FRAME_MAX];
        uint8_t good[FRAME_MAX];
        size_t len = 0;
        int batch = glue ? GLUE_FRAMES : 1;

        for (int i = 0; i < batch; ++i) {
            st.value = walk(st.value, kind);
            int64_t ts = (int64_t)time(NULL);
            /* Never corrupt the first frame: the gateway would drop the
             * connection as "malformed first frame". */
            bool corrupt = cfg.corrupt && n_frames > 0 && rand() % CORRUPT_ONE_IN == 0;
            /* The SGP encoder does not change st: encoding twice is safe. */
            if (kind->acked)
                kind->encode(&st, cfg.id, ts, false, good);
            len += kind->encode(&st, cfg.id, ts, corrupt, buf + len);
            ++n_frames;

            const char *mark = corrupt ? " (corrupt)" : "";
            if (strcmp(kind->name, "motion") == 0)
                printf("sent id=%u motion ts=%lld%s\n", cfg.id, (long long)ts, mark);
            else if (strcmp(kind->name, "door") == 0)
                printf("sent id=%u door=%s ts=%lld%s\n", cfg.id,
                       st.door_open ? "open" : "closed", (long long)ts, mark);
            else
                printf("sent id=%u value=%.2f ts=%lld%s\n", cfg.id, st.value,
                       (long long)ts, mark);
        }

        int rc = kind->acked
            ? send_acked(fd, good, buf, len, cfg.fragment, cfg.id)
            : send_bytes(fd, buf, len, cfg.fragment);
        if (rc == -1) {
            fprintf(stderr, "sensor %u: send: %s\n", cfg.id, strerror(errno));
            exit_code = EXIT_FAILURE;
            break;
        }

        /* Events (motion, door) come at random intervals with mean period. */
        long wait = kind->event
            ? cfg.period_ms / 2 + rand() % (cfg.period_ms + 1)
            : cfg.period_ms;
        sleep_ms(wait);
    }

    close(fd);
    return exit_code;
}
