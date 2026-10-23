#include "storagemgr.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"

static const char TABLE_EXISTS_SQL[] =
    "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '"
    STORAGE_TABLE "'";

static const char CREATE_TABLE_SQL[] =
    "CREATE TABLE IF NOT EXISTS " STORAGE_TABLE " ("
    "id    INTEGER NOT NULL, "
    "type  INTEGER NOT NULL, "
    "value REAL    NOT NULL, "
    "ts    INTEGER NOT NULL)";

static const char INSERT_SQL[] =
    "INSERT INTO " STORAGE_TABLE " (id, type, value, ts) VALUES (?, ?, ?, ?)";

void storagemgr_init(storagemgr_t *st, const char *db_path)
{
    st->db = NULL;
    st->insert = NULL;
    st->state = SQL_DOWN;
    st->attempts = 0;
    st->db_path = db_path;
}

/* sqlite3_open_v2() succeeds on any existing file; reading sqlite_master is
 * what reveals a file that is not a database or is locked by another process.
 * Returns 1 if the table exists, 0 if not, -1 on error. */
static int table_exists(sqlite3 *db)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, TABLE_EXISTS_SQL, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int storagemgr_connect(storagemgr_t *st, bool create)
{
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *insert = NULL;
    int exists = -1;

    if (sqlite3_open_v2(st->db_path, &db, flags, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_busy_timeout(db, STOR_BUSY_TIMEOUT_MS);
    /* A write-protected file is silently opened read-only: every INSERT
     * would then fail, so treat it as a failed connection. */
    if (sqlite3_db_readonly(db, "main") != 0)
        goto fail;
    exists = table_exists(db);
    if (exists == -1)
        goto fail;
    if (sqlite3_exec(db, CREATE_TABLE_SQL, NULL, NULL, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_prepare_v2(db, INSERT_SQL, -1, &insert, NULL) != SQLITE_OK)
        goto fail;

    st->db = db;
    st->insert = insert;
    st->state = SQL_CONNECTED;
    st->attempts = 0;
    printlog(LOG_SRC_STOR, "Connection to SQL server established.");
    if (!exists)
        printlog(LOG_SRC_STOR, "New table " STORAGE_TABLE " created.");
    return 0;

fail:
    DEBUG_PRINT("storagemgr: connect to %s failed: %s", st->db_path,
                db != NULL ? sqlite3_errmsg(db) : "out of memory");
    sqlite3_finalize(insert);
    sqlite3_close(db);
    printlog(LOG_SRC_STOR, "Unable to connect to SQL server.");
    return -1;
}

int storagemgr_insert(storagemgr_t *st, const sensor_data_t *d)
{
    if (st->insert == NULL)
        return -1;

    sqlite3_bind_int(st->insert, 1, d->id);
    sqlite3_bind_int(st->insert, 2, (int)d->type);
    sqlite3_bind_double(st->insert, 3, d->value);
    sqlite3_bind_int64(st->insert, 4, (sqlite3_int64)d->ts);
    int rc = sqlite3_step(st->insert);
    if (rc != SQLITE_DONE)
        DEBUG_PRINT("storagemgr: insert failed (%d): %s",
                    sqlite3_extended_errcode(st->db), sqlite3_errmsg(st->db));
    sqlite3_reset(st->insert);
    return rc == SQLITE_DONE ? 0 : -1;
}

void storagemgr_disconnect(storagemgr_t *st)
{
    sqlite3_finalize(st->insert);
    sqlite3_close(st->db);
    st->insert = NULL;
    st->db = NULL;
}

/* Returns true once connected; false after STOR_MAX_ATTEMPTS failed attempts
 * or when the gateway is stopping. */
static bool connect_with_retry(gateway_t *g, bool create)
{
    storagemgr_t *st = &g->stor;
    for (int attempt = 1; attempt <= STOR_MAX_ATTEMPTS; ++attempt) {
        if (attempt > 1 && gateway_wait_stop(g, STOR_RETRY_DELAY_S))
            return false;
        st->attempts = attempt;
        if (storagemgr_connect(st, create) == 0)
            return true;
    }
    st->state = SQL_DOWN;
    return false;
}

void *storagemgr_thread(void *arg)
{
    gateway_t *g = (gateway_t*)arg;
    storagemgr_t *st = &g->stor;

    printlog(LOG_SRC_STOR, "Storage manager started");

    bool connected = connect_with_retry(g, true);

    /* While reconnecting, d is the only measurement taken out of the sbuffer:
     * the storage cursor does not move, so all later ones stay queued there. */
    sensor_data_t d;
    while (connected &&
           sbuffer_read(&g->sbuf, SBUF_READER_STORAGE, &d) == SBUF_OK) {
        DEBUG_PRINT("storagemgr: id=%u type=%d value=%.2f ts=%lld",
                    d.id, (int)d.type, d.value, (long long)d.ts);
        while (connected && storagemgr_insert(st, &d) != 0) {
            printlog(LOG_SRC_STOR, "Connection to SQL server lost.");
            storagemgr_disconnect(st);
            st->state = SQL_LOST;
            /* No SQLITE_OPEN_CREATE: if the file was moved away, recreating
             * an empty one would silently split the measurement history. */
            connected = connect_with_retry(g, false);
        }
    }

    if (!connected && !gateway_should_stop(g))
        gateway_fail(g, "SQL server unreachable");
    storagemgr_disconnect(st);

    DEBUG_PRINT("storagemgr: exiting");
    return NULL;
}
