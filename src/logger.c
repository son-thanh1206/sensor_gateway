#define _GNU_SOURCE
#include "logger.h"
#include "debug.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_SRC_MAX_LEN  4
/* "<SRC> <msg>\n\0" */
#define LOG_LINE_MAX     (LOG_SRC_MAX_LEN + 1 + LOG_MSG_MAX + 1 + 1)
#define LOG_PROCESS_NAME "gateway_log_process"

/* A single write() of at most PIPE_BUF bytes to a FIFO is atomic
 * (man 7 pipe), so lines from concurrent writers never interleave. */
_Static_assert(LOG_LINE_MAX - 1 <= PIPE_BUF, "log line must fit in PIPE_BUF");

static const char *const SRC_NAME[LOG_SRC_COUNT] = {
    [LOG_SRC_MAIN] = "MAIN",
    [LOG_SRC_CONN] = "CONN",
    [LOG_SRC_DATA] = "DATA",
    [LOG_SRC_STOR] = "STOR",
    [LOG_SRC_WEB]  = "WEB",
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int fifo_fd = -1;

static int ensure_fifo(const char *path)
{
    if (mkfifo(path, 0666) == 0)
        return 0;
    if (errno != EEXIST) {
        perror("mkfifo");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == -1) {
        perror("stat");
        return -1;
    }
    if (!S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "%s exists but is not a FIFO\n", path);
        return -1;
    }
    return 0;
}

static void log_process_run(const char *fifo_path, const char *file_path)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    prctl(PR_SET_NAME, LOG_PROCESS_NAME);

    /* Open the FIFO before the log file: if the log file cannot be opened,
     * the main process is not left blocked in open(); it gets EPIPE. */
    int fd = open(fifo_path, O_RDONLY);
    if (fd == -1) {
        perror("log process: open fifo");
        _exit(EXIT_FAILURE);
    }
    FILE *in = fdopen(fd, "r");
    if (in == NULL) {
        perror("log process: fdopen");
        close(fd);
        _exit(EXIT_FAILURE);
    }
    /* "w": each gateway run starts a new file, so seq restarts at 1. */
    FILE *out = fopen(file_path, "w");
    if (out == NULL) {
        perror("log process: fopen log file");
        fclose(in);
        _exit(EXIT_FAILURE);
    }
    setvbuf(out, NULL, _IOLBF, 0);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    unsigned long seq = 0;

    /* getline() reassembles a line even if read() returns only part of it. */
    while ((n = getline(&line, &cap, in)) != -1) {
        if (n > 0 && line[n - 1] == '\n')
            line[--n] = '\0';
        if (n == 0)
            continue;

        time_t now = time(NULL);
        struct tm tm;
        char ts[32];
        localtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

        fprintf(out, "%lu %s %s\n", ++seq, ts, line);
    }

    free(line);
    fclose(in);
    fclose(out);
    /* _exit, not exit: must not run the parent's atexit handlers or
     * flush stdio buffers inherited from the parent. */
    _exit(EXIT_SUCCESS);
}

pid_t log_start(const char *fifo_path, const char *file_path)
{
    if (ensure_fifo(fifo_path) == -1)
        return -1;

    /* Otherwise unflushed stdout data is duplicated into the child. */
    fflush(NULL);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0)
        log_process_run(fifo_path, file_path);   /* never returns */
        
    /* Blocks until the log process opens the read end. */
    int fd = open(fifo_path, O_WRONLY);
    if (fd == -1) {
        perror("open fifo for writing");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    pthread_mutex_lock(&log_lock);
    fifo_fd = fd;
    pthread_mutex_unlock(&log_lock);

    DEBUG_PRINT("log process started (pid %d)", (int)pid);
    return pid;
}

int printlog(log_src_t src, const char *fmt, ...)
{
    if ((int)src < 0 || src >= LOG_SRC_COUNT)
    {
        perror("Log process: wrong source.");
        return -1;
    }
    
    char msg[LOG_MSG_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    for (char *p = msg; *p != '\0'; ++p)
        if (!isprint((unsigned char)*p))
            *p = ' ';

    char line[LOG_LINE_MAX];
    int len = snprintf(line, sizeof line, "%s %s\n", SRC_NAME[src], msg);

    int rc = -1;
    pthread_mutex_lock(&log_lock);
    if (fifo_fd != -1) {
        ssize_t w;
        do {
            w = write(fifo_fd, line, (size_t)len);
        } while (w == -1 && errno == EINTR);
        rc = (w == len) ? 0 : -1;
    }
    pthread_mutex_unlock(&log_lock);

    if (rc == -1)
        fprintf(stderr, "[log unavailable] %s", line);
    return rc;
}

int log_stop(pid_t log_pid)
{
    pthread_mutex_lock(&log_lock);
    int fd = fifo_fd;
    fifo_fd = -1;
    pthread_mutex_unlock(&log_lock);

    if (fd != -1)
        close(fd);

    int status;
    while (waitpid(log_pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid log process");
            return -1;
        }
    }
    DEBUG_PRINT("log process exited");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}