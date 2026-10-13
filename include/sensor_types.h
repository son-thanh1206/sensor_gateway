/* sensor_types.h - cung cấp sẵn, không sửa */
#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>
#include <time.h>

typedef uint16_t sensor_id_t;

typedef enum {
    SENSOR_TEMPERATURE = 1,   /* °C                          */
    SENSOR_HUMIDITY    = 2,   /* %RH                         */
    SENSOR_MOTION      = 3,   /* 1 = phát hiện chuyển động   */
    SENSOR_DOOR        = 4,   /* 0 = đóng, 1 = mở            */
    SENSOR_SMOKE_GAS   = 5    /* ppm                         */
} sensor_type_t;

typedef struct {
    sensor_id_t   id;     /* duy nhất trong hệ thống                */
    sensor_type_t type;   /* loại sensor                            */
    double        value;  /* theo đơn vị của loại sensor            */
    time_t        ts;     /* thời điểm đo, giây, UTC                */
} sensor_data_t;

#endif /* SENSOR_TYPES_H */
