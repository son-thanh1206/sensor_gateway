#ifndef CONNMGR_H
#define CONNMGR_H

#include <stdint.h>

#include "gateway.h"

/* Opens the listening sockets and the wake-up pipe. Returns 0 or -1. */
int   connmgr_init(connmgr_t *cm, uint16_t base_port);

/* Wakes the thread blocked in poll() so it notices the stop request. */
void  connmgr_wakeup(connmgr_t *cm);

/* Closes every socket and the pipe. Call after the thread is joined. */
void  connmgr_cleanup(connmgr_t *cm);

void *connmgr_thread(void *arg);   /* arg: gateway_t * */

#endif /* CONNMGR_H */
