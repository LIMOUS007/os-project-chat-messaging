/*
 * receiver.h — the client's receiver thread (SPEC §7.2, §5 step 2).
 *
 * It loops on recv_frame(): prints pushed messages, ACKs (and MARK_SEENs)
 * every MSG, formats replies to the current command, and wakes the input
 * thread through the notify pipe:
 *     'R' = reply received        'E' = error reply received
 *     'Q' = the server closed (SHUTDOWN or EOF) -> the client should exit
 *
 * Shared between the two client threads:
 *   - the socket, for sending: protected by send_mutex (client_send);
 *   - the pending-command context: protected by ctx_mutex. The notify byte
 *     only WAKES the input thread; the data itself is passed under the mutex.
 */
#ifndef RECEIVER_H
#define RECEIVER_H

#include <stdbool.h>

#include "commands.h"
#include "protocol.h"

/* Remembers the pipe used to wake the input thread. Call once. */
void receiver_init(int notify_fd);

/* Starts the receiver thread for a new connection. Resets the duplicate
 * filter and all per-connection state first. */
void receiver_start(int fd, const char *me);

/* Tells the receiver the input thread is disconnecting on purpose (so EOF is
 * not reported as "connection lost"). Locks: ctx_mutex. */
void receiver_stopping(void);

/* Joins the receiver thread (after shutdown(fd) woke it). */
void receiver_join(void);

/* Sends a frame on the connection. Locks: send_mutex. Returns 0 / -1. */
int client_send(const frame_builder_t *fb);

/* Records the command now waiting for a reply. Locks: ctx_mutex. */
void pending_set(const cmd_t *cmd);

/* Forgets the waiting command (timeout / disconnect). Locks: ctx_mutex. */
void pending_clear(void);

/* True if the server ended the connection (SHUTDOWN or EOF).
 * Locks: ctx_mutex. */
bool receiver_server_closed(void);

#endif
