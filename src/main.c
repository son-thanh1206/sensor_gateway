#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "gateway.h"

#define THREAD_COUNT     3
#define LAST_PORT_OFFSET 3

static gateway_t gw = {
    .stop_lock = PTHREAD_MUTEX_INITIALIZER,
    .stop_cond = PTHREAD_COND_INITIALIZER,
};

static void handle_stop_signal(int signo)
{
    gw.stop = signo;
}

static bool parse_port(const char *text, uint16_t *port)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > UINT16_MAX)
        return false;

    *port = (uint16_t)value;
    return true;
}

static bool parse_args(int argc, char *argv[], gateway_t *gateway)
{
    if (argc != 3)
        return false;
    if (!parse_port(argv[1], &gateway->base_port) ||
        !parse_port(argv[2], &gateway->http_port))
        return false;
    if (gateway->base_port > UINT16_MAX - LAST_PORT_OFFSET)
        return false;

    return gateway->http_port < gateway->base_port ||
           gateway->http_port > gateway->base_port + LAST_PORT_OFFSET;
}

static void wait_for_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    while (!gateway->stop_requested)
        pthread_cond_wait(&gateway->stop_cond, &gateway->stop_lock);
    pthread_mutex_unlock(&gateway->stop_lock);
}

static void request_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    gateway->stop_requested = true;
    pthread_cond_broadcast(&gateway->stop_cond);
    pthread_mutex_unlock(&gateway->stop_lock);
}

static void *connmgr_thread(void *arg)
{
    printf("CONN Connection manager thread started\n");
    DEBUG_PRINT("connection manager thread waiting for stop");
    wait_for_stop(arg);
    DEBUG_PRINT("connection manager thread exiting");
    return NULL;
}

static void *datamgr_thread(void *arg)
{
    printf("DATA Data manager thread started\n");
    DEBUG_PRINT("data manager thread waiting for stop");
    wait_for_stop(arg);
    DEBUG_PRINT("data manager thread exiting");
    return NULL;
}

static void *storagemgr_thread(void *arg)
{
    printf("STOR Storage manager thread started\n");
    DEBUG_PRINT("storage manager thread waiting for stop");
    wait_for_stop(arg);
    DEBUG_PRINT("storage manager thread exiting");
    return NULL;
}

int main(int argc, char *argv[])
{
    void *(*const entries[THREAD_COUNT])(void *) = {
        connmgr_thread, datamgr_thread, storagemgr_thread
    };
    pthread_t threads[THREAD_COUNT];
    struct sigaction action = { .sa_handler = handle_stop_signal };
    sigset_t stop_signals, original_mask;
    const char *reason;
    int created = 0;

    if (!parse_args(argc, argv, &gw)) {
        fprintf(stderr,
                "Usage: %s <tcp_base_port> <http_port>\n"
                "  ports are 1..65535; the gateway listens on tcp_base_port..+%d,\n"
                "  so http_port must be outside that range\n",
                argv[0], LAST_PORT_OFFSET);
        return EXIT_FAILURE;
    }

    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    /* Block before creating the threads: they inherit this mask, so the  */
    /* signals reach only the main thread, and only inside sigsuspend().  */
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, &original_mask);

    printf("MAIN Gateway started (TCP base port %u, HTTP port %u)\n",
           gw.base_port, gw.http_port);

    for (; created < THREAD_COUNT; created++) {
        int err = pthread_create(&threads[created], NULL, entries[created], &gw);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            break;
        }
    }

    if (created == THREAD_COUNT) {
        while (!gw.stop)
            sigsuspend(&original_mask);
        reason = gw.stop == SIGINT ? "SIGINT" : "SIGTERM";
    } else {
        reason = "thread creation failed";
    }

    printf("MAIN Gateway shutting down (%s)\n", reason);

    request_stop(&gw);
    for (int i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    DEBUG_PRINT("%d thread(s) joined", created);

    return created == THREAD_COUNT ? EXIT_SUCCESS : EXIT_FAILURE;
}
