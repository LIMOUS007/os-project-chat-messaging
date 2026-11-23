/*
 * expiry.h — the F1 expiry thread (SPEC §6.8).
 *
 * The thread sleeps until the earliest expires_at, deletes the expired
 * messages and tells online recipients "[a message from X has disappeared]".
 *
 * Missed wake-ups are prevented with a generation counter: SEND_TIMED bumps
 * expiry_gen AFTER its message is committed. If that happens between the
 * thread's "what is the next expiry?" query and its sleep, the thread sees the
 * counter changed and does not go to sleep with the old (later) deadline.
 */
#ifndef EXPIRY_H
#define EXPIRY_H

/* Starts the expiry thread. */
void expiry_start(void);

/* Called by SEND_TIMED after its transaction commits: bumps expiry_gen and
 * wakes the thread so it re-reads the earliest deadline. Locks: expiry_mutex. */
void expiry_notify(void);

/* Asks the thread to exit (shutdown). Locks: expiry_mutex. */
void expiry_stop(void);

/* Joins the thread (after expiry_stop). */
void expiry_join(void);

#endif
