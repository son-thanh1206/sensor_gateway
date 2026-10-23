/*
 * test_storagemgr.c - unit tests of the storage manager: connecting, table
 * creation, INSERT and detection of a lost connection (assignment Step 6).
 *
 *   make test-storagemgr
 *
 * printlog() is stubbed here, so the log messages can be checked exactly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"
#include "storagemgr.h"

#define DB_PATH     "test/out/test_storage.db"
#define MOVED_PATH  "test/out/test_storage_moved.db"
#define TEXT_PATH   "test/out/test_storage_not_a_db.txt"

static int failures;

#define CHECK(cond) do {                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

#define MAX_LOGS  8

static int  n_logs;
static char logs[MAX_LOGS][LOG_MSG_MAX + 1];

int printlog(log_src_t src, const char *fmt, ...)
{
    (void)src;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs[n_logs % MAX_LOGS], sizeof logs[0], fmt, ap);
    va_end(ap);
    ++n_logs;
    return 0;
}

static void clear_logs(void)
{
    n_logs = 0;
    memset(logs, 0, sizeof logs);
}

static storagemgr_t st;

static void remove_files(void)
{
    remove(DB_PATH);
    remove(MOVED_PATH);
    remove(TEXT_PATH);
}

static int count_rows(void)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int n = -1;
    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM " STORAGE_TABLE, -1,
                           &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return n;
}

static void test_connect_and_insert(void)
{
    remove_files();
    storagemgr_init(&st, DB_PATH);
    clear_logs();
    CHECK(storagemgr_connect(&st, true) == 0);
    CHECK(st.state == SQL_CONNECTED);
    CHECK(n_logs == 2);
    CHECK(strcmp(logs[0], "Connection to SQL server established.") == 0);
    CHECK(strcmp(logs[1], "New table SensorData created.") == 0);

    sensor_data_t in[] = {
        { .id = 10, .type = SENSOR_TEMPERATURE, .value = 21.5,  .ts = 1767225600 },
        { .id = 40, .type = SENSOR_DOOR,        .value = 1.0,   .ts = 1767225601 },
        { .id = 50, .type = SENSOR_SMOKE_GAS,   .value = 120.0, .ts = 4102444800 },
    };
    for (size_t i = 0; i < sizeof in / sizeof in[0]; ++i)
        CHECK(storagemgr_insert(&st, &in[i]) == 0);
    storagemgr_disconnect(&st);

    sqlite3 *db;
    sqlite3_stmt *stmt;
    CHECK(sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT id, type, value, ts FROM " STORAGE_TABLE
                             " ORDER BY rowid", -1, &stmt, NULL) == SQLITE_OK);
    size_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < sizeof in / sizeof in[0]) {
        CHECK(sqlite3_column_int(stmt, 0) == in[n].id);
        CHECK(sqlite3_column_int(stmt, 1) == (int)in[n].type);
        CHECK(sqlite3_column_double(stmt, 2) == in[n].value);
        CHECK(sqlite3_column_int64(stmt, 3) == (sqlite3_int64)in[n].ts);
        ++n;
    }
    CHECK(n == sizeof in / sizeof in[0]);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_reconnect_keeps_table(void)
{
    clear_logs();
    CHECK(storagemgr_connect(&st, false) == 0);
    CHECK(n_logs == 1);
    CHECK(strcmp(logs[0], "Connection to SQL server established.") == 0);
    CHECK(count_rows() == 3);
    storagemgr_disconnect(&st);
}

static void test_renamed_file(void)
{
    CHECK(storagemgr_connect(&st, false) == 0);
    CHECK(rename(DB_PATH, MOVED_PATH) == 0);

    sensor_data_t d = { .id = 10, .type = SENSOR_TEMPERATURE, .value = 18.0, .ts = 1 };
    CHECK(storagemgr_insert(&st, &d) == -1);
    storagemgr_disconnect(&st);

    clear_logs();
    CHECK(storagemgr_connect(&st, false) == -1);
    CHECK(n_logs == 1);
    CHECK(strcmp(logs[0], "Unable to connect to SQL server.") == 0);
    CHECK(access(DB_PATH, F_OK) == -1);          /* not recreated empty */

    CHECK(rename(MOVED_PATH, DB_PATH) == 0);
    CHECK(storagemgr_connect(&st, false) == 0);
    CHECK(storagemgr_insert(&st, &d) == 0);
    storagemgr_disconnect(&st);
    CHECK(count_rows() == 4);
}

static void test_locked_file(void)
{
    sqlite3 *other;
    CHECK(sqlite3_open(DB_PATH, &other) == SQLITE_OK);
    CHECK(storagemgr_connect(&st, false) == 0);
    CHECK(sqlite3_exec(other, "BEGIN EXCLUSIVE", NULL, NULL, NULL) == SQLITE_OK);

    sensor_data_t d = { .id = 21, .type = SENSOR_HUMIDITY, .value = 50.0, .ts = 2 };
    CHECK(storagemgr_insert(&st, &d) == -1);     /* after STOR_BUSY_TIMEOUT_MS */
    storagemgr_disconnect(&st);
    CHECK(storagemgr_connect(&st, false) == -1);

    CHECK(sqlite3_exec(other, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(other);
    CHECK(storagemgr_connect(&st, false) == 0);
    CHECK(storagemgr_insert(&st, &d) == 0);
    storagemgr_disconnect(&st);
}

static void test_read_only_file(void)
{
    if (geteuid() == 0)                          /* root ignores file modes */
        return;
    CHECK(chmod(DB_PATH, 0444) == 0);
    CHECK(storagemgr_connect(&st, false) == -1);
    CHECK(chmod(DB_PATH, 0644) == 0);
}

static void test_bad_files(void)
{
    remove_files();
    CHECK(storagemgr_connect(&st, false) == -1); /* missing, no create */
    CHECK(access(DB_PATH, F_OK) == -1);

    FILE *f = fopen(TEXT_PATH, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("this is not an SQLite database, just some plain text\n", f);
        fclose(f);
    }
    storagemgr_init(&st, TEXT_PATH);
    CHECK(storagemgr_connect(&st, true) == -1);
    CHECK(st.db == NULL && st.insert == NULL);
    storagemgr_disconnect(&st);                  /* safe when not connected */
}

int main(void)
{
    test_connect_and_insert();
    test_reconnect_keeps_table();
    test_renamed_file();
    test_locked_file();
    test_read_only_file();
    test_bad_files();
    remove_files();

    if (failures == 0)
        printf("test_storagemgr: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
