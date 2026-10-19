#include "connmgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

void *connmgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_CONN, "Connection manager started");
    gateway_wait_for_stop(g);
    DEBUG_PRINT("connmgr: exiting");
    return NULL;
}