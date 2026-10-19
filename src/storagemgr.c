#include "storagemgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

void *storagemgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_STOR, "Storage manager started");
    gateway_wait_for_stop(g);
    DEBUG_PRINT("storagemgr: exiting");
    return NULL;
}