#include "gateway.h"

int gateway_sync_init(gateway_t *gateway)
{
    int rc = pthread_mutex_init(&gateway->stop_lock, NULL);
    if (rc != 0)
        return rc;
    rc = pthread_cond_init(&gateway->stop_cond, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&gateway->stop_lock);
        return rc;
    }
    gateway->stop_requested = false;
    return 0;
}

void gateway_sync_destroy(gateway_t *gateway)
{
    pthread_cond_destroy(&gateway->stop_cond);
    pthread_mutex_destroy(&gateway->stop_lock);
}

void gateway_request_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    gateway->stop_requested = true;
    /* broadcast, not signal: several threads may be waiting */
    pthread_cond_broadcast(&gateway->stop_cond);
    pthread_mutex_unlock(&gateway->stop_lock);
}

void gateway_wait_for_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    /* while, not if: pthread_cond_wait() may wake up spuriously */
    while (!gateway->stop_requested)
        pthread_cond_wait(&gateway->stop_cond, &gateway->stop_lock);
    pthread_mutex_unlock(&gateway->stop_lock);
}

bool gateway_should_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    bool stop = gateway->stop_requested;
    pthread_mutex_unlock(&gateway->stop_lock);
    return stop;
}
