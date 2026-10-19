#ifndef LOG_H
#define LOG_H

#include <sys/types.h>

#define LOG_FIFO_PATH  "logFifo"
#define LOG_FILE_PATH  "gateway.log"
#define LOG_MSG_MAX    200

typedef enum {
    LOG_SRC_MAIN,
    LOG_SRC_CONN,
    LOG_SRC_DATA,
    LOG_SRC_STOR,
    LOG_SRC_WEB,
    LOG_SRC_COUNT
} log_src_t;

pid_t log_start(const char *fifo_path, const char *file_path);

int printlog(log_src_t src, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

int log_stop(pid_t log_pid);

#endif /* LOG_H */