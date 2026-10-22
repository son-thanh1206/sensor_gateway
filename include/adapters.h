/* adapters.h - protocol adapters registered in the Connection manager */
#ifndef ADAPTERS_H
#define ADAPTERS_H

#include "protocol_adapter.h"

#define TBP_FRAME_LEN    18

#define HLP_MAX_LINE     64       /* including the LF */

#define EVP_HDR_LEN       2       /* type + length */
#define EVP_TYPE_MOTION   0x01
#define EVP_TYPE_DOOR     0x02
#define EVP_LEN_MOTION    6       /* id(2) + ts(4) */
#define EVP_LEN_DOOR      7       /* id(2) + state(1) + ts(4) */

#define SGP_FRAME_LEN    11
#define SGP_MAGIC        0x7E
#define SGP_ACK          0x06
#define SGP_NAK          0x15

extern const protocol_adapter_t TBP_ADAPTER;   /* temperature,  port P   */
extern const protocol_adapter_t HLP_ADAPTER;   /* humidity,     port P+1 */
extern const protocol_adapter_t EVP_ADAPTER;   /* motion, door, port P+2 */
extern const protocol_adapter_t SGP_ADAPTER;   /* smoke/gas,    port P+3 */

/* parse() has no way to say why a frame is malformed, so an adapter that
 * returns PARSE_ERROR first stores the <reason> for the "Received malformed
 * frame" log line here. A single static slot is enough: only the Connection
 * manager thread calls parse(). */
void        adapter_set_error(const char *reason);
const char *adapter_last_error(void);

#endif /* ADAPTERS_H */
