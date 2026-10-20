#include "sbuffer.h"

#include <stdlib.h>

int sbuffer_init(sbuffer_t *sbuf)
{
    sbuf->head = sbuf->tail = NULL;
    sbuf->size = 0;
    sbuf->shutdown = false;
    for (int r = 0; r < SBUF_READER_COUNT; ++r)
        sbuf->cursor[r] = NULL;

    int rc = pthread_mutex_init(&sbuf->lock, NULL);
    if (rc != 0)
        return rc;
    rc = pthread_cond_init(&sbuf->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&sbuf->lock);
        return rc;
    }
    return 0;
}

/* Called after all threads are joined, so no locking is needed. */
void sbuffer_destroy(sbuffer_t *sbuf)
{
    sbuffer_node_t *node = sbuf->head;
    while (node != NULL) {
        sbuffer_node_t *next = node->next;
        free(node);
        node = next;
    }
    sbuf->head = sbuf->tail = NULL;
    sbuf->size = 0;
    pthread_cond_destroy(&sbuf->not_empty);
    pthread_mutex_destroy(&sbuf->lock);
}

int sbuffer_insert(sbuffer_t *sbuf, const sensor_data_t *data)
{
    /* malloc outside the lock: keep the critical section short */
    sbuffer_node_t *node = malloc(sizeof *node);
    if (node == NULL)
        return -1;
    node->data = *data;
    node->read_by_data = false;
    node->read_by_storage = false;
    node->next = NULL;

    pthread_mutex_lock(&sbuf->lock);
    if (sbuf->shutdown) {
        pthread_mutex_unlock(&sbuf->lock);
        free(node);
        return -1;
    }
    if (sbuf->tail == NULL)
        sbuf->head = node;
    else
        sbuf->tail->next = node;
    sbuf->tail = node;
    sbuf->size++;
    for (int r = 0; r < SBUF_READER_COUNT; ++r)
        if (sbuf->cursor[r] == NULL)
            sbuf->cursor[r] = node;
    /* broadcast, not signal: the two readers wait for different nodes */
    pthread_cond_broadcast(&sbuf->not_empty);
    pthread_mutex_unlock(&sbuf->lock);
    return 0;
}

sbuf_result_t sbuffer_read(sbuffer_t *sbuf, sbuf_reader_t reader,
                           sensor_data_t *out)
{
    if ((int)reader < 0 || reader >= SBUF_READER_COUNT)
        return SBUF_ERR;

    sbuffer_node_t *to_free = NULL;

    pthread_mutex_lock(&sbuf->lock);
    /* while, not if: pthread_cond_wait() may wake up spuriously */
    while (sbuf->cursor[reader] == NULL && !sbuf->shutdown)
        pthread_cond_wait(&sbuf->not_empty, &sbuf->lock);

    sbuffer_node_t *node = sbuf->cursor[reader];
    if (node == NULL) {                    /* shut down and drained */
        pthread_mutex_unlock(&sbuf->lock);
        return SBUF_SHUTDOWN;
    }

    *out = node->data;
    if (reader == SBUF_READER_DATA)
        node->read_by_data = true;
    else
        node->read_by_storage = true;
    sbuf->cursor[reader] = node->next;

    /* Both readers consume in FIFO order, so a node read by both has every
     * older node already removed: it is always the head. */
    if (node->read_by_data && node->read_by_storage) {
        sbuf->head = node->next;
        if (sbuf->tail == node)
            sbuf->tail = NULL;
        sbuf->size--;
        to_free = node;
    }
    pthread_mutex_unlock(&sbuf->lock);

    free(to_free);
    return SBUF_OK;
}

void sbuffer_shutdown(sbuffer_t *sbuf)
{
    pthread_mutex_lock(&sbuf->lock);
    sbuf->shutdown = true;
    pthread_cond_broadcast(&sbuf->not_empty);
    pthread_mutex_unlock(&sbuf->lock);
}

size_t sbuffer_size(sbuffer_t *sbuf)
{
    pthread_mutex_lock(&sbuf->lock);
    size_t n = sbuf->size;
    pthread_mutex_unlock(&sbuf->lock);
    return n;
}
