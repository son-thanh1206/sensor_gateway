/* protocol_adapter.h - cung cấp sẵn */
#ifndef PROTOCOL_ADAPTER_H
#define PROTOCOL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include "sensor_types.h"

typedef enum {
    PARSE_ERROR     = -1,   /* frame hỏng: sai magic, độ dài, CRC, giá trị */
    PARSE_NEED_MORE =  0,   /* chưa đủ byte để tạo thành một frame         */
    PARSE_OK        =  1    /* đã giải mã đúng một frame                   */
} parse_result_t;

typedef struct {
    const char *name;         /* "TBP", "HLP", "EVP", "SGP"            */
    uint16_t    port_offset;  /* cổng lắng nghe = cổng gốc + port_offset  */

    /* Giải mã tối đa một frame ở đầu buf.                               */
    /* PARSE_OK:        điền *out, *consumed = số byte đã dùng.          */
    /* PARSE_NEED_MORE: giữ nguyên byte, chờ thêm dữ liệu.               */
    /* PARSE_ERROR:     *consumed = số byte cần bỏ để đồng bộ lại.       */
    parse_result_t (*parse)(const uint8_t *buf, size_t len,
                            sensor_data_t *out, size_t *consumed);

    /* Tạo phản hồi gửi lại sensor; trả 0 nếu giao thức không có phản hồi. */
    size_t (*build_reply)(parse_result_t result, uint8_t *reply, size_t cap);
} protocol_adapter_t;

#endif /* PROTOCOL_ADAPTER_H */
