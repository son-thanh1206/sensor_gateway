#include "datamgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

void *datamgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_DATA, "Data manager started");
    gateway_wait_for_stop(g);
    DEBUG_PRINT("datamgr: exiting");
    return NULL;
}