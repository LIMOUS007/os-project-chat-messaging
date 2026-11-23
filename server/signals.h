/*
 * signals.h — SIGINT/SIGTERM handling with a dedicated thread (SPEC §6.9).
 *
 * Instead of a signal handler (which may only call async-signal-safe
 * functions), SIGINT and SIGTERM are blocked in every thread and one ordinary
 * thread waits for them with sigwait(). When one arrives it just sets the
 * shutdown flag and wakes the event loop, which runs the shutdown sequence.
 */
#ifndef SIGNALS_H
#define SIGNALS_H

#include <stdbool.h>

/* Blocks SIGINT and SIGTERM and ignores SIGPIPE. Must be called in main
 * BEFORE any thread is created, so every thread inherits the mask. */
void signals_block(void);

/* Starts the signal thread. */
void signals_start(void);

/* True once SIGINT or SIGTERM was received. Atomic: no lock needed. */
bool shutdown_requested(void);

/* Name of the signal that caused the shutdown ("SIGINT" / "SIGTERM"). */
const char *shutdown_signal_name(void);

/* Makes the signal thread return if it is still waiting, then joins it. */
void signals_stop_and_join(void);

#endif
