/* sbuffer.h - the single measurement queue shared by the three threads */
#ifndef SBUFFER_H
#define SBUFFER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "sensor_types.h"

typedef enum {
    SBUF_READER_DATA,
    SBUF_READER_STORAGE,
    SBUF_READER_COUNT
} sbuf_reader_t;

typedef enum {
    SBUF_OK,
    SBUF_SHUTDOWN,     /* buffer shut down and nothing left for this reader */
    SBUF_ERR
} sbuf_result_t;

typedef struct sbuffer_node {
    sensor_data_t        data;
    bool                 read_by_data;      /* Data manager has read it    */
    bool                 read_by_storage;   /* Storage manager has read it */
    struct sbuffer_node *next;
} sbuffer_node_t;

typedef struct {
    sbuffer_node_t *head, *tail;
    size_t          size;
    bool            shutdown;               /* wake readers on exit */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;              /* wakes the readers    */
    
    sbuffer_node_t *cursor[SBUF_READER_COUNT];
} sbuffer_t;

int  sbuffer_init(sbuffer_t *sbuf);
void sbuffer_destroy(sbuffer_t *sbuf);

int  sbuffer_insert(sbuffer_t *sbuf, const sensor_data_t *data);

/* Blocks until a node this reader has not read yet is available, copies it
 * into *out and marks it read. The node is freed once both readers read it.
 * Returns SBUF_SHUTDOWN once the buffer is shut down and fully drained. */
sbuf_result_t sbuffer_read(sbuffer_t *sbuf, sbuf_reader_t reader,
                           sensor_data_t *out);

void   sbuffer_shutdown(sbuffer_t *sbuf);
size_t sbuffer_size(sbuffer_t *sbuf);

#endif /* SBUFFER_H */
