#ifndef DATAMGR_H
#define DATAMGR_H

#include "gateway.h"

#define ROOM_SENSOR_PATH  "room_sensor.map"

/* Loads the room-sensor file into dm.
 * Returns 0, or -1 after printing the reason to stderr. */
int  datamgr_init(datamgr_t *dm, const char *path);

/* Validates one measurement against the room-sensor table and runs the
 * handler of its sensor type. Called only by the data manager thread. */
void datamgr_process(datamgr_t *dm, const sensor_data_t *d);

void *datamgr_thread(void *arg);   /* arg: gateway_t * */

#endif /* DATAMGR_H */
