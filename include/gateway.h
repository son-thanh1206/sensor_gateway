/* gateway.h - data structures of the gateway components (assignment section 3.9) */
#ifndef GATEWAY_H
#define GATEWAY_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "protocol_adapter.h"
#include "sensor_config.h"
#include "sensor_types.h"

#define RX_BUF_SIZE  256
#define MAX_CONN      64
#define MAX_SENSORS   64
typedef enum { STATE_COLLECTING, STATE_NORMAL, STATE_ALERT } sensor_state_t;
typedef enum { SQL_CONNECTED, SQL_LOST, SQL_DOWN } sql_state_t;

typedef struct {              /* one line of the room-sensor file */
    uint16_t      room;
    sensor_id_t   id;
    sensor_type_t type;
} sensor_info_t;

typedef struct sbuffer_node {
    sensor_data_t        data;
    bool                 read_by_data;
    bool                 read_by_storage;
    struct sbuffer_node *next;
} sbuffer_node_t;

typedef struct {
    sbuffer_node_t *head, *tail;
    size_t          size;
    bool            shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} sbuffer_t;

typedef struct {
    int                     fd;
    const protocol_adapter_t *adapter;
    uint8_t                 rx[RX_BUF_SIZE];
    size_t                  rx_len;
    bool                    identified;       /* sensor ID is known */
    sensor_id_t             sensor_id;
} connection_t;

typedef struct {
    int          listen_fd[4];                /* ports P .. P+3 */
    connection_t conns[MAX_CONN];
    size_t       n_conns;
} connmgr_t;

typedef struct {
    sensor_info_t  info;
    double         window[RUN_AVG_LENGTH];    /* ring buffer */
    size_t         count, next;
    double         running_avg;
    sensor_state_t state;
} datamgr_sensor_t;

typedef struct { datamgr_sensor_t sensors[MAX_SENSORS]; size_t n; } datamgr_t;
typedef void (*sensor_handler_t)(datamgr_t *, datamgr_sensor_t *, const sensor_data_t *);

/* TODO(Step 6): replace these forward declarations with #include <sqlite3.h> */
typedef struct sqlite3      sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef struct {
    sqlite3      *db;
    sqlite3_stmt *insert;
    sql_state_t   state;
    int           attempts;
    const char   *db_path;
} storagemgr_t;

typedef struct {
    sensor_id_t id;  
    uint16_t room;  
    sensor_type_t type;
    double last_value, running_avg;  
    bool has_avg;
    sensor_state_t state;  
    bool connected;  
    time_t last_update;
} sensor_status_t;

typedef struct {
    pthread_mutex_t lock;                       /* PROCESS_SHARED + ROBUST */
    uint64_t        received, processed, stored;
    uint32_t        sbuffer_size;
    sql_state_t     sql_state;  uint8_t sql_retries;
    time_t          start_time;
    uint16_t        sensor_count;
    sensor_status_t sensors[MAX_SENSORS];
} gateway_status_t;

typedef struct {
    uint16_t          base_port, http_port;
    sbuffer_t         sbuf;
    connmgr_t         conn;
    datamgr_t         data;
    storagemgr_t      stor;
    gateway_status_t *status;                   /* NULL until Step 8 */
    pid_t             log_pid, web_pid;
    volatile sig_atomic_t stop;                 /* received signal number, 0 = none */

    /* stop is written only by the signal handler and read only by the main */
    /* thread. The worker threads wait on stop_requested under stop_lock:   */
    /* reading stop unlocked from several threads is a data race, and a     */
    /* condition variable cannot wait on it.                                */
    pthread_mutex_t   stop_lock;
    pthread_cond_t    stop_cond;
    bool              stop_requested;
} gateway_t;

int  gateway_sync_init(gateway_t *gateway);
void gateway_sync_destroy(gateway_t *gateway);
void gateway_request_stop(gateway_t *gateway);
void gateway_wait_for_stop(gateway_t *gateway);
bool gateway_should_stop(gateway_t *gateway);

#endif /* GATEWAY_H */
