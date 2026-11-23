/*
 * log.h — timestamped console logging for the server (SPEC §6.11).
 *
 * Locking: log_event takes its own private print mutex while it writes one
 * line, so lines from different threads never mix. That mutex is a "leaf":
 * project rule — never call log_event while holding any other lock
 * (sessions_mutex, db_mutex, jobq.mutex, expiry_mutex). Copy what you need,
 * unlock, then log.
 */
#ifndef LOG_H
#define LOG_H

/* Prints "[HH:MM:SS] <message>\n" to stdout and flushes. printf-style. */
void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
