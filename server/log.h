/* log.h — dead-simple timestamped logging.
 * Each call emits exactly one fprintf so concurrent threads don't interleave
 * within a line (POSIX guarantees stdio calls are atomic per FILE). */
#ifndef PULSELINK_LOG_H
#define PULSELINK_LOG_H

#include <stdio.h>
#include <time.h>

#define LOGF(fmt, ...)                                                      \
    do {                                                                    \
        time_t _t = time(NULL);                                             \
        struct tm _tm;                                                      \
        localtime_r(&_t, &_tm);                                             \
        char _ts[16];                                                       \
        strftime(_ts, sizeof _ts, "%H:%M:%S", &_tm);                        \
        fprintf(stderr, "[%s] " fmt "\n", _ts, ##__VA_ARGS__);             \
    } while (0)

#endif /* PULSELINK_LOG_H */
