/* connmgr.c - Connection manager: accepts sensor nodes, decodes their frames
 * with the protocol adapter of the port and writes them into the sbuffer.
 *
 * One thread serves every socket with poll(): no thread per connection, so
 * no extra stacks or context switches. The trade-off is that each connection
 * keeps its own receive buffer and parsing state (connection_t).
 */
#include "connmgr.h"
#include "adapters.h"
#include "debug.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define REPLY_MAX        16
#define ACCEPT_RETRY_MS  1000
#define FD_RESERVE       16     /* stdio, log FIFO, listeners, wake pipe, SQLite */

/* Policy for repeated errors: a malformed first frame closes the connection
 * at once; after that, MAX_CONSEC_ERRORS malformed frames in a row close it.
 * A valid frame resets the count, so a noisy link that only corrupts a frame
 * now and then (sensor_node --corrupt) stays connected, while a node that
 * sends nothing but garbage is dropped instead of flooding the log. */
#define MAX_CONSEC_ERRORS  5

/* One adapter per port: ADAPTERS[i] listens on base port + port_offset. */
static const protocol_adapter_t *const ADAPTERS[] = {
    &TBP_ADAPTER,
    &HLP_ADAPTER,
    &EVP_ADAPTER,
    &SGP_ADAPTER,
};
#define N_ADAPTERS (sizeof ADAPTERS / sizeof ADAPTERS[0])
_Static_assert(N_ADAPTERS <= N_PORTS, "more adapters than listening ports");

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }
    /* Allows a restart while old connections are still in TIME_WAIT. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == -1)
        perror("setsockopt SO_REUSEADDR");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* nodes may be remote */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        fprintf(stderr, "bind port %u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    /* Non-blocking: a client that disconnects between poll() and accept()
     * must not block the whole thread in accept(). */
    if (listen(fd, SOMAXCONN) == -1 || set_nonblocking(fd) == -1) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int connmgr_init(connmgr_t *cm, uint16_t base_port)
{
    cm->n_conns = 0;
    cm->accept_paused = false;
    for (size_t i = 0; i < N_PORTS; ++i) {
        cm->listen_fd[i] = -1;
        cm->listen_adapter[i] = NULL;
    }
    cm->wake_fd[0] = cm->wake_fd[1] = -1;

    /* Keep FD_RESERVE descriptors free for everything else: if sensor nodes
     * took the last one, SQLite could not open its journal and the storage
     * manager would stop the whole gateway. */
    struct rlimit rl;
    cm->max_conns = SIZE_MAX;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        cm->max_conns = rl.rlim_cur > FD_RESERVE ? rl.rlim_cur - FD_RESERVE : 0;
    DEBUG_PRINT("connmgr: at most %zu connections", cm->max_conns);

    cm->cap_conns = MAX_CONN;
    cm->conns = malloc(cm->cap_conns * sizeof *cm->conns);
    if (cm->conns == NULL) {
        perror("malloc connections");
        return -1;
    }

    if (pipe(cm->wake_fd) == -1) {
        perror("pipe");
        connmgr_cleanup(cm);
        return -1;
    }
    /* The write end must never block connmgr_wakeup(). */
    if (set_nonblocking(cm->wake_fd[0]) == -1 ||
        set_nonblocking(cm->wake_fd[1]) == -1) {
        perror("fcntl wake pipe");
        connmgr_cleanup(cm);
        return -1;
    }

    for (size_t i = 0; i < N_ADAPTERS; ++i) {
        uint16_t port = (uint16_t)(base_port + ADAPTERS[i]->port_offset);
        cm->listen_fd[i] = open_listener(port);
        if (cm->listen_fd[i] == -1) {
            connmgr_cleanup(cm);
            return -1;
        }
        cm->listen_adapter[i] = ADAPTERS[i];
        DEBUG_PRINT("connmgr: %s listening on port %u", ADAPTERS[i]->name, port);
    }
    return 0;
}

void connmgr_wakeup(connmgr_t *cm)
{
    if (cm->wake_fd[1] == -1)
        return;
    char c = 'x';
    /* EAGAIN means the pipe already holds a wake-up byte: fine. */
    ssize_t w;
    do {
        w = write(cm->wake_fd[1], &c, 1);
    } while (w == -1 && errno == EINTR);
}

void connmgr_cleanup(connmgr_t *cm)
{
    /* Normally empty: connmgr_thread() closes its connections on exit. */
    for (size_t i = 0; i < cm->n_conns; ++i)
        close(cm->conns[i].fd);
    cm->n_conns = 0;
    for (size_t i = 0; i < N_PORTS; ++i) {
        if (cm->listen_fd[i] != -1)
            close(cm->listen_fd[i]);
        cm->listen_fd[i] = -1;
    }
    for (int i = 0; i < 2; ++i) {
        if (cm->wake_fd[i] != -1)
            close(cm->wake_fd[i]);
        cm->wake_fd[i] = -1;
    }
    free(cm->conns);
    cm->conns = NULL;
    cm->cap_conns = 0;
}

/* Removes conns[idx] by moving the last connection into its slot. */
static void close_conn(connmgr_t *cm, size_t idx)
{
    connection_t *c = &cm->conns[idx];
    if (c->identified)
        printlog(LOG_SRC_CONN,
                 "The sensor node with %u has closed the connection",
                 c->sensor_id);
    else
        DEBUG_PRINT("connmgr: unidentified connection fd %d closed", c->fd);
    close(c->fd);
    cm->conns[idx] = cm->conns[--cm->n_conns];
    cm->accept_paused = false;
}

/* Makes room for one more connection. Returns 0 or -1. */
static int reserve_conn(connmgr_t *cm)
{
    if (cm->n_conns < cm->cap_conns)
        return 0;
    size_t cap = cm->cap_conns * 2;
    connection_t *conns = realloc(cm->conns, cap * sizeof *conns);
    if (conns == NULL)
        return -1;
    cm->conns = conns;
    cm->cap_conns = cap;
    DEBUG_PRINT("connmgr: capacity raised to %zu connections", cap);
    return 0;
}

static void accept_all(connmgr_t *cm, int listen_fd,
                       const protocol_adapter_t *adapter)
{
    while (!cm->accept_paused) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;                       /* backlog drained */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* The pending connection stays in the backlog, so poll()
                 * would report the listener readable again at once: stop
                 * polling the listeners until a descriptor is freed. */
                cm->accept_paused = true;
                printlog(LOG_SRC_CONN,
                         "Connection attempt failed on %s port (too many connections)",
                         adapter->name);
                return;
            }
            printlog(LOG_SRC_CONN,
                     "Connection attempt failed on %s port (accept error)",
                     adapter->name);
            return;
        }
        if (cm->n_conns >= cm->max_conns || reserve_conn(cm) == -1) {
            close(fd);
            printlog(LOG_SRC_CONN,
                     "Connection attempt failed on %s port (too many connections)",
                     adapter->name);
            continue;
        }
        /* fd from accept() does not inherit O_NONBLOCK; it stays blocking,
         * and recv() is only called after poll() reported it readable. */
        connection_t *c = &cm->conns[cm->n_conns++];
        c->fd         = fd;
        c->adapter    = adapter;
        c->rx_len     = 0;
        c->identified    = false;
        c->sensor_id     = 0;
        c->consec_errors = 0;
        c->has_last      = false;
        c->last_ts       = 0;
        c->last_value    = 0.0;
        DEBUG_PRINT("connmgr: accepted fd %d on %s port", fd, adapter->name);
    }
}

static void send_reply(connection_t *c, parse_result_t result)
{
    uint8_t reply[REPLY_MAX];
    size_t n = c->adapter->build_reply(result, reply, sizeof reply);
    if (n == 0)
        return;
    /* MSG_NOSIGNAL: a node that already left must not raise SIGPIPE. */
    if (send(c->fd, reply, n, MSG_NOSIGNAL) == -1)
        DEBUG_PRINT("connmgr: send reply: %s", strerror(errno));
}

/* Reads what is available and decodes every complete frame.
 * Returns false if the connection must be closed. */
static bool handle_readable(gateway_t *g, connection_t *c)
{
    size_t space = RX_BUF_SIZE - c->rx_len;
    if (space == 0)                 /* cannot happen: frames < RX_BUF_SIZE */
        return false;

    ssize_t n = recv(c->fd, c->rx + c->rx_len, space, 0);
    if (n == 0)
        return false;               /* peer closed the connection */
    if (n == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        DEBUG_PRINT("connmgr: recv fd %d: %s", c->fd, strerror(errno));
        return false;
    }
    c->rx_len += (size_t)n;

    /* TCP is a byte stream: one recv() may hold half a frame or several
     * frames, so parse until the adapter needs more bytes. */
    size_t off = 0;
    bool keep = true;
    while (off < c->rx_len) {
        size_t left = c->rx_len - off;
        size_t consumed = 0;
        sensor_data_t data;
        parse_result_t r = c->adapter->parse(c->rx + off, left, &data, &consumed);
        if (r == PARSE_NEED_MORE)
            break;
        if (consumed == 0 || consumed > left)   /* guard against bad adapter */
            consumed = (r == PARSE_OK) ? left : 1;
        off += consumed;
        send_reply(c, r);

        if (r == PARSE_OK) {
            c->consec_errors = 0;
            if (!c->identified) {
                /* The sensor ID is only known after the first valid frame. */
                c->identified = true;
                c->sensor_id  = data.id;
                printlog(LOG_SRC_CONN,
                         "A sensor node with %u has opened a new connection",
                         data.id);
            }
            /* SGP resends a frame whose ACK was lost or late: the copy has
             * the same id and ts. It was ACKed again above but is not stored
             * twice. ts has a 1 s resolution, so the value is compared too:
             * a sensor sending faster than 1 Hz must not lose real readings. */
            if (c->adapter == &SGP_ADAPTER && c->has_last &&
                data.id == c->sensor_id && data.ts == c->last_ts &&
                data.value == c->last_value) {
                DEBUG_PRINT("connmgr: duplicate SGP frame from sensor %u ignored",
                            data.id);
                continue;
            }
            c->has_last   = true;
            c->last_ts    = data.ts;
            c->last_value = data.value;
            if (sbuffer_insert(&g->sbuf, &data) != 0)
                fprintf(stderr, "connmgr: sbuffer_insert failed, frame dropped\n");
        } else if (!c->identified) {
            printlog(LOG_SRC_CONN,
                     "Connection attempt failed on %s port (malformed first frame)",
                     c->adapter->name);
            keep = false;
            break;
        } else {
            printlog(LOG_SRC_CONN, "Received malformed frame on %s port (%s)",
                     c->adapter->name, adapter_last_error());
            DEBUG_PRINT("connmgr: malformed %s frame from sensor %u, %zu bytes dropped",
                        c->adapter->name, c->sensor_id, consumed);
            if (++c->consec_errors >= MAX_CONSEC_ERRORS) {
                DEBUG_PRINT("connmgr: %u malformed frames in a row from sensor %u, closing",
                            c->consec_errors, c->sensor_id);
                keep = false;
                break;
            }
        }
    }

    memmove(c->rx, c->rx + off, c->rx_len - off);
    c->rx_len -= off;
    return keep;
}

void *connmgr_thread(void *arg)
{
    gateway_t *g  = (gateway_t *)arg;
    connmgr_t *cm = &g->conn;

    printlog(LOG_SRC_CONN, "Connection manager started");

    struct pollfd *pfds = NULL;
    size_t         pfds_cap = 0;
    size_t         listen_idx[N_PORTS];   /* pfds slot -> listen_fd index */

    while (!gateway_should_stop(g)) {
        /* Sized from cap_conns, which only grows inside accept_all(). */
        if (pfds_cap < 1 + N_PORTS + cm->cap_conns) {
            size_t cap = 1 + N_PORTS + cm->cap_conns;
            struct pollfd *p = realloc(pfds, cap * sizeof *p);
            if (p == NULL) {
                gateway_fail(g, "out of memory");
                break;
            }
            pfds = p;
            pfds_cap = cap;
        }

        size_t n = 0;
        pfds[n++] = (struct pollfd){ .fd = cm->wake_fd[0], .events = POLLIN };

        size_t first_listen = n;
        for (size_t i = 0; i < N_PORTS; ++i) {
            if (cm->listen_fd[i] == -1 || cm->accept_paused)
                continue;
            listen_idx[n - first_listen] = i;
            pfds[n++] = (struct pollfd){ .fd = cm->listen_fd[i], .events = POLLIN };
        }
        size_t n_listen = n - first_listen;

        size_t first_conn = n;
        size_t n_conns = cm->n_conns;
        for (size_t i = 0; i < n_conns; ++i)
            pfds[n++] = (struct pollfd){ .fd = cm->conns[i].fd, .events = POLLIN };

        /* Infinite timeout: the thread sleeps until a socket is ready or
         * connmgr_wakeup() writes to the pipe; no periodic polling. While
         * accepting is paused, retry after a while in case the descriptors
         * were used up by something other than our connections. */
        int timeout = cm->accept_paused ? ACCEPT_RETRY_MS : -1;
        int ready = poll(pfds, n, timeout);
        if (ready == -1) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (ready == 0) {
            cm->accept_paused = false;
            continue;
        }
        if (pfds[0].revents != 0)
            break;                              /* stop requested */

        /* Backwards: close_conn() moves the last connection into the freed
         * slot, and that one has already been handled. */
        for (size_t i = n_conns; i-- > 0;) {
            short re = pfds[first_conn + i].revents;
            if (re == 0)
                continue;
            bool keep = (re & (POLLIN | POLLHUP | POLLERR))
                            ? handle_readable(g, &cm->conns[i])
                            : false;            /* POLLNVAL */
            if (!keep)
                close_conn(cm, i);
        }

        /* After the connection loop: accepting appends to conns[]. */
        for (size_t k = 0; k < n_listen; ++k) {
            if (pfds[first_listen + k].revents & POLLIN) {
                size_t i = listen_idx[k];
                accept_all(cm, cm->listen_fd[i], cm->listen_adapter[i]);
            }
        }
    }

    /* Close the remaining connections here, while the log is still up, so
     * every "opened" line in gateway.log has its matching "closed" line. */
    while (cm->n_conns > 0)
        close_conn(cm, cm->n_conns - 1);
    free(pfds);

    DEBUG_PRINT("connmgr: exiting");
    return NULL;
}
