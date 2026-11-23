/*
 * users.h — the username whitelist loaded from users.txt (SPEC §8.1).
 *
 * Loaded once at startup, before any thread is created, and never changed
 * afterwards — so reading it needs no lock.
 */
#ifndef USERS_H
#define USERS_H

#include <stdbool.h>
#include <stddef.h>

/* Reads the file: one name per line, blank lines and lines starting with '#'
 * are skipped, surrounding spaces and '\r' are trimmed. An invalid name is an
 * error. Returns 0 on success; on failure returns -1 and writes a message into
 * err. No locks. */
int users_load(const char *path, char *err, size_t err_size);

/* True if name is in the whitelist (binary search in the sorted array). */
bool users_exists(const char *name);

/* Number of loaded users. */
int users_count(void);

/* Frees the list. */
void users_free(void);

#endif
