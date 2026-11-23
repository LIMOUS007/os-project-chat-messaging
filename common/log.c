/*
 * log.c — timestamped logging. See log.h for the locking rule.
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* print_mutex: several server threads log at the same time; this keeps each
 * line whole. It is only ever held for the duration of one printf. */
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&print_mutex);
    printf("[%s] %s\n", stamp, line);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
}
