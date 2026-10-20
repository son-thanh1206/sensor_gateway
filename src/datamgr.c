#include "datamgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

void *datamgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;

    printlog(LOG_SRC_DATA, "Data manager started");

    /* Blocks inside sbuffer_read(); returns SBUF_SHUTDOWN once the buffer
     * is shut down and every remaining measurement has been read. */
    sensor_data_t d;
    while (sbuffer_read(&g->sbuf, SBUF_READER_DATA, &d) == SBUF_OK)
        DEBUG_PRINT("datamgr: id=%u type=%d value=%.2f ts=%lld",
                    d.id, (int)d.type, d.value, (long long)d.ts);

    DEBUG_PRINT("datamgr: exiting");
    return NULL;
}