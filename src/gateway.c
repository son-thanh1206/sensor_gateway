#define _POSIX_C_SOURCE 200809L

#include "gateway.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

int gateway_sync_init(gateway_t *gateway)
{
    int rc = pthread_mutex_init(&gateway->stop_lock, NULL);
    if (rc != 0)
        return rc;

    /* gateway_wait_stop() uses a monotonic deadline: setting the system
     * clock must not shorten or stretch the wait. */
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc == 0) {
        rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (rc == 0)
            rc = pthread_cond_init(&gateway->stop_cond, &attr);
        pthread_condattr_destroy(&attr);
    }
    if (rc != 0) {
        pthread_mutex_destroy(&gateway->stop_lock);
        return rc;
    }
    gateway->stop_requested = false;
    gateway->fail_reason = NULL;
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

bool gateway_should_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    bool stop = gateway->stop_requested;
    pthread_mutex_unlock(&gateway->stop_lock);
    return stop;
}

bool gateway_wait_stop(gateway_t *gateway, unsigned seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&gateway->stop_lock);
    int rc = 0;
    /* while, not if: pthread_cond_timedwait() may wake up spuriously */
    while (!gateway->stop_requested && rc == 0)
        rc = pthread_cond_timedwait(&gateway->stop_cond, &gateway->stop_lock,
                                    &deadline);
    bool stop = gateway->stop_requested;
    pthread_mutex_unlock(&gateway->stop_lock);
    return stop;
}

void gateway_fail(gateway_t *gateway, const char *reason)
{
    pthread_mutex_lock(&gateway->stop_lock);
    if (gateway->fail_reason == NULL)
        gateway->fail_reason = reason;
    gateway->stop_requested = true;
    pthread_cond_broadcast(&gateway->stop_cond);
    pthread_mutex_unlock(&gateway->stop_lock);

    /* Process-directed: every worker blocks SIGUSR1, so it is delivered to
     * the main thread waiting in sigsuspend(). */
    kill(getpid(), SIGUSR1);
}

const char *gateway_fail_reason(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_lock);
    const char *reason = gateway->fail_reason;
    pthread_mutex_unlock(&gateway->stop_lock);
    return reason;
}
