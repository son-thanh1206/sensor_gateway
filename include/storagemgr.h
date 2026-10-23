#ifndef STORAGEMGR_H
#define STORAGEMGR_H

#include <stdbool.h>

#include "gateway.h"

#define STORAGE_DB_PATH       "gateway.db"
#define STORAGE_TABLE         "SensorData"
#define STOR_MAX_ATTEMPTS     3
#define STOR_RETRY_DELAY_S    5
#define STOR_BUSY_TIMEOUT_MS  1000      /* wait for a lock held by another process */

/* Only sets the fields: the database is opened by the storage manager thread. */
void storagemgr_init(storagemgr_t *st, const char *db_path);

/* One connection attempt: opens the database, creates the table if missing
 * and prepares the INSERT. create allows creating a missing database file.
 * Logs the outcome. Returns 0, or -1 with nothing left open. */
int  storagemgr_connect(storagemgr_t *st, bool create);

/* Returns 0, or -1 when the row could not be written (connection lost). */
int  storagemgr_insert(storagemgr_t *st, const sensor_data_t *d);

/* Safe to call when not connected. */
void storagemgr_disconnect(storagemgr_t *st);

void *storagemgr_thread(void *arg);   /* arg: gateway_t * */

#endif /* STORAGEMGR_H */
