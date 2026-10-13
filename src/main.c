#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"

#define THREAD_COUNT 3

static void *connmgr_thread(void *arg)
{
    (void)arg;
    printf("CONN Connection manager thread started\n");
    return NULL;
}

static void *datamgr_thread(void *arg)
{
    (void)arg;
    printf("DATA Data manager thread started\n");
    return NULL;
}

static void *storagemgr_thread(void *arg)
{
    (void)arg;
    printf("STOR Storage manager thread started\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    void *(*const entries[THREAD_COUNT])(void *) = {
        connmgr_thread, datamgr_thread, storagemgr_thread
    };
    pthread_t threads[THREAD_COUNT];
    int created = 0;
    int status = EXIT_SUCCESS;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <tcp_base_port> <http_port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("MAIN Gateway started (TCP base port %s, HTTP port %s)\n",
           argv[1], argv[2]);

    for (; created < THREAD_COUNT; created++) {
        int err = pthread_create(&threads[created], NULL, entries[created], NULL);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            status = EXIT_FAILURE;
            break;
        }
    }

    for (int i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    DEBUG_PRINT("%d thread(s) joined", created);

    printf("MAIN Gateway shutting down (%s)\n",
           status == EXIT_SUCCESS ? "normal exit" : "thread creation failed");

    return status;
}
