/* test_sbuffer.c - unit test of the two-reader sbuffer
 *
 * One writer inserts N_ITEMS measurements with increasing ts while two
 * readers consume concurrently. Each reader must see every item exactly once
 * and in order, and the buffer must be empty at the end.
 * Also run it under: valgrind --tool=helgrind ./bin/test_sbuffer
 */
#include "sbuffer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define N_ITEMS 10000

static sbuffer_t sbuf;

typedef struct {
    sbuf_reader_t reader;
    size_t        count;
    int           errors;
} reader_ctx_t;

static void *writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < N_ITEMS; ++i) {
        sensor_data_t d = {
            .id = (sensor_id_t)(i % 5), .type = SENSOR_TEMPERATURE,
            .value = i * 0.5, .ts = (time_t)i,
        };
        if (sbuffer_insert(&sbuf, &d) != 0) {
            fprintf(stderr, "insert %d failed\n", i);
            exit(EXIT_FAILURE);
        }
    }
    return NULL;
}

static void *reader(void *arg)
{
    reader_ctx_t *ctx = arg;
    sensor_data_t d;
    while (sbuffer_read(&sbuf, ctx->reader, &d) == SBUF_OK) {
        if (d.ts != (time_t)ctx->count || d.value != ctx->count * 0.5)
            ctx->errors++;
        ctx->count++;
    }
    return NULL;
}

int main(void)
{
    if (sbuffer_init(&sbuf) != 0) {
        fprintf(stderr, "sbuffer_init failed\n");
        return EXIT_FAILURE;
    }

    reader_ctx_t ctx[SBUF_READER_COUNT] = {
        { .reader = SBUF_READER_DATA },
        { .reader = SBUF_READER_STORAGE },
    };
    pthread_t rt[SBUF_READER_COUNT], wt;
    for (int r = 0; r < SBUF_READER_COUNT; ++r)
        pthread_create(&rt[r], NULL, reader, &ctx[r]);
    pthread_create(&wt, NULL, writer, NULL);

    /* Same shutdown order as the gateway: writer first, then readers. */
    pthread_join(wt, NULL);
    sbuffer_shutdown(&sbuf);
    for (int r = 0; r < SBUF_READER_COUNT; ++r)
        pthread_join(rt[r], NULL);

    int failed = 0;
    for (int r = 0; r < SBUF_READER_COUNT; ++r) {
        printf("reader %d: %zu items, %d out of order\n",
               r, ctx[r].count, ctx[r].errors);
        if (ctx[r].count != N_ITEMS || ctx[r].errors != 0)
            failed = 1;
    }
    size_t left = sbuffer_size(&sbuf);
    printf("items left in sbuffer: %zu\n", left);
    if (left != 0)
        failed = 1;

    sensor_data_t d = { .id = 1 };
    if (sbuffer_insert(&sbuf, &d) == 0) {
        printf("insert after shutdown was accepted\n");
        failed = 1;
    }

    sbuffer_destroy(&sbuf);
    printf("%s\n", failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
