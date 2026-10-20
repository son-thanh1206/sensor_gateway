#include "storagemgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

void *storagemgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_STOR, "Storage manager started");

    /* TODO(Step 6): INSERT into SQLite instead of printing. */
    sensor_data_t d;
    while (sbuffer_read(&g->sbuf, SBUF_READER_STORAGE, &d) == SBUF_OK)
        DEBUG_PRINT("storagemgr: id=%u type=%d value=%.2f ts=%lld",
                    d.id, (int)d.type, d.value, (long long)d.ts);

    DEBUG_PRINT("storagemgr: exiting");
    return NULL;
}