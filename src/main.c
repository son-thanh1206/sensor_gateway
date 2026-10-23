#define _POSIX_C_SOURCE 200809L

#include "connmgr.h"
#include "datamgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"
#include "storagemgr.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static gateway_t gateway;
static volatile sig_atomic_t caught_signal;

/* Only assigns volatile sig_atomic_t variables: anything else (printf,
 * malloc, mutexes...) is not async-signal-safe (man 7 signal-safety). */
static void on_signal(int signo)
{
    caught_signal = signo;
    gateway.stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <TCP base port P> <HTTP port>\n"
                    "  e.g. %s 1234 8080\n", prog, prog);
}

static bool parse_port(const char *s, uint16_t *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_args(int argc, char *argv[])
{
    if (argc != 3) {
        usage(argv[0]);
        return false;
    }
    if (!parse_port(argv[1], &gateway.base_port)) {
        fprintf(stderr, "Invalid TCP base port: '%s' (1-65535)\n", argv[1]);
        return false;
    }
    if (!parse_port(argv[2], &gateway.http_port)) {
        fprintf(stderr, "Invalid HTTP port: '%s' (1-65535)\n", argv[2]);
        return false;
    }
    if (gateway.base_port > 65535 - (N_PORTS - 1)) {
        fprintf(stderr, "TCP base port too large: P+%d must be <= 65535\n",
                N_PORTS - 1);
        return false;
    }
    if (gateway.http_port >= gateway.base_port &&
        gateway.http_port < gateway.base_port + N_PORTS) {
        fprintf(stderr, "HTTP port must not be in range %u-%u\n",
                gateway.base_port, gateway.base_port + N_PORTS - 1);
        return false;
    }
    return true;
}

/* Every sensor node holds one descriptor, so the soft limit (often 1024)
 * would cap the number of nodes; the hard limit is as far as we may go. */
static void raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1 || rl.rlim_cur == rl.rlim_max)
        return;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
        DEBUG_PRINT("setrlimit RLIMIT_NOFILE: %s", strerror(errno));
}

static int install_signal_handlers(sigset_t *old_mask)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    /* SIGUSR1 is sent by gateway_fail() when a worker thread gives up. */
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    /* If the log process dies, write() to the FIFO returns EPIPE instead
     * of SIGPIPE killing the whole gateway. */
    signal(SIGPIPE, SIG_IGN);

    /* Threads created later inherit this mask, so SIGINT/SIGTERM/SIGUSR1
     * can only be delivered to the main thread, inside sigsuspend(). */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    int rc = pthread_sigmask(SIG_BLOCK, &block, old_mask);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        return -1;
    }
    return 0;
}

typedef struct {
    const char *name;
    void *(*task)(void *);
} worker_t;

/* connmgr must stay first: it is the only sbuffer writer, joined first. */
static const worker_t WORKERS[] = {
    { "connmgr",    connmgr_thread    },
    { "datamgr",    datamgr_thread    },
    { "storagemgr", storagemgr_thread },
};
#define N_WORKERS (sizeof WORKERS / sizeof WORKERS[0])

int main(int argc, char *argv[])
{
    DEBUG_PRINT("Main process PID: %d", getpid());
    if (!parse_args(argc, argv))
        return EXIT_FAILURE;
    raise_fd_limit();

    sigset_t old_mask;
    if (install_signal_handlers(&old_mask) == -1)
        return EXIT_FAILURE;

    /* Fork while still single-threaded: fork() copies only the calling
     * thread, so a mutex held by another thread would stay locked forever
     * in the child. */
    gateway.log_pid = log_start(LOG_FIFO_PATH, LOG_FILE_PATH);
    if (gateway.log_pid == -1)
        return EXIT_FAILURE;
    gateway.web_pid = -1;

    printlog(LOG_SRC_MAIN, "Gateway started (TCP base port %u, HTTP port %u)",
              gateway.base_port, gateway.http_port);

    int exit_code = EXIT_SUCCESS;
    int rc = gateway_sync_init(&gateway);
    if (rc != 0) {
        fprintf(stderr, "gateway_sync_init fail: %s\n", strerror(rc));
        goto out_log;
    }
    rc = sbuffer_init(&gateway.sbuf);
    if (rc != 0) {
        fprintf(stderr, "sbuffer_init fail: %s\n", strerror(rc));
        goto out_sync;
    }
    /* Loaded before the threads start: a bad config file stops the gateway
     * before any sensor is accepted. */
    if (datamgr_init(&gateway.data, ROOM_SENSOR_PATH) == -1)
        goto out_sbuf;
    if (connmgr_init(&gateway.conn, gateway.base_port) == -1)
        goto out_sbuf;
    storagemgr_init(&gateway.stor, STORAGE_DB_PATH);

    pthread_t tids[N_WORKERS];
    size_t n_started = 0;
    for (; n_started < N_WORKERS; ++n_started) {
        rc = pthread_create(&tids[n_started], NULL,
                            WORKERS[n_started].task, &gateway);
        if (rc != 0) {
            fprintf(stderr, "pthread_create %s: %s\n",
                    WORKERS[n_started].name, strerror(rc));
            exit_code = EXIT_FAILURE;
            break;
        }
        DEBUG_PRINT("thread %s created", WORKERS[n_started].name);
    }

    /* sigsuspend() unblocks the signals and sleeps atomically. With
     * pause(), a signal arriving between the check and the sleep would
     * be missed and the program would hang. */
    if (exit_code == EXIT_SUCCESS) {
        while (!gateway.stop)
            sigsuspend(&old_mask);
    }

    const char *fail_reason = gateway_fail_reason(&gateway);
    if (exit_code != EXIT_SUCCESS) {
        printlog(LOG_SRC_MAIN, "Gateway shutting down (thread creation failed)");
    } else if (fail_reason != NULL) {
        printlog(LOG_SRC_MAIN, "Gateway shutting down (%s)", fail_reason);
        exit_code = EXIT_FAILURE;
    } else {
        printlog(LOG_SRC_MAIN, "Gateway shutting down (received %s)",
                 caught_signal == SIGTERM ? "SIGTERM" :
                 caught_signal == SIGUSR1 ? "SIGUSR1" : "SIGINT");
    }

    gateway_request_stop(&gateway);
    connmgr_wakeup(&gateway.conn);

    /* Join the only writer (WORKERS[0]) first, then shut the sbuffer down:
     * no measurement can be inserted after the readers have drained it. */
    if (n_started > 0) {
        pthread_join(tids[0], NULL);
        DEBUG_PRINT("thread %s joined", WORKERS[0].name);
    }
    sbuffer_shutdown(&gateway.sbuf);
    for (size_t i = 1; i < n_started; ++i) {
        pthread_join(tids[i], NULL);
        DEBUG_PRINT("thread %s joined", WORKERS[i].name);
    }

    connmgr_cleanup(&gateway.conn);
    sbuffer_destroy(&gateway.sbuf);
    gateway_sync_destroy(&gateway);

    /* Only after all threads are joined: they may log until they exit. */
    if (log_stop(gateway.log_pid) != 0)
        exit_code = EXIT_FAILURE;

    return exit_code;

out_sbuf:
    sbuffer_destroy(&gateway.sbuf);
out_sync:
    gateway_sync_destroy(&gateway);
out_log:
    printlog(LOG_SRC_MAIN, "Gateway shutting down (init error)");
    log_stop(gateway.log_pid);
    return EXIT_FAILURE;
}