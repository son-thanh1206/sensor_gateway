/*
 * test_sensors.c - simulate real sensors: start one sensor_node client
 * process per line of a CSV file, all sending to the same TCP server.
 *
 *   bin/test_sensors [config.csv] [seconds]
 *
 * CSV columns: type,id,host,period_ms,options (see test/sensors.csv).
 * Lines that do not parse are skipped silently. The port comes from the PORT
 * environment variable (default 1234). With seconds = 0 (the default) the
 * sensors run until SIGINT/SIGTERM; all child processes are then killed at once.
 * Output of each node goes to test/out/node_<id>.out.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUT_DIR      "test/out"
#define NODE_BIN     "./bin/sensor_node"
#define MAX_SENSORS  64
#define MAX_ARGS     16
#define LINE_LEN     512

static const char *const SENSOR_TYPES[] = {
    "temperature", "humidity", "motion", "door", "smoke_gas",
};

static pid_t pids[MAX_SENSORS];
static volatile sig_atomic_t n_children;
static volatile sig_atomic_t stop_requested;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int is_sensor_type(const char *s)
{
    for (size_t i = 0; i < sizeof SENSOR_TYPES / sizeof *SENSOR_TYPES; ++i)
        if (strcmp(SENSOR_TYPES[i], s) == 0)
            return 1;
    return 0;
}

static int is_number(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s; ++s)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

/* Async-signal-safe: also called from the signal handler. */
static void kill_all_children(void)
{
    for (int i = 0; i < n_children; ++i)
        kill(pids[i], SIGINT);
}

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
    kill_all_children();
}

static void install_signal_handlers(void)
{
    struct sigaction sa = {.sa_handler = on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static pid_t spawn_node(char *fields[5], const char *port)
{
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &previous);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        sigprocmask(SIG_SETMASK, &previous, NULL);

        char path[PATH_MAX];
        snprintf(path, sizeof path, OUT_DIR "/node_%s.out", fields[1]);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        char *argv[MAX_ARGS];
        int argc = 0;
        argv[argc++] = NODE_BIN;
        argv[argc++] = fields[0];
        argv[argc++] = fields[1];
        argv[argc++] = fields[2];
        argv[argc++] = (char *)port;
        argv[argc++] = fields[3];
        for (char *tok = strtok(fields[4], " \t"); tok && argc < MAX_ARGS - 1;
             tok = strtok(NULL, " \t"))
            argv[argc++] = tok;
        argv[argc] = NULL;
        execv(NODE_BIN, argv);
        _exit(127);
    }
    if (pid > 0) {
        pids[n_children] = pid;
        n_children++;
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return pid;
}

static void start_sensors(const char *config, const char *port)
{
    FILE *f = fopen(config, "r");
    if (!f)
        return;

    char line[LINE_LEN];
    while (!stop_requested && n_children < MAX_SENSORS && fgets(line, sizeof line, f)) {
        char *fields[5] = {"", "", "", "", ""};
        char *p = line;
        for (int i = 0; i < 5; ++i) {
            char *comma = (i < 4) ? strchr(p, ',') : NULL;
            if (comma)
                *comma = '\0';
            fields[i] = trim(p);
            if (!comma)
                break;
            p = comma + 1;
        }
        if (!is_sensor_type(fields[0]) || !is_number(fields[1]) ||
            fields[2][0] == '\0' || !is_number(fields[3]))
            continue;

        if (spawn_node(fields, port) > 0)
            usleep(100000);
    }
    fclose(f);
}

static void wait_for_children(void)
{
    while (wait(NULL) > 0 || errno == EINTR)
        ;
}

int main(int argc, char *argv[])
{
    const char *config = argc > 1 ? argv[1] : "test/sensors.csv";
    unsigned duration = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    const char *port = getenv("PORT") ? getenv("PORT") : "1234";

    mkdir(OUT_DIR, 0755);
    install_signal_handlers();
    start_sensors(config, port);
    printf("started %d sensor nodes:", (int)n_children);
    for (int i = 0; i < n_children; ++i)
        printf(" %d", (int)pids[i]);
    printf("\n");
    fflush(stdout);

    if (duration > 0) {
        for (unsigned left = duration; left > 0 && !stop_requested; --left)
            sleep(1);
        kill_all_children();
    } else {
        while (!stop_requested)
            pause();
    }
    wait_for_children();
    return 0;
}
