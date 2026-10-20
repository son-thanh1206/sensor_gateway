#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
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

#define TBP_FRAME_LEN  18
#define TEMP_START     18.0
#define TEMP_STEP       1.0     /* max change between two measurements */
#define TEMP_LOW       10.0     /* random walk stays in [LOW, HIGH] so  */
#define TEMP_HIGH      26.0     /* both normal and alert values appear  */

typedef struct {
    const char *type;
    uint16_t    id;
    const char *host;
    uint16_t    base_port;
    long        period_ms;
    bool        fragment, glue, corrupt;
} node_cfg_t;

static volatile sig_atomic_t stop;

static void on_signal(int signo)
{
    (void)signo;
    stop = 1;
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

    if (strcmp(cfg->type, "temperature") != 0) {
        fprintf(stderr, "Sensor type '%s' is not implemented yet\n", cfg->type);
        return false;
    }
    if (cfg->fragment || cfg->glue || cfg->corrupt)
        fprintf(stderr, "Note: --fragment/--glue/--corrupt are ignored until Step 4\n");
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

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void tbp_encode(uint8_t frame[TBP_FRAME_LEN], uint16_t id,
                       double value, int64_t ts)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    put_le16(frame, id);
    put_le64(frame + 2, bits);
    put_le64(frame + 10, (uint64_t)ts);
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

static double next_temperature(double t)
{
    t += ((double)rand() / RAND_MAX - 0.5) * 2.0 * TEMP_STEP;
    if (t < TEMP_LOW)
        t = TEMP_LOW;
    if (t > TEMP_HIGH)
        t = TEMP_HIGH;
    return t;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    /* Interrupted by SIGINT/SIGTERM (no SA_RESTART): the loop checks stop. */
    nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
    node_cfg_t cfg;
    if (!parse_args(argc, argv, &cfg))
        return EXIT_FAILURE;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Line-buffered even when redirected to a file by the test script. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)(time(NULL) ^ getpid() ^ cfg.id));

    uint16_t port = cfg.base_port;          /* TBP: port offset 0 */
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

    double temp = TEMP_START;
    int exit_code = EXIT_SUCCESS;
    while (!stop) {
        temp = next_temperature(temp);
        int64_t ts = (int64_t)time(NULL);
        uint8_t frame[TBP_FRAME_LEN];
        tbp_encode(frame, cfg.id, temp, ts);

        if (send_all(fd, frame, sizeof frame) == -1) {
            fprintf(stderr, "sensor %u: send: %s\n", cfg.id, strerror(errno));
            exit_code = EXIT_FAILURE;
            break;
        }
        printf("sent id=%u value=%.2f ts=%lld\n", cfg.id, temp, (long long)ts);
        sleep_ms(cfg.period_ms);
    }

    close(fd);
    return exit_code;
}
